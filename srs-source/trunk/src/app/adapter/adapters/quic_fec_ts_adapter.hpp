#pragma once
#include "../core/iadapter.hpp"
#include "../components/fec/fec_group_buffer.hpp"
#include "../components/reorder/reorder_buffer.hpp"
#include "../core/adapter_stats.hpp"
#include "../../../kernel/srs_kernel_log.hpp"
#include "../../../kernel/srs_kernel_error.hpp"
#include "../../../kernel/srs_kernel_buffer.hpp"
#include "../../../kernel/srs_kernel_ts.hpp"
#include <vector>
#include <memory>
#include <map>
#include <string>
#include <atomic>
#include <mutex>

// 前向声明
class SrsTsContext;
class SrsTsMessage;
class ISrsTsHandler;
class FrameToSourceBridge;
class SrsRawH264Stream;
class SrsRawHEVCStream;
class SrsRawAacStream;
struct SrsRawAacStreamCodec;

// QUIC+FEC TS适配器
class QuicFecTsAdapter : public IAdapter {
public:
    QuicFecTsAdapter();
    virtual ~QuicFecTsAdapter();

public:
    virtual srs_error_t start(const AdapterInit& init) override;
    virtual srs_error_t feed(const uint8_t* data, size_t nbytes) override;
    virtual srs_error_t parseFrame() override;
    virtual srs_error_t flush() override;
    virtual void close() override;
    
    virtual void setOnStartStream(OnStartStreamCallback callback) override;
    virtual void setOnStopStream(OnStopStreamCallback callback) override;

private:
    // QUIC会话管理（由 QuicUdpHandler 管理，这里只保留元数据）
    struct QuicSessionInfo {
        std::string connection_id;
        int64_t last_activity_ms;
        bool is_active;
        
        QuicSessionInfo() : last_activity_ms(0), is_active(false) {}
    };
    
    // 协议探测模式
    enum ProtocolMode {
        MODE_UNKNOWN,           // 未探测
        MODE_QUIC_FEC,          // QUIC+FEC模式
        MODE_BARE_TS            // 裸TS模式（兜底）
    };
    
    // 配置参数
    struct Config {
        std::string listen_address;     // 监听地址
        int listen_port;                // 监听端口
        std::string remote_address;     // 远程地址（客户端模式）
        int remote_port;                // 远程端口
        
        // FEC配置
        FecGroupConfig fec_config;
        
        // 乱序缓冲配置
        ReorderBufferConfig reorder_config;
        
        // 协议探测
        bool enable_protocol_detection; // 启用协议探测
        int64_t detection_timeout_ms;   // 探测超时
        
        // 资源限制
        size_t max_sessions;            // 最大会话数
        size_t max_buffer_size;         // 最大缓冲区大小
        
        Config() 
            : listen_port(8443), remote_port(0),
              enable_protocol_detection(true), detection_timeout_ms(1000),
              max_sessions(100), max_buffer_size(50 * 1024 * 1024) {} // 50MB
    };
    
    // 初始化配置
    srs_error_t init_config(const AdapterInit& init);
    
    // QUIC会话管理
    srs_error_t create_quic_session(const std::string& connection_id);
    srs_error_t remove_quic_session(const std::string& connection_id);
    
    // 协议探测
    srs_error_t detect_protocol(const uint8_t* data, size_t size);
    ProtocolMode get_current_mode() const { return current_mode_; }
    
    // 数据接收处理
    srs_error_t handle_quic_data(const std::string& connection_id, 
                                  const uint8_t* data, size_t size,
                                  uint64_t seq_num, bool is_parity,
                                  uint32_t group_id, uint32_t block_index,
                                  int64_t timestamp_ms, bool is_keyframe);
    
    srs_error_t handle_bare_ts_data(const uint8_t* data, size_t size);
    
    // FEC处理流程
    srs_error_t process_fec_groups();
    
    // 乱序重排处理
    srs_error_t process_reorder_buffer(const std::string& connection_id);
    
    // TS包输出
    srs_error_t output_ts_packets(const std::vector<std::vector<uint8_t> >& packets);
    
    // 清理过期资源
    void cleanup_expired_sessions();
    
    // 统计信息
    struct Stats {
        std::atomic<uint64_t> total_packets_received;
        std::atomic<uint64_t> fec_repaired_packets;
        std::atomic<uint64_t> reordered_packets;
        std::atomic<uint64_t> dropped_packets;
        std::atomic<uint64_t> bare_ts_packets;
        std::atomic<uint64_t> quic_packets;
        
        Stats() 
            : total_packets_received(0), fec_repaired_packets(0),
              reordered_packets(0), dropped_packets(0),
              bare_ts_packets(0), quic_packets(0) {}
    } stats_;

private:
    AdapterInit init_;
    Config config_;
    ProtocolMode current_mode_;
    
    // QUIC会话映射
    std::map<std::string, std::unique_ptr<QuicSessionInfo> > sessions_;
    std::mutex sessions_mutex_;
    
    // FEC修复管理器（每个会话一个）
    std::map<std::string, std::unique_ptr<FecRepairManager> > fec_managers_;
    
    // 乱序缓冲区（每个会话一个）
    std::map<std::string, std::unique_ptr<ReorderBuffer> > reorder_buffers_;
    
    // TS解析器和处理器
    std::unique_ptr<SrsTsContext> ts_context_;
    std::unique_ptr<FrameToSourceBridge> source_bridge_;
    
    
    // TS处理器适配器（实现ISrsTsHandler，复用SrsMpegtsOverUdp的FLV转换逻辑）
    class TsHandlerAdapter : public ISrsTsHandler {
    public:
        QuicFecTsAdapter* adapter_;
        
        TsHandlerAdapter(QuicFecTsAdapter* adapter);
        virtual ~TsHandlerAdapter();
        
        virtual srs_error_t on_ts_message(SrsTsMessage* msg) override;
        
    private:
        // 复用 SrsMpegtsOverUdp 的 TS 处理逻辑
        srs_error_t on_ts_video(SrsTsMessage* msg, SrsBuffer* avs);
        srs_error_t on_ts_video_hevc(SrsTsMessage* msg, SrsBuffer* avs);
        srs_error_t on_ts_audio(SrsTsMessage* msg, SrsBuffer* avs);
        
        // 复用 SrsMpegtsOverUdp 的 FLV 转换逻辑
        srs_error_t write_h264_sps_pps(uint32_t dts, uint32_t pts);
        srs_error_t write_h264_ipb_frame(char *frame, int frame_size, uint32_t dts, uint32_t pts);
        srs_error_t write_h265_vps_sps_pps(uint32_t dts, uint32_t pts);
        srs_error_t write_h265_ipb_frame(char *frame, int frame_size, uint32_t dts, uint32_t pts);
        srs_error_t write_audio_raw_frame(char *frame, int frame_size, SrsRawAacStreamCodec *codec, uint32_t dts);
        
        // 推送到 live source（替代 rtmp_write_packet）
        srs_error_t push_to_live_source(SrsFrameType type, uint32_t timestamp, char *data, int size);
        
        // H264/AAC 处理工具（复用 SrsMpegtsOverUdp 的成员）
        std::unique_ptr<SrsRawH264Stream> avc_;
        std::string h264_sps_;
        bool h264_sps_changed_;
        std::string h264_pps_;
        bool h264_pps_changed_;
        bool h264_sps_pps_sent_;
        
        // H265/HEVC 处理工具
        std::unique_ptr<SrsRawHEVCStream> hevc_;
        std::string h265_vps_;
        std::string h265_sps_;
        std::string h265_pps_;
        bool h265_vps_sps_pps_changed_;
        bool h265_vps_sps_pps_sent_;
        
        std::unique_ptr<SrsRawAacStream> aac_;
        std::string aac_specific_config_;
    };
    std::unique_ptr<TsHandlerAdapter> ts_handler_;
    
    // 回调
    OnStartStreamCallback on_start_stream_;
    OnStopStreamCallback on_stop_stream_;
    
    // 流状态
    bool stream_started_;
    std::string connection_id_;
    
    // 输入缓冲区（用于协议探测和裸TS模式）
    std::vector<uint8_t> input_buffer_;
    std::mutex input_buffer_mutex_;
};

