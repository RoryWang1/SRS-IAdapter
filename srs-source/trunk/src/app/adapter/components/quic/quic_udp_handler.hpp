#pragma once
#include <srs_app_listener.hpp>
#include "../../core/adapter_manager.hpp"
#include "quic_session_wrapper.hpp"
#include <map>
#include <memory>
#include <string>
#include <mutex>

// 前向声明
class IAdapter;

// QUIC UDP 处理器 - 将UDP数据包路由到对应的Adapter
class QuicUdpHandler : public ISrsUdpHandler {
public:
    QuicUdpHandler(const std::string& protocol_name);
    virtual ~QuicUdpHandler();

public:
    // ISrsUdpHandler接口
    virtual srs_error_t on_udp_packet(const sockaddr *from, const int fromlen, 
                                      char *buf, int nb_buf) override;

    // 路由配置
    struct Route {
        std::string vhost;
        std::string app;
        std::string stream;
    };

    // 设置固定路由
    void set_fixed_route(const Route& r);
    // 增加端口映射路由
    void add_port_mapping(int port, const Route& r);

    // 清理过期连接
    void cleanup_expired_adapters();
    
    // 设置QUIC证书和密钥文件
    void set_quic_cert_files(const std::string& cert_file, const std::string& key_file) {
        quic_cert_file_ = cert_file;
        quic_key_file_ = key_file;
    }

private:
    // 从sockaddr获取连接标识（IP:Port）
    std::string get_connection_id(const sockaddr* from, int fromlen);
    
    // 获取或创建adapter实例
    IAdapter* get_or_create_adapter(const std::string& connection_id, 
                                    const std::string& client_ip, int client_port);
    
    // 根据端口获取路由
    Route get_route_for_port(int port);

private:
    std::string protocol_name_;
    Route fixed_route_;
    std::map<int, Route> port_mapping_;
    
    // Adapter实例管理（按连接ID）
    std::map<std::string, std::unique_ptr<IAdapter> > adapters_;
    std::map<std::string, int64_t> adapter_last_activity_;
    std::mutex adapters_mutex_;
    
    // QUIC会话管理（按连接ID）
    std::map<std::string, std::unique_ptr<QuicSessionWrapper> > quic_sessions_;
    std::mutex quic_sessions_mutex_;
    
    // 超时配置
    int64_t adapter_timeout_ms_;
    
    // QUIC配置
    std::string quic_cert_file_;
    std::string quic_key_file_;
    
    // 获取或创建QUIC会话
    QuicSessionWrapper* get_or_create_quic_session(const std::string& connection_id,
                                                    IAdapter* adapter);
};

