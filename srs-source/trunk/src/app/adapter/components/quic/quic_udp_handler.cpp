#include "quic_udp_handler.hpp"
#include "../../core/adapter_manager.hpp"
#include "../../core/adapter_stats.hpp"
#include "quic_session_wrapper.hpp"
#include "../../core/iadapter.hpp"
#include <srs_core_time.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_utility.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sstream>
#include <cstring>

// QUIC datagram接收回调函数（C++98兼容）
static void on_quic_datagram_received(void* ctx, const uint8_t* data, size_t size, int64_t timestamp_ms) {
    IAdapter* adapter = static_cast<IAdapter*>(ctx);
    if (adapter && data && size > 0) {
        // 将QUIC datagram中的数据传递给adapter
        srs_error_t err = adapter->feed(data, size);
        if (err != srs_success) {
            srs_warn("Adapter feed datagram failed: %s", srs_error_desc(err).c_str());
            srs_freep(err);
            return;
        }
        
        // 触发帧解析
        err = adapter->parseFrame();
        if (err != srs_success) {
            srs_warn("Adapter parseFrame failed: %s", srs_error_desc(err).c_str());
            srs_freep(err);
        }
    }
}

QuicUdpHandler::QuicUdpHandler(const std::string& protocol_name)
    : protocol_name_(protocol_name), adapter_timeout_ms_(300000) { // 5分钟超时
    fixed_route_.vhost = "__defaultVhost__";
    fixed_route_.app = "live";
    fixed_route_.stream = "stream";
    
    // 默认QUIC证书路径（可以从配置读取）
    quic_cert_file_ = "";  // 需要配置
    quic_key_file_ = "";   // 需要配置
}

QuicUdpHandler::~QuicUdpHandler() {
    std::lock_guard<std::mutex> lock(adapters_mutex_);
    adapters_.clear();
}

std::string QuicUdpHandler::get_connection_id(const sockaddr* from, int fromlen) {
    if (fromlen < (int)sizeof(sockaddr_in)) {
        return "";
    }
    
    const sockaddr_in* addr = (const sockaddr_in*)from;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr->sin_addr, ip_str, INET_ADDRSTRLEN);
    
    std::stringstream ss;
    ss << ip_str << ":" << ntohs(addr->sin_port);
    return ss.str();
}

QuicUdpHandler::Route QuicUdpHandler::get_route_for_port(int port) {
    auto it = port_mapping_.find(port);
    if (it != port_mapping_.end()) {
        return it->second;
    }
    return fixed_route_;
}

IAdapter* QuicUdpHandler::get_or_create_adapter(const std::string& connection_id,
                                                 const std::string& client_ip, 
                                                 int client_port) {
    std::lock_guard<std::mutex> lock(adapters_mutex_);
    
    // 查找现有adapter
    auto it = adapters_.find(connection_id);
    if (it != adapters_.end()) {
        adapter_last_activity_[connection_id] = srs_time_now_cached() / 1000;
        return it->second.get();
    }
    
    // 创建新adapter
    IAdapter* adapter = AdapterManager::instance().create(protocol_name_);
    if (!adapter) {
        srs_error("Failed to create adapter for connection: %s", connection_id.c_str());
        return nullptr;
    }
    
    // 配置初始化（使用固定路由或端口映射）
    AdapterInit init;
    Route route = get_route_for_port(client_port);
    init.vhost = route.vhost;
    init.app = route.app;
    init.stream = route.stream;
    
    // 传递连接ID和客户端信息到adapter
    init.set_param("connection_id", connection_id);
    init.set_param("client_ip", client_ip);
    init.set_param("client_port", std::to_string(client_port));
    
    // 启动adapter
    srs_error_t err = adapter->start(init);
    if (err != srs_success) {
        srs_error("Failed to start adapter for connection %s: %s", 
                 connection_id.c_str(), srs_error_desc(err).c_str());
        srs_freep(err);
        delete adapter;
        return nullptr;
    }
    
    // 保存adapter实例
    adapters_[connection_id] = std::unique_ptr<IAdapter>(adapter);
    adapter_last_activity_[connection_id] = srs_time_now_cached() / 1000;
    
    // 记录连接统计
    AdapterStatsManager::instance().add_connection(
        connection_id, protocol_name_, init.vhost, init.app, init.stream,
        client_ip, client_port);
    
    srs_trace("Created adapter for connection: %s -> %s/%s/%s", 
             connection_id.c_str(), init.vhost.c_str(), init.app.c_str(), init.stream.c_str());
    
    return adapter;
}

srs_error_t QuicUdpHandler::on_udp_packet(const sockaddr *from, const int fromlen, 
                                           char *buf, int nb_buf) {
    srs_error_t err = srs_success;
    
    if (nb_buf <= 0 || buf == nullptr) {
        return srs_success;
    }
    
    // 获取连接标识
    std::string connection_id = get_connection_id(from, fromlen);
    if (connection_id.empty()) {
        return srs_error_new(ERROR_SYSTEM_IP_INVALID, "invalid connection id");
    }
    
    // 解析客户端信息
    char address_string[64];
    char port_string[16];
    if (getnameinfo(from, fromlen,
                    (char *)&address_string, sizeof(address_string),
                    (char *)&port_string, sizeof(port_string),
                    NI_NUMERICHOST | NI_NUMERICSERV)) {
        return srs_error_new(ERROR_SYSTEM_IP_INVALID, "bad address");
    }
    std::string client_ip = std::string(address_string);
    int client_port = atoi(port_string);
    
    // 获取或创建adapter
    IAdapter* adapter = get_or_create_adapter(connection_id, client_ip, client_port);
    if (!adapter) {
        return srs_error_new(ERROR_RTMP_MESSAGE_CREATE, "get or create adapter failed");
    }
    
    // 获取或创建QUIC会话，将UDP数据包传递给QUIC库解析
    QuicSessionWrapper* quic_session = get_or_create_quic_session(connection_id, adapter);
    if (quic_session) {
        // 通过QUIC库解析UDP数据包
        if ((err = quic_session->on_udp_packet(from, (const uint8_t*)buf, nb_buf)) != srs_success) {
            return srs_error_wrap(err, "quic session process packet");
        }
        
        // 处理QUIC连接事件
        if ((err = quic_session->process_connections()) != srs_success) {
            return srs_error_wrap(err, "quic process connections");
        }
    } else {
        // 如果QUIC库未启用或创建失败，fallback到直接传递UDP数据包
        // 将UDP数据包传递给adapter（保持向后兼容）
        static uint64_t feed_count = 0;
        feed_count++;
        if (feed_count <= 5 || feed_count % 50 == 0) {
            srs_trace("QuicUdpHandler: feed to adapter, count=%llu, size=%d", feed_count, nb_buf);
        }
        if ((err = adapter->feed((const uint8_t*)buf, nb_buf)) != srs_success) {
            return srs_error_wrap(err, "adapter feed");
        }
        
        // 触发帧解析（如果需要）
        if ((err = adapter->parseFrame()) != srs_success) {
            return srs_error_wrap(err, "adapter parseFrame");
        }
    }
    
    // 定期清理过期adapter（每100个包清理一次，避免频繁检查）
    static uint64_t packet_count = 0;
    if (++packet_count % 100 == 0) {
        cleanup_expired_adapters();
    }
    
    return err;
}

void QuicUdpHandler::set_fixed_route(const Route& r) {
    fixed_route_ = r;
}

void QuicUdpHandler::add_port_mapping(int port, const Route& r) {
    port_mapping_[port] = r;
}

QuicSessionWrapper* QuicUdpHandler::get_or_create_quic_session(const std::string& connection_id,
                                                                IAdapter* adapter) {
    std::lock_guard<std::mutex> lock(quic_sessions_mutex_);
    
    // 查找现有QUIC会话
    auto it = quic_sessions_.find(connection_id);
    if (it != quic_sessions_.end()) {
        return it->second.get();
    }
    
    // 创建新的QUIC会话
    std::unique_ptr<QuicSessionWrapper> session(new QuicSessionWrapper());
    
    // 设置datagram接收回调，将QUIC datagram传递给adapter（使用函数指针，兼容C++98）
    session->set_datagram_callback(on_quic_datagram_received, adapter);
    
    // 初始化QUIC服务器（如果有证书文件）
    if (!quic_cert_file_.empty() && !quic_key_file_.empty()) {
        srs_error_t err = session->init_server(quic_cert_file_, quic_key_file_);
        if (err != srs_success) {
            srs_warn("Failed to init QUIC session for %s: %s", 
                    connection_id.c_str(), srs_error_desc(err).c_str());
            srs_freep(err);
            // 如果初始化失败，返回nullptr，使用fallback模式
            return nullptr;
        }
    } else {
        // 没有证书文件，使用占位模式
        srs_error_t err = session->init_server("", "");
        if (err != srs_success) {
            srs_warn("Failed to init QUIC session (placeholder) for %s: %s", 
                    connection_id.c_str(), srs_error_desc(err).c_str());
            srs_freep(err);
            return nullptr;
        }
    }
    
    // 保存QUIC会话
    QuicSessionWrapper* session_ptr = session.get();
    quic_sessions_[connection_id] = std::move(session);
    
    srs_trace("Created QUIC session for connection: %s", connection_id.c_str());
    
    return session_ptr;
}

void QuicUdpHandler::cleanup_expired_adapters() {
    std::lock_guard<std::mutex> lock(adapters_mutex_);
    std::lock_guard<std::mutex> quic_lock(quic_sessions_mutex_);
    
    srs_utime_t now_ms = srs_time_now_cached() / 1000;
    std::vector<std::string> expired_ids;
    
    for (auto& pair : adapter_last_activity_) {
        const std::string& id = pair.first;
        int64_t last_activity = pair.second;
        
        if (now_ms - last_activity > adapter_timeout_ms_) {
            expired_ids.push_back(id);
        }
    }
    
    // 清理过期adapter和QUIC会话
    for (const auto& id : expired_ids) {
        // 清理adapter
        auto it = adapters_.find(id);
        if (it != adapters_.end()) {
            it->second->close();
            adapters_.erase(it);
            adapter_last_activity_.erase(id);
            
            // 移除连接统计
            AdapterStatsManager::instance().remove_connection(id);
        }
        
        // 清理QUIC会话
        auto quic_it = quic_sessions_.find(id);
        if (quic_it != quic_sessions_.end()) {
            quic_it->second->close();
            quic_sessions_.erase(quic_it);
        }
        
        srs_trace("Cleaned up expired adapter and QUIC session: %s", id.c_str());
    }
}

