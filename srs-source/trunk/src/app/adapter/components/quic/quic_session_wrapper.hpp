#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <memory>
#include <srs_kernel_error.hpp>
#include <sys/socket.h>
#include <netinet/in.h>

// ngtcp2前向声明（避免直接包含，通过实现文件包含）
// 在未启用QUIC时使用占位类型
#ifndef SRS_QUIC_ENABLED
#define SRS_QUIC_ENABLED 0
#endif

#if SRS_QUIC_ENABLED
// 真实ngtcp2类型（通过ngtcp2.h定义）
struct ngtcp2_conn;
struct ngtcp2_callbacks;
struct ngtcp2_rand_ctx;
struct ngtcp2_cid;
struct ngtcp2_crypto_aead_ctx;
struct ngtcp2_path;
struct ngtcp2_addr;
struct ngtcp2_preferred_addr;
struct ngtcp2_pkt_stateless_reset;
struct ngtcp2_crypto_conn_ref;
struct ngtcp2_version_cid;
struct ngtcp2_path_storage;
struct ngtcp2_pkt_hd;
struct ngtcp2_pkt_retry;
struct ngtcp2_vec;

// 占位类型定义（用于头文件中的类型声明）
// 这些类型在所有情况下都需要定义
typedef struct ngtcp2_pkt_hd ngtcp2_pkt_hd;
typedef int _ngtcp2_encryption_level_t;
typedef int _ngtcp2_path_validation_result_t;
// 这些类型在实现文件中通过包含完整的头文件来定义
// 在头文件中只做前向声明，在未启用QUIC时使用占位类型
struct ngtcp2_settings;
struct ngtcp2_transport_params;
// enum 类型在头文件中不能前向声明
// 使用int作为占位类型，在实现文件中会从ngtcp2.h获得完整的enum定义
// 虽然类型不精确，但函数签名兼容（enum在C++中可隐式转换为int）
// 这些类型在所有情况下都需要定义，用于函数签名
typedef int _ngtcp2_encryption_level_t;
typedef int _ngtcp2_path_validation_result_t;
// OpenSSL 类型（前向声明，避免包含完整的 OpenSSL 头文件）
struct ssl_ctx_st;
struct ssl_st;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
#else
// 占位类型
typedef void ngtcp2_conn;
typedef void ngtcp2_callbacks;
typedef void ngtcp2_rand_ctx;
typedef void ngtcp2_cid;
typedef void ngtcp2_crypto_aead_ctx;
typedef void ngtcp2_path;
typedef void ngtcp2_addr;
typedef void ngtcp2_preferred_addr;
typedef void ngtcp2_pkt_stateless_reset;
typedef void ngtcp2_crypto_conn_ref;
typedef struct ngtcp2_pkt_hd ngtcp2_pkt_hd;  // 前向声明
typedef int ngtcp2_encryption_level;
typedef int ngtcp2_path_validation_result;
typedef int _ngtcp2_encryption_level_t;  // 占位类型，用于函数签名
typedef int _ngtcp2_path_validation_result_t;  // 占位类型，用于函数签名
// SSL 类型在 SRS 的其他地方已经定义，不需要在这里重复定义
#endif

struct sockaddr;

// QUIC会话包装类 - 封装ngtcp2库的QUIC功能
class QuicSessionWrapper {
public:
    // Datagram接收回调（使用函数指针，兼容C++98）
    typedef void (*DatagramCallback)(void* ctx, const uint8_t* data, size_t size, int64_t timestamp_ms);
    
    // Stream数据接收回调（使用函数指针，兼容C++98）
    typedef void (*StreamCallback)(void* ctx, uint64_t stream_id, const uint8_t* data, size_t size, int fin);
    
    // 连接事件回调（使用函数指针，兼容C++98）
    typedef void (*ConnectionCallback)(void* ctx, int connected, const char* error);

public:
    QuicSessionWrapper();
    virtual ~QuicSessionWrapper();

    // 初始化QUIC服务器
    srs_error_t init_server(const std::string& cert_file, const std::string& key_file);
    
    // 接收UDP数据包（从QuicUdpHandler调用）
    srs_error_t on_udp_packet(const sockaddr* peer, const uint8_t* data, size_t size);
    
    // 处理连接和接收数据（需要定期调用）
    srs_error_t process_connections();
    
    // 发送datagram
    srs_error_t send_datagram(const uint8_t* data, size_t size);
    
    // 设置回调（使用函数指针和上下文指针）
    void set_datagram_callback(DatagramCallback cb, void* ctx) { 
        datagram_cb_ = cb; 
        datagram_cb_ctx_ = ctx;
    }
    void set_stream_callback(StreamCallback cb, void* ctx) { 
        stream_cb_ = cb; 
        stream_cb_ctx_ = ctx;
    }
    void set_connection_callback(ConnectionCallback cb, void* ctx) { 
        connection_cb_ = cb; 
        connection_cb_ctx_ = ctx;
    }
    
    // 获取连接状态
    bool is_connected() const { return is_connected_; }
    
    // 设置连接ID
    void set_connection_id(const std::string& id) { connection_id_ = id; }
    
    // 获取连接ID（用于标识）
    std::string get_connection_id() const { return connection_id_; }
    
    // 清理资源
    void close();

private:
#if SRS_QUIC_ENABLED
    // 设置ngtcp2回调函数
    void setup_callbacks();
    
    // 获取 ngtcp2_conn（供静态辅助函数使用）
    ngtcp2_conn* get_conn() const { return conn_; }
    
    // 静态辅助函数：从conn_ref获取ngtcp2_conn
    static ngtcp2_conn* get_ngtcp2_conn(ngtcp2_crypto_conn_ref* conn_ref);
#endif
    // ngtcp2连接
    ngtcp2_conn* conn_;
    
#if SRS_QUIC_ENABLED
    // SSL/TLS上下文（服务器共享）
    static SSL_CTX* ssl_ctx_;
    static int ssl_ctx_ref_count_;
    
    // SSL会话（每个连接一个，使用 QuicTLS）
    SSL* ssl_;
#endif
    
    // 回调函数（函数指针）
    DatagramCallback datagram_cb_;
    StreamCallback stream_cb_;
    ConnectionCallback connection_cb_;
    
    // 回调上下文指针
    void* datagram_cb_ctx_;
    void* stream_cb_ctx_;
    void* connection_cb_ctx_;
    
    // 连接状态
    bool is_connected_;
    std::string connection_id_;
    
    // 证书和密钥文件路径
    std::string cert_file_;
    std::string key_file_;
    
    // 对等方地址（存储最新接收的UDP包的对等方地址）
    struct sockaddr_storage peer_addr_;
    socklen_t peer_addrlen_;
    
    // ngtcp2设置和传输参数
#if SRS_QUIC_ENABLED
    // 这些类型在实现文件中通过包含完整的头文件来定义
    // 在头文件中使用指针以避免需要完整类型定义
    ngtcp2_settings* settings_ptr_;
    ngtcp2_transport_params* transport_params_ptr_;
    ngtcp2_callbacks* callbacks_ptr_;
    ngtcp2_cid* scid_ptr_; // 服务器连接ID（使用指针避免完整类型）
    ngtcp2_cid* dcid_ptr_; // 客户端连接ID
#else
    void* settings_ptr_; // 占位
    void* transport_params_ptr_; // 占位
    void* callbacks_ptr_; // 占位
    void* scid_ptr_; // 占位
    void* dcid_ptr_; // 占位
#endif
    
    // ngtcp2回调函数（静态，转发到实例方法）
    // 注意：ngtcp2回调函数的第一个参数是 ngtcp2_conn*，第二个参数是 user_data
    static int recv_stream_data(ngtcp2_conn* conn,
                                uint32_t flags,
                                int64_t stream_id,
                                uint64_t offset,
                                const uint8_t* data,
                                size_t datalen,
                                void* user_data,
                                void* stream_user_data);
    
    static int acked_stream_data_offset(ngtcp2_conn* conn,
                                       int64_t stream_id,
                                       uint64_t offset,
                                       uint64_t datalen,
                                       void* user_data,
                                       void* stream_user_data);
    
    static int extend_max_stream_data(ngtcp2_conn* conn,
                                     int64_t stream_id,
                                     uint64_t max_data,
                                     void* user_data,
                                     void* stream_user_data);
    
    static int recv_datagram(ngtcp2_conn* conn,
                            uint32_t flags,
                            const uint8_t* data,
                            size_t datalen,
                            void* user_data);
    
    static int acked_datagram(ngtcp2_conn* conn,
                             uint64_t dg,
                             void* user_data);
    
    static int stream_close(ngtcp2_conn* conn,
                           uint32_t flags,
                           int64_t stream_id,
                           uint64_t app_error_code,
                           void* user_data,
                           void* stream_user_data);
    
    static int extend_max_streams_bidi(ngtcp2_conn* conn,
                                      uint64_t max_streams,
                                      void* user_data);
    
    static int extend_max_streams_uni(ngtcp2_conn* conn,
                                     uint64_t max_streams,
                                     void* user_data);
    
    static void rand(uint8_t* dest, size_t destlen, const ngtcp2_rand_ctx* rand_ctx);
    
    static int get_new_connection_id(ngtcp2_conn* conn,
                                    ngtcp2_cid* cid,
                                    uint8_t* token,
                                    size_t cidlen,
                                    void* user_data);
    
    static int remove_connection_id(ngtcp2_conn* conn,
                                   const ngtcp2_cid* cid,
                                   void* user_data);
    
    // update_key 回调直接使用 ngtcp2_crypto_update_key_cb，无需单独声明
    
    static int path_validation(ngtcp2_conn* conn,
                              uint32_t flags,
                              const ngtcp2_path* path,
                              const ngtcp2_path* old_path,
                              _ngtcp2_path_validation_result_t res,
                              void* user_data);
    
    static int select_preferred_address(ngtcp2_conn* conn,
                                       ngtcp2_addr* dest,
                                       const ngtcp2_preferred_addr* paddr,
                                       void* user_data);
    
    static int stream_reset(ngtcp2_conn* conn,
                           int64_t stream_id,
                           uint64_t final_size,
                           uint64_t app_error_code,
                           void* user_data,
                           void* stream_user_data);
    
    static int extend_max_remote_streams_bidi(ngtcp2_conn* conn,
                                             uint64_t max_streams,
                                             void* user_data);
    
    static int extend_max_remote_streams_uni(ngtcp2_conn* conn,
                                            uint64_t max_streams,
                                            void* user_data);
    
    static int extend_max_stream_data_bidi(ngtcp2_conn* conn,
                                          int64_t stream_id,
                                          uint64_t max_data,
                                          void* user_data,
                                          void* stream_user_data);
    
    static int recv_rx_key(ngtcp2_conn* conn,
#if SRS_QUIC_ENABLED
                          // 在实现文件中会使用完整的enum类型
                          int level,  // 占位，实际是ngtcp2_encryption_level
#else
                          _ngtcp2_encryption_level_t level,
#endif
                          void* user_data);
    
    static int recv_tx_key(ngtcp2_conn* conn,
#if SRS_QUIC_ENABLED
                          // 在实现文件中会使用完整的enum类型
                          int level,  // 占位，实际是ngtcp2_encryption_level
#else
                          _ngtcp2_encryption_level_t level,
#endif
                          void* user_data);
    
    static int early_data(ngtcp2_conn* conn,
                         void* user_data);
    
    static int handshake_completed(ngtcp2_conn* conn,
                                  void* user_data);
    
    static int recv_version_negotiation(ngtcp2_conn* conn,
                                       const ngtcp2_pkt_hd* hd,
                                       const uint32_t* sv,
                                       size_t nsv,
                                       void* user_data);
    
    static int recv_retry(ngtcp2_conn* conn,
                         const ngtcp2_pkt_hd* hd,
                         void* user_data);
    
    static int recv_new_token(ngtcp2_conn* conn,
                             const uint8_t* token,
                             size_t tokenlen,
                             void* user_data);
    
    static int recv_stateless_reset(ngtcp2_conn* conn,
                                   const ngtcp2_pkt_stateless_reset* sr,
                                   void* user_data);
    
    // 发送数据包到UDP socket的回调
    typedef int (*SendPacketFunc)(void* user_data, const uint8_t* data, size_t datalen, const sockaddr* addr, socklen_t addrlen);
    SendPacketFunc send_packet_func_;
    void* send_packet_user_data_;
    
    // 设置发送数据包回调（由QuicUdpHandler调用）
    void set_send_packet_func(SendPacketFunc func, void* user_data) {
        send_packet_func_ = func;
        send_packet_user_data_ = user_data;
    }
};
