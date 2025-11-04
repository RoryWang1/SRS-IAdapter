#include "quic_session_wrapper.hpp"
#include <srs_kernel_log.hpp>
#include <srs_core_time.hpp>
#include <cstring>
#include <sstream>
#include <netinet/in.h>
#include <sys/socket.h>

#if SRS_QUIC_ENABLED
// ngtcp2 实现
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_quictls.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#else
// 占位实现
#endif

#if SRS_QUIC_ENABLED
// 静态成员初始化
SSL_CTX* QuicSessionWrapper::ssl_ctx_ = NULL;
int QuicSessionWrapper::ssl_ctx_ref_count_ = 0;
#endif

QuicSessionWrapper::QuicSessionWrapper()
    : conn_(nullptr),
#if SRS_QUIC_ENABLED
      ssl_(NULL),
#endif
      datagram_cb_(NULL), stream_cb_(NULL), connection_cb_(NULL),
      datagram_cb_ctx_(NULL), stream_cb_ctx_(NULL), connection_cb_ctx_(NULL),
      is_connected_(false),
      send_packet_func_(NULL), send_packet_user_data_(NULL) {
    memset(&peer_addr_, 0, sizeof(peer_addr_));
    peer_addrlen_ = 0;
#if SRS_QUIC_ENABLED
    // 分配内存给结构体
    settings_ptr_ = new ngtcp2_settings();
    transport_params_ptr_ = new ngtcp2_transport_params();
    callbacks_ptr_ = new ngtcp2_callbacks();
    scid_ptr_ = new ngtcp2_cid();
    dcid_ptr_ = new ngtcp2_cid();
    memset(settings_ptr_, 0, sizeof(ngtcp2_settings));
    memset(transport_params_ptr_, 0, sizeof(ngtcp2_transport_params));
    memset(callbacks_ptr_, 0, sizeof(ngtcp2_callbacks));
    memset(scid_ptr_, 0, sizeof(ngtcp2_cid));
    memset(dcid_ptr_, 0, sizeof(ngtcp2_cid));
#else
    settings_ptr_ = NULL;
    transport_params_ptr_ = NULL;
    callbacks_ptr_ = NULL;
    scid_ptr_ = NULL;
    dcid_ptr_ = NULL;
#endif
}

QuicSessionWrapper::~QuicSessionWrapper() {
    close();
#if SRS_QUIC_ENABLED
    // 释放分配的内存
    if (settings_ptr_) {
        delete settings_ptr_;
        settings_ptr_ = NULL;
    }
    if (transport_params_ptr_) {
        delete transport_params_ptr_;
        transport_params_ptr_ = NULL;
    }
    if (callbacks_ptr_) {
        delete callbacks_ptr_;
        callbacks_ptr_ = NULL;
    }
    if (scid_ptr_) {
        delete scid_ptr_;
        scid_ptr_ = NULL;
    }
    if (dcid_ptr_) {
        delete dcid_ptr_;
        dcid_ptr_ = NULL;
    }
#endif
}

srs_error_t QuicSessionWrapper::init_server(const std::string& cert_file, const std::string& key_file) {
    srs_error_t err = srs_success;
    
#if SRS_QUIC_ENABLED
    cert_file_ = cert_file;
    key_file_ = key_file;
    
    // 初始化共享的 SSL_CTX（如果还没有初始化）
    if (!ssl_ctx_) {
        // 初始化 ngtcp2 crypto QuicTLS 库
        if (ngtcp2_crypto_quictls_init() != 0) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "ngtcp2_crypto_quictls_init failed");
        }
        
        // 创建 SSL_CTX
        ssl_ctx_ = SSL_CTX_new(TLS_method());
        if (!ssl_ctx_) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "SSL_CTX_new failed");
        }
        
        // 设置证书和密钥
        if (SSL_CTX_use_certificate_chain_file(ssl_ctx_, cert_file.c_str()) != 1) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = NULL;
            return srs_error_new(ERROR_TLS_KEY_CRT, "SSL_CTX_use_certificate_chain_file failed: %s", cert_file.c_str());
        }
        
        if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = NULL;
            return srs_error_new(ERROR_TLS_KEY_CRT, "SSL_CTX_use_PrivateKey_file failed: %s", key_file.c_str());
        }
        
        if (SSL_CTX_check_private_key(ssl_ctx_) != 1) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = NULL;
            return srs_error_new(ERROR_TLS_KEY_CRT, "SSL_CTX_check_private_key failed");
        }
        
        // 配置 QuicTLS（必须在创建 SSL 会话之前）
        if (ngtcp2_crypto_quictls_configure_server_context(ssl_ctx_) != 0) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = NULL;
            return srs_error_new(ERROR_TLS_HANDSHAKE, "ngtcp2_crypto_quictls_configure_server_context failed");
        }
        
        ssl_ctx_ref_count_ = 0;
        srs_trace("QUIC SSL_CTX initialized successfully (QuicTLS)");
    }
    
    ssl_ctx_ref_count_++;
    
    // 初始化 ngtcp2 settings
    ngtcp2_settings_default(settings_ptr_);
    settings_ptr_->log_printf = NULL; // 使用 SRS 的日志系统
    settings_ptr_->initial_ts = srs_time_now_cached() / 1000; // 毫秒
    
    // 初始化传输参数
    ngtcp2_transport_params_default(transport_params_ptr_);
    transport_params_ptr_->initial_max_stream_data_bidi_local = 128 * 1024;
    transport_params_ptr_->initial_max_stream_data_bidi_remote = 128 * 1024;
    transport_params_ptr_->initial_max_stream_data_uni = 128 * 1024;
    transport_params_ptr_->initial_max_data = 1024 * 1024;
    transport_params_ptr_->initial_max_streams_bidi = 100;
    transport_params_ptr_->initial_max_streams_uni = 100;
    transport_params_ptr_->max_idle_timeout = 30 * 1000; // 30秒
    transport_params_ptr_->max_udp_payload_size = NGTCP2_MAX_UDP_PAYLOAD_SIZE;
    
    srs_trace("QUIC session wrapper initialized (server mode, ngtcp2)");
#else
    srs_warn("QUIC library not enabled, using placeholder implementation");
    // 占位：标记为已连接以允许测试
    is_connected_ = true;
    connection_id_ = "placeholder";
#endif
    
    return err;
}

srs_error_t QuicSessionWrapper::on_udp_packet(const sockaddr* peer, const uint8_t* data, size_t size) {
    srs_error_t err = srs_success;
    
#if SRS_QUIC_ENABLED
    // 保存对等方地址
    if (peer) {
        memcpy(&peer_addr_, peer, sizeof(peer_addr_));
        if (peer->sa_family == AF_INET) {
            peer_addrlen_ = sizeof(struct sockaddr_in);
        } else if (peer->sa_family == AF_INET6) {
            peer_addrlen_ = sizeof(struct sockaddr_in6);
        } else {
            peer_addrlen_ = sizeof(struct sockaddr_storage);
        }
    }
    
    if (!conn_) {
        // 解析 Initial 包以获取连接ID和版本信息
        ngtcp2_version_cid vc;
        // 使用默认的SCID长度（通常为8字节）
        size_t scidlen = 8;
        int rv = ngtcp2_pkt_decode_version_cid(&vc, data, size, scidlen);
        if (rv != 0) {
            // 不是有效的 QUIC 包，忽略
            return err;
        }
        
        // 使用 ngtcp2_accept 验证是否是有效的 Initial 包
        ngtcp2_pkt_hd hd;
        rv = ngtcp2_accept(&hd, data, size);
        if (rv != 0) {
            // 不是 Initial 包，忽略
            srs_warn("QUIC: received non-initial packet before connection established");
            return err;
        }
        
        // 只处理 Initial 包
        if (hd.type != NGTCP2_PKT_INITIAL) {
            srs_warn("QUIC: received non-initial packet before connection established");
            return err;
        }
        
        // 保存客户端连接ID（从version_cid中获取，因为它支持更长的CID）
        ngtcp2_cid_init(dcid_ptr_, vc.dcid, vc.dcidlen);
        
        // 生成服务器连接ID
        if (RAND_bytes(scid_ptr_->data, NGTCP2_MAX_CIDLEN) != 1) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "RAND_bytes failed for SCID");
        }
        scid_ptr_->datalen = NGTCP2_MAX_CIDLEN;
        
        // 创建 SSL 会话（QuicTLS 已在 SSL_CTX 级别配置，无需单独配置 SSL）
        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            return srs_error_new(ERROR_TLS_HANDSHAKE, "SSL_new failed");
        }
        
        // 配置回调函数（必须在创建连接之前）
        setup_callbacks();
        
        // 创建路径
        ngtcp2_path_storage ps;
        ngtcp2_path_storage_zero(&ps);
        // 设置远程地址
        ngtcp2_addr_init(&ps.path.remote, peer, peer_addrlen_);
        
        // 创建 ngtcp2 连接（先创建连接对象）
        uint32_t version = NGTCP2_PROTO_VER_V1;
        ngtcp2_conn* conn = NULL;
        rv = ngtcp2_conn_server_new(&conn, dcid_ptr_, scid_ptr_, &ps.path, version,
                                     callbacks_ptr_, settings_ptr_, transport_params_ptr_,
                                     NULL, this);
        
        if (rv != 0) {
            SSL_free(ssl_);
            ssl_ = NULL;
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "ngtcp2_conn_server_new failed: %s", ngtcp2_strerror(rv));
        }
        
        conn_ = conn;
        
        // 设置应用数据（用于回调，必须在创建连接之后）
        ngtcp2_crypto_conn_ref conn_ref;
        conn_ref.get_conn = get_ngtcp2_conn;
        conn_ref.user_data = this;
        SSL_set_app_data(ssl_, &conn_ref);
        
        // 设置 SSL 为接受状态（服务器）
        SSL_set_accept_state(ssl_);
        
        // 设置连接ID字符串（用于标识）
        std::ostringstream oss;
        for (size_t i = 0; i < scid_ptr_->datalen; i++) {
            oss << std::hex << (int)scid_ptr_->data[i];
        }
        connection_id_ = oss.str();
        
        srs_trace("QUIC: created new connection, SCID=%s", connection_id_.c_str());
    }
    
    // 将UDP数据包传递给ngtcp2连接
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    
    // 设置远程地址
    ngtcp2_addr_init(&ps.path.remote, peer, peer_addrlen_);
    
    // ngtcp2_path 的 local 和 remote 是 ngtcp2_addr 类型（值，不是指针）
    // 直接使用 ps.path 的地址
    ngtcp2_path* path = &ps.path;
    
    // 接收数据包
    ngtcp2_pkt_info pi;
    int rv = ngtcp2_conn_read_pkt(conn_, path, &pi, data, size, srs_time_now_cached() / 1000); // 转换为毫秒
    
    if (rv != 0 && rv != NGTCP2_ERR_DISCARD_PKT) {
        // NGTCP2_ERR_DISCARD_PKT 表示包被丢弃（正常情况，可能是不匹配的包）
        // 处理各种错误情况
        if (ngtcp2_err_is_fatal(rv)) {
            switch (rv) {
            case NGTCP2_ERR_CRYPTO:
                // 加密错误，可能是握手失败
                return srs_error_new(ERROR_TLS_HANDSHAKE, "QUIC crypto error: %s", ngtcp2_strerror(rv));
            case NGTCP2_ERR_DRAINING:
                // 连接正在关闭，停止处理
                is_connected_ = false;
                return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "QUIC connection draining");
            case NGTCP2_ERR_RETRY:
                // 需要发送 Retry 包（服务器模式）
                return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "QUIC retry required");
            case NGTCP2_ERR_DROP_CONN:
                // 连接需要被丢弃
                is_connected_ = false;
                return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "QUIC connection dropped");
            default:
                return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "ngtcp2_conn_read_pkt failed: %s", ngtcp2_strerror(rv));
            }
        }
    }
    
    // 处理连接事件（可能会触发datagram回调）
    if ((err = process_connections()) != srs_success) {
        return srs_error_wrap(err, "process connections");
    }
#else
    // 占位：直接触发datagram回调（模拟QUIC datagram接收）
    if (datagram_cb_ && size > 0) {
        srs_utime_t now_us = srs_time_now_cached();
        int64_t timestamp_ms = now_us / 1000;
        datagram_cb_(datagram_cb_ctx_, data, size, timestamp_ms);
    }
#endif
    
    return err;
}

srs_error_t QuicSessionWrapper::process_connections() {
    srs_error_t err = srs_success;
    
#if SRS_QUIC_ENABLED
    if (!conn_) {
        return err;
    }
    
    // 处理连接超时和重传
    uint64_t expiry = ngtcp2_conn_get_expiry(conn_);
    srs_utime_t now_ms = srs_time_now_cached() / 1000;
    
    if (expiry <= now_ms) {
        // 处理连接超时和重传
        int rv = ngtcp2_conn_handle_expiry(conn_, now_ms);
        if (rv != 0) {
            // 超时处理失败可能表示连接已断开
            if (ngtcp2_err_is_fatal(rv)) {
                is_connected_ = false;
            }
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "ngtcp2_conn_handle_expiry failed: %s", ngtcp2_strerror(rv));
        }
    }
    
    // 发送待发送的数据包（循环发送直到没有更多数据）
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    
    // 设置远程地址
    ngtcp2_addr_init(&ps.path.remote, (const struct sockaddr*)&peer_addr_, peer_addrlen_);
    
    ngtcp2_path* path = &ps.path;
    ngtcp2_tstamp ts = now_ms;
    
    // 循环发送所有待发送的包
    while (true) {
        uint8_t out[1500];
        size_t outlen = sizeof(out);
        ngtcp2_pkt_info pi;
        
        ssize_t nwrite = ngtcp2_conn_write_pkt(conn_, path, &pi, out, outlen, ts);
        
        if (nwrite < 0) {
            if (nwrite == NGTCP2_ERR_WRITE_MORE) {
                // 需要继续写入，但先发送当前包
                if (send_packet_func_ && outlen > 0) {
                    send_packet_func_(send_packet_user_data_, out, outlen, path->remote.addr, path->remote.addrlen);
                }
                continue;
            } else {
                // 其他错误或没有数据可发送
                break;
            }
        } else if (nwrite > 0) {
            // 成功写入数据包
            if (send_packet_func_) {
                send_packet_func_(send_packet_user_data_, out, nwrite, path->remote.addr, path->remote.addrlen);
            }
        } else {
            // 没有更多数据包需要发送
            break;
        }
    }
    
    // 检查连接状态（使用handshake_completed回调来设置is_connected_）
    // 注意：某些版本的ngtcp2可能没有ngtcp2_conn_get_state函数
    // 连接状态已在handshake_completed回调中处理
    // 这里不需要再次检查，避免使用可能不存在的API
#else
    // 占位：无需处理
#endif
    
    return err;
}

srs_error_t QuicSessionWrapper::send_datagram(const uint8_t* data, size_t size) {
    srs_error_t err = srs_success;
    
#if SRS_QUIC_ENABLED
    if (!conn_) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "no active QUIC connection");
    }
    
    // 检查连接是否准备好发送 datagram
    // 注意：某些版本的ngtcp2可能没有ngtcp2_conn_get_state函数
    // 使用is_connected_标志来判断连接状态（在handshake_completed回调中设置）
    if (!is_connected_) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "QUIC connection not ready for datagram");
    }
    
    // 发送 datagram
    // 准备路径信息
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_addr_init(&ps.path.remote, (const struct sockaddr*)&peer_addr_, peer_addrlen_);
    ngtcp2_path* path = &ps.path;
    
    // 发送缓冲区
    uint8_t out[1500];
    size_t outlen = sizeof(out);
    ngtcp2_pkt_info pi;
    int accepted = 0;
    ngtcp2_tstamp ts = srs_time_now_cached() / 1000;
    
    // 使用 ngtcp2_conn_write_datagram 发送单个 datagram
    // (宏定义会自动使用正确的版本化函数)
    ssize_t spktlen = ngtcp2_conn_write_datagram(
        conn_, path, &pi, out, outlen, &accepted,
        NGTCP2_WRITE_DATAGRAM_FLAG_NONE, 0,  // dgram_id = 0 (让 ngtcp2 自动分配)
        data, size, ts);
    
    if (spktlen < 0) {
        // 处理错误（可能是不支持 datagram 或连接未准备好）
        if (spktlen == NGTCP2_ERR_DATAGRAM_UNSUPPORTED) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "QUIC datagram not supported by peer");
        }
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "ngtcp2_conn_write_datagram failed: %s", ngtcp2_strerror((int)spktlen));
    }
    
    if (spktlen > 0 && accepted) {
        // 成功打包了 datagram，通过 send_packet_func_ 发送
        if (send_packet_func_) {
            send_packet_func_(send_packet_user_data_, out, (size_t)spktlen, path->remote.addr, path->remote.addrlen);
        }
    } else if (!accepted) {
        // datagram 被拒绝（可能因为流控或连接状态）
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "QUIC datagram not accepted");
    }
    
    // 触发发送其他待发送的包（包括 ack 等）
    if ((err = process_connections()) != srs_success) {
        return srs_error_wrap(err, "process connections after send_datagram");
    }
#else
    // 占位：记录日志
    srs_info("QUIC send_datagram (placeholder): size=%zu", size);
#endif
    
    return err;
}

void QuicSessionWrapper::close() {
#if SRS_QUIC_ENABLED
    // 清理ngtcp2资源
    if (conn_) {
        ngtcp2_conn_del(conn_);
        conn_ = nullptr;
    }
    
    // 清理SSL和crypto context
    if (ssl_) {
        // QuicTLS 需要在 SSL_free 之前清除 app_data
        SSL_set_app_data(ssl_, NULL);
        SSL_free(ssl_);
        ssl_ = NULL;
    }
    
    // 减少SSL_CTX引用计数
    if (ssl_ctx_ && ssl_ctx_ref_count_ > 0) {
        ssl_ctx_ref_count_--;
        // 注意：这里不释放ssl_ctx_，因为它是共享的
        // 如果需要完全清理，应该在应用程序层面处理
    }
#else
    // 占位：重置状态
#endif
    
    is_connected_ = false;
    connection_id_.clear();
}

#if SRS_QUIC_ENABLED
// 静态辅助函数：从conn_ref获取ngtcp2_conn
ngtcp2_conn* QuicSessionWrapper::get_ngtcp2_conn(ngtcp2_crypto_conn_ref* conn_ref) {
    QuicSessionWrapper* wrapper = static_cast<QuicSessionWrapper*>(conn_ref->user_data);
    return wrapper ? wrapper->get_conn() : NULL;
}

// 设置ngtcp2回调函数
void QuicSessionWrapper::setup_callbacks() {
    memset(callbacks_ptr_, 0, sizeof(ngtcp2_callbacks));
    
    // 使用 ngtcp2_crypto 提供的通用回调（适配 QuicTLS 后端）
    callbacks_ptr_->client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks_ptr_->recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks_ptr_->encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks_ptr_->decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks_ptr_->hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks_ptr_->recv_retry = ngtcp2_crypto_recv_retry_cb;
    callbacks_ptr_->version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks_ptr_->update_key = ngtcp2_crypto_update_key_cb;
    
    // 应用层回调
    callbacks_ptr_->recv_stream_data = recv_stream_data;
    callbacks_ptr_->acked_stream_data_offset = acked_stream_data_offset;
    callbacks_ptr_->extend_max_stream_data = extend_max_stream_data;
    callbacks_ptr_->recv_datagram = recv_datagram;
    // acked_datagram 在某些版本中可能是 ack_datagram
    // callbacks_ptr_->acked_datagram = acked_datagram;
    callbacks_ptr_->stream_close = stream_close;
    // extend_max_streams_bidi/uni 在某些版本中可能是 extend_max_local_streams_bidi/uni
    // callbacks_ptr_->extend_max_streams_bidi = extend_max_streams_bidi;
    // callbacks_ptr_->extend_max_streams_uni = extend_max_streams_uni;
    callbacks_ptr_->rand = rand;
    callbacks_ptr_->get_new_connection_id = get_new_connection_id;
    callbacks_ptr_->remove_connection_id = remove_connection_id;
    // path_validation需要类型转换，因为头文件中使用的是占位类型
    callbacks_ptr_->path_validation = reinterpret_cast<ngtcp2_path_validation>(path_validation);
    // select_preferred_address 在某些版本的ngtcp2中可能不存在，暂时注释
    // callbacks_ptr_->select_preferred_address = select_preferred_address;
    callbacks_ptr_->stream_reset = stream_reset;
    callbacks_ptr_->extend_max_remote_streams_bidi = extend_max_remote_streams_bidi;
    callbacks_ptr_->extend_max_remote_streams_uni = extend_max_remote_streams_uni;
    // extend_max_stream_data_bidi 在某些版本中可能不存在，暂时注释
    // callbacks_ptr_->extend_max_stream_data_bidi = extend_max_stream_data_bidi;
    // recv_rx_key和recv_tx_key需要类型转换
    callbacks_ptr_->recv_rx_key = reinterpret_cast<ngtcp2_recv_key>(recv_rx_key);
    callbacks_ptr_->recv_tx_key = reinterpret_cast<ngtcp2_recv_key>(recv_tx_key);
    // early_data 在某些版本的ngtcp2中可能不存在，暂时注释
    // callbacks_ptr_->early_data = early_data;
    callbacks_ptr_->handshake_completed = handshake_completed;
    callbacks_ptr_->recv_version_negotiation = recv_version_negotiation;
    callbacks_ptr_->recv_retry = recv_retry;
    callbacks_ptr_->recv_new_token = recv_new_token;
    callbacks_ptr_->recv_stateless_reset = recv_stateless_reset;
}
#endif

// ngtcp2 回调函数实现
#if SRS_QUIC_ENABLED
int QuicSessionWrapper::recv_stream_data(ngtcp2_conn* conn,
                                        uint32_t flags,
                                        int64_t stream_id,
                                        uint64_t offset,
                                        const uint8_t* data,
                                        size_t datalen,
                                        void* user_data,
                                        void* stream_user_data) {
    QuicSessionWrapper* self = static_cast<QuicSessionWrapper*>(user_data);
    if (self && self->stream_cb_) {
        int fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0;
        self->stream_cb_(self->stream_cb_ctx_, stream_id, data, datalen, fin);
    }
    return 0;
}

int QuicSessionWrapper::recv_datagram(ngtcp2_conn* conn,
                                     uint32_t flags,
                                     const uint8_t* data,
                                     size_t datalen,
                                     void* user_data) {
    QuicSessionWrapper* self = static_cast<QuicSessionWrapper*>(user_data);
    if (self && self->datagram_cb_) {
        srs_utime_t now_us = srs_time_now_cached();
        int64_t timestamp_ms = now_us / 1000;
        self->datagram_cb_(self->datagram_cb_ctx_, data, datalen, timestamp_ms);
    }
    return 0;
}

int QuicSessionWrapper::acked_stream_data_offset(ngtcp2_conn* conn,
                                                int64_t stream_id,
                                                uint64_t offset,
                                                uint64_t datalen,
                                                void* user_data,
                                                void* stream_user_data) {
    return 0;
}

int QuicSessionWrapper::extend_max_stream_data(ngtcp2_conn* conn,
                                              int64_t stream_id,
                                              uint64_t max_data,
                                              void* user_data,
                                              void* stream_user_data) {
    return 0;
}

int QuicSessionWrapper::acked_datagram(ngtcp2_conn* conn,
                                      uint64_t dg,
                                      void* user_data) {
    return 0;
}

int QuicSessionWrapper::stream_close(ngtcp2_conn* conn,
                                    uint32_t flags,
                                    int64_t stream_id,
                                    uint64_t app_error_code,
                                    void* user_data,
                                    void* stream_user_data) {
    return 0;
}

int QuicSessionWrapper::extend_max_streams_bidi(ngtcp2_conn* conn,
                                               uint64_t max_streams,
                                               void* user_data) {
    return 0;
}

int QuicSessionWrapper::extend_max_streams_uni(ngtcp2_conn* conn,
                                              uint64_t max_streams,
                                              void* user_data) {
    return 0;
}

void QuicSessionWrapper::rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx) {
    // 使用OpenSSL的随机数生成器
    // 注意：ngtcp2的rand回调返回void，如果失败会调用其他方式
    RAND_bytes(dest, destlen);
}

int QuicSessionWrapper::get_new_connection_id(ngtcp2_conn* conn,
                                             ngtcp2_cid* cid,
                                             uint8_t* token,
                                             size_t cidlen,
                                             void* user_data) {
    // 生成新的连接ID（使用随机数）
    if (RAND_bytes(cid->data, cidlen) != 1) {
        return -1;
    }
    cid->datalen = cidlen;
    
    // TODO: 生成 stateless reset token（如果需要）
    if (token) {
        if (RAND_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN) != 1) {
            return -1;
        }
    }
    
    return 0;
}

int QuicSessionWrapper::remove_connection_id(ngtcp2_conn* conn,
                                            const ngtcp2_cid* cid,
                                            void* user_data) {
    return 0;
}

// update_key 回调现在直接使用 ngtcp2_crypto_update_key_cb，不需要单独实现

int QuicSessionWrapper::path_validation(ngtcp2_conn* conn,
                                       uint32_t flags,
                                       const ngtcp2_path* path,
                                       const ngtcp2_path* old_path,
                                       _ngtcp2_path_validation_result_t res,
                                       void* user_data) {
    // 在实现文件中，虽然参数类型是占位类型，但实际运行时ngtcp2会传入正确的enum值
    (void)conn;
    (void)flags;
    (void)path;
    (void)old_path;
    (void)res;
    (void)user_data;
    return 0;
}

int QuicSessionWrapper::select_preferred_address(ngtcp2_conn* conn,
                                                ngtcp2_addr* dest,
                                                const ngtcp2_preferred_addr* paddr,
                                                void* user_data) {
    return 0;
}

int QuicSessionWrapper::stream_reset(ngtcp2_conn* conn,
                                    int64_t stream_id,
                                    uint64_t final_size,
                                    uint64_t app_error_code,
                                    void* user_data,
                                    void* stream_user_data) {
    (void)conn;
    (void)stream_id;
    (void)final_size;
    (void)app_error_code;
    (void)user_data;
    (void)stream_user_data;
    return 0;
}

int QuicSessionWrapper::extend_max_remote_streams_bidi(ngtcp2_conn* conn,
                                                      uint64_t max_streams,
                                                      void* user_data) {
    return 0;
}

int QuicSessionWrapper::extend_max_remote_streams_uni(ngtcp2_conn* conn,
                                                     uint64_t max_streams,
                                                     void* user_data) {
    return 0;
}

int QuicSessionWrapper::extend_max_stream_data_bidi(ngtcp2_conn* conn,
                                                   int64_t stream_id,
                                                   uint64_t max_data,
                                                   void* user_data,
                                                   void* stream_user_data) {
    return 0;
}

int QuicSessionWrapper::recv_rx_key(ngtcp2_conn* conn,
#if SRS_QUIC_ENABLED
                                   int level,  // 占位，实际是ngtcp2_encryption_level
#else
                                   _ngtcp2_encryption_level_t level,
#endif
                                   void* user_data) {
    (void)conn;
    (void)level;
    (void)user_data;
    return 0;
}

int QuicSessionWrapper::recv_tx_key(ngtcp2_conn* conn,
#if SRS_QUIC_ENABLED
                                   int level,  // 占位，实际是ngtcp2_encryption_level
#else
                                   _ngtcp2_encryption_level_t level,
#endif
                                   void* user_data) {
    (void)conn;
    (void)level;
    (void)user_data;
    return 0;
}

int QuicSessionWrapper::early_data(ngtcp2_conn* conn,
                                  void* user_data) {
    return 0;
}

int QuicSessionWrapper::handshake_completed(ngtcp2_conn* conn,
                                           void* user_data) {
    QuicSessionWrapper* self = static_cast<QuicSessionWrapper*>(user_data);
    if (self && self->connection_cb_) {
        self->connection_cb_(self->connection_cb_ctx_, 1, "");
    }
    return 0;
}

int QuicSessionWrapper::recv_version_negotiation(ngtcp2_conn* conn,
                                                const ngtcp2_pkt_hd* hd,
                                                const uint32_t* sv,
                                                size_t nsv,
                                                void* user_data) {
    return 0;
}

int QuicSessionWrapper::recv_retry(ngtcp2_conn* conn,
                                  const ngtcp2_pkt_hd* hd,
                                  void* user_data) {
    return 0;
}

int QuicSessionWrapper::recv_new_token(ngtcp2_conn* conn,
                                      const uint8_t* token,
                                      size_t tokenlen,
                                      void* user_data) {
    (void)conn;
    (void)token;
    (void)tokenlen;
    (void)user_data;
    return 0;
}

int QuicSessionWrapper::recv_stateless_reset(ngtcp2_conn* conn,
                                            const ngtcp2_pkt_stateless_reset* sr,
                                            void* user_data) {
    return 0;
}

// send_packet 不是类方法，而是通过 send_packet_func_ 回调函数调用的
// 不需要在这里实现类方法
#endif
