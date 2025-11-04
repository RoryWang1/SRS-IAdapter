#include "quic_fec_ts_adapter.hpp"
#include "../components/frame/frame_to_source_bridge.hpp"
#include "../../../core/srs_core_time.hpp"
#include "../../../app/srs_app_rtmp_source.hpp"
#include <srs_kernel_ts.hpp>
#include <srs_kernel_buffer.hpp>
#include <srs_kernel_stream.hpp>
#include <srs_kernel_codec.hpp>
#include "../../../protocol/srs_protocol_raw_avc.hpp"
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <cstdio>
#include <unistd.h>

extern SrsLiveSourceManager *_srs_sources;

QuicFecTsAdapter::QuicFecTsAdapter()
    : current_mode_(MODE_UNKNOWN), stream_started_(false) {
    
    // connection_id将在start()时从配置中读取，如果没有则生成默认值
    connection_id_ = "";
    
    // 初始化TS上下文
    ts_context_ = std::unique_ptr<SrsTsContext>(new SrsTsContext());
    source_bridge_ = std::unique_ptr<FrameToSourceBridge>(new FrameToSourceBridge());
    ts_handler_ = std::unique_ptr<TsHandlerAdapter>(new TsHandlerAdapter(this));
}

// TsHandlerAdapter构造函数 - 复用 SrsMpegtsOverUdp 的初始化逻辑
QuicFecTsAdapter::TsHandlerAdapter::TsHandlerAdapter(QuicFecTsAdapter* adapter)
    : adapter_(adapter),
      avc_(new SrsRawH264Stream()),
      h264_sps_changed_(false),
      h264_pps_changed_(false),
      h264_sps_pps_sent_(false),
      hevc_(new SrsRawHEVCStream()),
      h265_vps_sps_pps_changed_(false),
      h265_vps_sps_pps_sent_(false),
      aac_(new SrsRawAacStream()) {
}

QuicFecTsAdapter::TsHandlerAdapter::~TsHandlerAdapter() {
}

// 复用 SrsMpegtsOverUdp::on_ts_message 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::on_ts_message(SrsTsMessage* msg) {
    srs_error_t err = srs_success;
    
    if (!adapter_ || !adapter_->source_bridge_) {
        return srs_success;
    }
    
    // 检查是否支持该编解码器（复用 SrsMpegtsOverUdp 的检查逻辑）
    if (msg->channel_->stream_ != SrsTsStreamVideoH264 && 
        msg->channel_->stream_ != SrsTsStreamVideoHEVC &&
        msg->channel_->stream_ != SrsTsStreamAudioAAC) {
        return srs_error_new(ERROR_STREAM_CASTER_TS_CODEC, 
                           "ts: unsupported stream codec=%d", msg->channel_->stream_);
    }
    
    // 处理TS消息，提取音视频帧并转换为FLV格式（复用 SrsMpegtsOverUdp 的逻辑）
    // msg->payload_ 是 SrsSimpleStream*，需要访问其数据
    if (!msg->payload_) {
        return srs_success;
    }
    
    int payload_len = msg->payload_->length();
    char* payload_data = (char*)msg->payload_->bytes();
    
    SrsBuffer avs(payload_data, payload_len);
    
    // 处理视频流
    if (msg->channel_->stream_ == SrsTsStreamVideoH264) {
        if ((err = on_ts_video(msg, &avs)) != srs_success) {
            srs_warn("on_ts_video failed: %s", srs_error_desc(err).c_str());
            return srs_error_wrap(err, "ts: consume video h264");
        }
    }
    // 处理 H.265/HEVC 视频流
    else if (msg->channel_->stream_ == SrsTsStreamVideoHEVC) {
        if ((err = on_ts_video_hevc(msg, &avs)) != srs_success) {
            srs_warn("on_ts_video_hevc failed: %s", srs_error_desc(err).c_str());
            return srs_error_wrap(err, "ts: consume video hevc");
        }
    }
    // 处理音频流
    else if (msg->channel_->stream_ == SrsTsStreamAudioAAC) {
        if ((err = on_ts_audio(msg, &avs)) != srs_success) {
            srs_warn("on_ts_audio failed: %s", srs_error_desc(err).c_str());
            return srs_error_wrap(err, "ts: consume audio");
        }
    }
    
    return err;
}

QuicFecTsAdapter::~QuicFecTsAdapter() {
    close();
}

srs_error_t QuicFecTsAdapter::start(const AdapterInit& init) {
    srs_error_t err = srs_success;
    
    init_ = init;
    current_mode_ = MODE_UNKNOWN;
    stream_started_ = false;
    
    // 从配置中读取connection_id（由QuicUdpHandler传递）
    connection_id_ = init.get_param("connection_id", "");
    if (connection_id_.empty()) {
        // 如果没有提供，生成默认connection_id
        srs_utime_t now_us = srs_time_now_cached();
        int64_t now_ms = now_us / 1000;
        int64_t now_sec = now_ms / 1000;
        int ms_part = now_ms % 1000;
        
        std::stringstream ss;
        ss << "quicfec_" << now_sec 
           << "_" << std::setfill('0') << std::setw(3) << ms_part
           << "_" << (rand() % 10000);
        connection_id_ = ss.str();
    }
    
    // 读取客户端信息（用于统计和日志）
    config_.remote_address = init.get_param("client_ip", "");
    config_.remote_port = init.get_int_param("client_port", 0);
    
    // 初始化配置
    if ((err = init_config(init)) != srs_success) {
        return srs_error_wrap(err, "init config");
    }
    
    // 初始化source bridge
    if ((err = source_bridge_->initialize(init.vhost, init.app, init.stream)) != srs_success) {
        return srs_error_wrap(err, "initialize source bridge");
    }
    
    // 创建主会话的FEC管理器和乱序缓冲区
    {
        std::unique_ptr<FecRepairManager> fec_mgr(new FecRepairManager());
        fec_mgr->set_config(config_.fec_config);
        fec_mgr->set_max_groups(config_.fec_config.k * 10); // 允许10个组的缓冲
        fec_managers_[connection_id_] = std::move(fec_mgr);
    }
    
    {
        std::unique_ptr<ReorderBuffer> reorder_buf(new ReorderBuffer(config_.reorder_config));
        reorder_buffers_[connection_id_] = std::move(reorder_buf);
    }
    
    // 注册统计
    AdapterStatsManager::instance().add_connection(
        connection_id_, "quic_fec_ts", init.vhost, init.app, init.stream,
        config_.remote_address, config_.remote_port);
    
    return err;
}

srs_error_t QuicFecTsAdapter::init_config(const AdapterInit& init) {
    srs_error_t err = srs_success;
    
    // 读取配置参数
    config_.listen_address = init.get_param("listen_address", "0.0.0.0");
    config_.listen_port = init.get_int_param("listen_port", 8443);
    config_.remote_address = init.get_param("remote_address", "");
    config_.remote_port = init.get_int_param("remote_port", 0);
    
    // 验证端口范围
    if (config_.listen_port <= 0 || config_.listen_port > 65535) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid listen port: %d (must be 1-65535)", config_.listen_port);
    }
    
    // FEC配置
    config_.fec_config.k = init.get_int_param("fec_k", 8);
    config_.fec_config.n = init.get_int_param("fec_n", 12);
    config_.fec_config.repair_deadline_ms = init.get_int_param("fec_repair_deadline_ms", 100);
    config_.fec_config.enable_keyframe_relax = init.get_bool_param("fec_keyframe_relax", true);
    
    // 验证FEC配置
    if (config_.fec_config.k <= 0 || config_.fec_config.k > 255) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid FEC k: %d (must be 1-255)", config_.fec_config.k);
    }
    if (config_.fec_config.n < config_.fec_config.k || config_.fec_config.n > 255) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid FEC n: %d (must be >= k and <= 255)", config_.fec_config.n);
    }
    if (config_.fec_config.repair_deadline_ms <= 0 || config_.fec_config.repair_deadline_ms > 10000) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid FEC repair deadline: %lldms (must be 1-10000ms)", 
                           config_.fec_config.repair_deadline_ms);
    }
    
    // 乱序缓冲配置
    config_.reorder_config.reorder_window_ms = init.get_int_param("reorder_window_ms", 200);
    config_.reorder_config.enable_keyframe_relax = init.get_bool_param("reorder_keyframe_relax", true);
    config_.reorder_config.keyframe_relax_ms = init.get_int_param("reorder_keyframe_relax_ms", 100);
    config_.reorder_config.max_buffer_size = init.get_int_param("reorder_max_buffer_size", 10 * 1024 * 1024);
    
    // 验证乱序缓冲配置
    if (config_.reorder_config.reorder_window_ms <= 0 || config_.reorder_config.reorder_window_ms > 5000) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid reorder window: %lldms (must be 1-5000ms)", 
                           config_.reorder_config.reorder_window_ms);
    }
    if (config_.reorder_config.max_buffer_size < 1024 * 1024 || 
        config_.reorder_config.max_buffer_size > 500 * 1024 * 1024) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid reorder max buffer size: %zu (must be 1MB-500MB)", 
                           config_.reorder_config.max_buffer_size);
    }
    
    // 协议探测
    config_.enable_protocol_detection = init.get_bool_param("enable_protocol_detection", true);
    config_.detection_timeout_ms = init.get_int_param("detection_timeout_ms", 1000);
    
    // 验证协议探测配置
    if (config_.detection_timeout_ms <= 0 || config_.detection_timeout_ms > 10000) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid detection timeout: %lldms (must be 1-10000ms)", 
                           config_.detection_timeout_ms);
    }
    
    // 资源限制
    config_.max_sessions = init.get_int_param("max_sessions", 100);
    config_.max_buffer_size = init.get_int_param("max_buffer_size", 50 * 1024 * 1024);
    
    // 验证资源限制
    if (config_.max_sessions <= 0 || config_.max_sessions > 10000) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid max sessions: %zu (must be 1-10000)", config_.max_sessions);
    }
    if (config_.max_buffer_size < 1024 * 1024 || config_.max_buffer_size > 1024 * 1024 * 1024) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Invalid max buffer size: %zu (must be 1MB-1GB)", config_.max_buffer_size);
    }
    
    return err;
}

srs_error_t QuicFecTsAdapter::feed(const uint8_t* data, size_t nbytes) {
    srs_error_t err = srs_success;
    
    if (nbytes == 0 || data == nullptr) {
        return srs_success;
    }
    
    stats_.total_packets_received++;
    
    // 协议探测（如果还未确定）
    if (current_mode_ == MODE_UNKNOWN && config_.enable_protocol_detection) {
        if ((err = detect_protocol(data, nbytes)) != srs_success) {
            srs_warn("Protocol detection failed: %s", srs_error_desc(err).c_str());
            srs_freep(err);
        }
    }
    
    // 根据模式处理数据
    if (current_mode_ == MODE_BARE_TS) {
        // 裸TS模式：直接处理
        if ((err = handle_bare_ts_data(data, nbytes)) != srs_success) {
            return srs_error_wrap(err, "handle bare ts");
        }
    } else {
        // QUIC+FEC模式：使用QUIC库解析QUIC协议，然后FEC库处理FEC，最后输出TS包
        // 沿袭原有调用栈：feed -> handle_quic_data -> FEC修复 -> 乱序重排 -> output_ts_packets -> context_->decode
        std::string conn_id = connection_id_;
        
        // QUIC+FEC协议格式：[seq(8)][group_id(4)][block_index(2)][flags(1)][reserved(1)][payload...]
        if (nbytes < 16) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                               "QUIC+FEC packet too small: %zu bytes (minimum 16)", nbytes);
        }
        
        uint64_t seq_num = 0;
        uint32_t group_id = 0;
        uint16_t block_index = 0;
        memcpy(&seq_num, data, 8);
        memcpy(&group_id, data + 8, 4);
        memcpy(&block_index, data + 12, 2);
        uint8_t flags = data[14];
        
        bool is_parity = (flags & 0x01) != 0;
        bool is_keyframe_packet = (flags & 0x02) != 0;
        
        if (seq_num == 0 && group_id == 0) {
            srs_warn("Invalid QUIC+FEC packet: seq=0, group_id=0, dropping");
            return srs_success;
        }
        
        const uint8_t* payload = data + 16;
        size_t payload_size = nbytes - 16;
        
        if (payload_size == 0) {
            srs_warn("QUIC+FEC packet with empty payload: seq=%llu, group=%u", 
                    seq_num, group_id);
            return srs_success;
        }
        
        srs_utime_t now_us = srs_time_now_cached();
        int64_t timestamp_ms = now_us / 1000;
        
        if ((err = handle_quic_data(conn_id, payload, payload_size, 
                                     seq_num, is_parity, group_id, block_index, 
                                     timestamp_ms, is_keyframe_packet)) != srs_success) {
            return srs_error_wrap(err, "handle quic data");
        }
        
        stats_.quic_packets++;
    }
    
    // 定期清理过期会话（每100个包清理一次，避免频繁检查）
    if (stats_.total_packets_received % 100 == 0) {
        cleanup_expired_sessions();
    }
    
    // 处理FEC组和乱序缓冲（沿袭原有调用栈）
    if ((err = process_fec_groups()) != srs_success) {
        srs_warn("process_fec_groups failed: %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }
    
    // 乱序重排后输出TS包（内部调用 output_ts_packets -> context_->decode，沿袭原有调用栈）
    if ((err = process_reorder_buffer(connection_id_)) != srs_success) {
        srs_warn("process_reorder_buffer failed: %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }
    
    return err;
}

srs_error_t QuicFecTsAdapter::detect_protocol(const uint8_t* data, size_t size) {
    srs_error_t err = srs_success;
    
    if (size < 4) {
        return srs_success; // 等待更多数据
    }
    
    // 方法1: 检查是否为TS包（第一个字节是0x47，且每188字节对齐）
    if (data[0] == 0x47) {
        // 验证是否为完整的TS包序列
        bool is_ts = true;
        if (size >= SRS_TS_PACKET_SIZE) {
            // 检查第二个TS包的同步字节（如果存在）
            if (size >= SRS_TS_PACKET_SIZE * 2 && data[SRS_TS_PACKET_SIZE] != 0x47) {
                is_ts = false;
            }
        }
        
        if (is_ts) {
            current_mode_ = MODE_BARE_TS;
            stats_.bare_ts_packets++;
            return err;
        }
    }
    
    // 方法2: 检查QUIC协议特征
    // QUIC包通常以特定格式开始：
    // - 短包头: 1字节标志 (最高位通常是1)
    // - 长包头: 固定字符串 "Q" + 版本号等
    // 这里使用简化的检测逻辑
    
    // 检查QUIC短包头特征（最高位为1）
    if (size >= 1 && (data[0] & 0x80) != 0) {
        current_mode_ = MODE_QUIC_FEC;
        stats_.quic_packets++;
        return err;
    }
    
    // 检查QUIC长包头特征（可能以"Q"或其他特征开始）
    // 或者检查是否有我们自定义的QUIC+FEC协议格式
    if (size >= 16) {
        // 检查是否有合理的序列号和组ID（自定义协议格式检测）
        uint64_t seq_num = 0;
        uint32_t group_id = 0;
        memcpy(&seq_num, data, 8);
        memcpy(&group_id, data + 8, 4);
        
        // 如果序列号和组ID看起来合理（非全0，且在合理范围内）
        if ((seq_num > 0 && seq_num < UINT64_MAX / 2) || 
            (group_id > 0 && group_id < UINT32_MAX / 2)) {
            current_mode_ = MODE_QUIC_FEC;
            stats_.quic_packets++;
            return err;
        }
    }
    
    // 如果无法确定，且禁用协议探测，使用默认模式
    if (!config_.enable_protocol_detection) {
        // 根据配置选择默认模式
        std::string default_mode = init_.get_param("default_mode", "quic_fec");
        if (default_mode == "bare_ts") {
            current_mode_ = MODE_BARE_TS;
        } else {
            current_mode_ = MODE_QUIC_FEC;
        }
        return err;
    }
    
    // 继续等待更多数据以便更准确检测
    return srs_success;
}

srs_error_t QuicFecTsAdapter::handle_quic_data(const std::string& connection_id,
                                                 const uint8_t* data, size_t size,
                                                 uint64_t seq_num, bool is_parity,
                                                 uint32_t group_id, uint32_t block_index,
                                                 int64_t timestamp_ms, bool is_keyframe) {
    srs_error_t err = srs_success;
    
    // 更新连接ID（如果不同）
    if (connection_id_ != connection_id) {
        connection_id_ = connection_id;
    }
    
    // 确保会话的FEC管理器和乱序缓冲区存在
    if (fec_managers_.find(connection_id) == fec_managers_.end()) {
        std::unique_ptr<FecRepairManager> fec_mgr(new FecRepairManager());
        fec_mgr->set_config(config_.fec_config);
        fec_mgr->set_max_groups(config_.fec_config.k * 10);
        fec_managers_[connection_id] = std::move(fec_mgr);
    }
    
    if (reorder_buffers_.find(connection_id) == reorder_buffers_.end()) {
        std::unique_ptr<ReorderBuffer> reorder_buf(new ReorderBuffer(config_.reorder_config));
        reorder_buffers_[connection_id] = std::move(reorder_buf);
    }
    
        // 添加到FEC管理器（传递序列号和关键帧信息）
        if ((err = fec_managers_[connection_id]->add_block(
                group_id, block_index, data, size, is_parity, timestamp_ms, seq_num, is_keyframe)) != srs_success) {
            return srs_error_wrap(err, "add fec block");
        }
    
    return err;
}

srs_error_t QuicFecTsAdapter::handle_bare_ts_data(const uint8_t* data, size_t size) {
    srs_error_t err = srs_success;
    
    // 沿袭 SrsMpegtsOverUdp::on_udp_bytes 的处理逻辑：
    // 1. 累积数据到 buffer
    // 2. 查找 sync byte (0x47)
    // 3. 按 188 字节切分 TS 包
    // 4. 调用 output_ts_packets（内部调用 context_->decode）
    
    {
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        // 累积数据到 buffer
        input_buffer_.insert(input_buffer_.end(), data, data + size);
    }
    
    // 查找 sync byte 并处理
    std::vector<std::vector<uint8_t> > packets;
    {
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        
        // 查找 sync byte (0x47)
        size_t sync_pos = 0;
        for (size_t i = 0; i < input_buffer_.size(); i++) {
            if (input_buffer_[i] == 0x47) {
                sync_pos = i;
                break;
            }
        }
        
        // 删除 sync byte 之前的字节
        if (sync_pos > 0) {
            input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + sync_pos);
        }
        
        // 确保至少有 188 字节（一个完整 TS 包）
        if (input_buffer_.size() < SRS_TS_PACKET_SIZE) {
            return srs_success; // 等待更多数据
        }
        
        // 按 188 字节切分 TS 包
        size_t nb_packet = input_buffer_.size() / SRS_TS_PACKET_SIZE;
        for (size_t i = 0; i < nb_packet; i++) {
            size_t pos = i * SRS_TS_PACKET_SIZE;
            std::vector<uint8_t> packet(
                input_buffer_.begin() + pos,
                input_buffer_.begin() + pos + SRS_TS_PACKET_SIZE
            );
            packets.push_back(std::move(packet));
        }
        
        // 删除已处理的字节
        if (nb_packet > 0) {
            input_buffer_.erase(
                input_buffer_.begin(),
                input_buffer_.begin() + nb_packet * SRS_TS_PACKET_SIZE
            );
        }
    }
    
    // 调用 output_ts_packets（内部会调用 context_->decode，沿袭原有调用栈）
    if (!packets.empty()) {
        if ((err = output_ts_packets(packets)) != srs_success) {
            return srs_error_wrap(err, "output ts packets");
        }
    }
    
    return err;
}

srs_error_t QuicFecTsAdapter::process_fec_groups() {
    srs_error_t err = srs_success;
    
    // 遍历所有会话的FEC管理器
    for (auto& pair : fec_managers_) {
        const std::string& conn_id = pair.first;
        FecRepairManager* fec_mgr = pair.second.get();
        
        // 使用带元数据的修复方法，保留seq_num和is_keyframe信息
        std::vector<std::vector<uint8_t> > restored_data;
        std::vector<uint64_t> seq_nums;
        std::vector<bool> is_keyframes;
        
        // 检查并修复FEC组（带元数据）
        if ((err = fec_mgr->check_and_repair_with_metadata(
                restored_data, seq_nums, is_keyframes)) != srs_success) {
            srs_warn("FEC repair with metadata failed for connection %s: %s", 
                    conn_id.c_str(), srs_error_desc(err).c_str());
            srs_freep(err);
            continue;
        }
        
        if (!restored_data.empty()) {
            stats_.fec_repaired_packets += restored_data.size();
            
            // 将修复后的数据添加到乱序缓冲区
            if (reorder_buffers_.find(conn_id) != reorder_buffers_.end()) {
                ReorderBuffer* reorder_buf = reorder_buffers_[conn_id].get();
                
                srs_utime_t now_us = srs_time_now_cached();
                int64_t timestamp_ms = now_us / 1000;
                
                // 将修复后的数据添加到乱序缓冲区（使用保留的元数据）
                static uint64_t global_seq_counter = 0; // 使用全局连续序列号
                for (size_t i = 0; i < restored_data.size(); i++) {
                    const auto& data = restored_data[i];
                    // 使用全局连续序列号，而不是原始 QUIC 序列号（可能不连续）
                    uint64_t seq_num = ++global_seq_counter;
                    bool is_keyframe = (i < is_keyframes.size()) ? is_keyframes[i] : false;
                    
                    if ((err = reorder_buf->add_packet(seq_num, data.data(), 
                                                        data.size(), timestamp_ms, is_keyframe)) != srs_success) {
                        srs_warn("Failed to add packet to reorder buffer: %s",
                                srs_error_desc(err).c_str());
                        srs_freep(err);
                    }
                }
            }
        }
    }
    
    return err;
}

srs_error_t QuicFecTsAdapter::process_reorder_buffer(const std::string& connection_id) {
    srs_error_t err = srs_success;
    
    if (reorder_buffers_.find(connection_id) == reorder_buffers_.end()) {
        return srs_success;
    }
    
    ReorderBuffer* reorder_buf = reorder_buffers_[connection_id].get();
    
    if (!reorder_buf->has_ready_packets()) {
        return srs_success;
    }
    
    srs_utime_t now_us = srs_time_now_cached();
    int64_t current_time_ms = now_us / 1000;
    
    std::vector<std::vector<uint8_t> > ordered_packets;
    
    if ((err = reorder_buf->get_ordered_packets(ordered_packets, current_time_ms)) != srs_success) {
        return srs_error_wrap(err, "get ordered packets");
    }
    
    if (!ordered_packets.empty()) {
        stats_.reordered_packets += ordered_packets.size();
        
        if ((err = output_ts_packets(ordered_packets)) != srs_success) {
            return srs_error_wrap(err, "output ts packets");
        }
    }
    
    return err;
}

srs_error_t QuicFecTsAdapter::output_ts_packets(const std::vector<std::vector<uint8_t> >& packets) {
    srs_error_t err = srs_success;
    
    if (packets.empty()) {
        return srs_success;
    }
    
    // 触发流开始回调
    if (!stream_started_) {
        stream_started_ = true;
        AdapterStatsManager::instance().update_first_frame_time(connection_id_);
        if (on_start_stream_) {
            on_start_stream_(init_.vhost, init_.app, init_.stream);
        }
    }
    
    // 使用SrsTsContext解析TS包
    for (std::vector<std::vector<uint8_t> >::const_iterator it = packets.begin(); it != packets.end(); ++it) {
        const std::vector<uint8_t>& packet = *it;
        if (packet.size() != SRS_TS_PACKET_SIZE) {
            srs_warn("Invalid TS packet size: %zu, expected %d", 
                    packet.size(), SRS_TS_PACKET_SIZE);
            continue;
        }
        
        // 解析PID用于判断是否为视频流
        uint16_t pid = ((packet[1] & 0x1f) << 8) | packet[2];
        
        // 创建SrsBuffer包装TS包
        SrsUniquePtr<SrsBuffer> stream(new SrsBuffer((char*)packet.data(), SRS_TS_PACKET_SIZE));
        
        // 使用TS上下文解析包，可能会返回完成的message并调用on_ts_message
        if ((err = ts_context_->decode(stream.get(), ts_handler_.get())) != srs_success) {
            srs_freep(err);
            continue;
        }
        
        // 对于视频PID，如果PES_packet_length=0且累积了足够数据，触发处理
        // TS parser会自动累积数据到channel->msg_->payload_，当PES_packet_length=0时
        // 需要触发on_ts_message来完成PES message处理
        if (pid == 0x0100) {
            SrsTsChannel* channel = ts_context_->get(pid);
            if (channel && channel->msg_ && channel->msg_->PES_packet_length_ == 0) {
                size_t accumulated_size = channel->msg_->payload_ ? channel->msg_->payload_->length() : 0;
                
                // 每累积约1300字节（约7个packet）触发一次处理
                static size_t last_processed_size = 0;
                if (accumulated_size >= 1300 && accumulated_size > last_processed_size + 1000) {
                    SrsTsMessage* msg = channel->msg_;
                    if ((err = ts_handler_->on_ts_message(msg)) != srs_success) {
                        srs_warn("on_ts_message failed: %s", srs_error_desc(err).c_str());
                        srs_freep(err);
                    }
                    
                    // 重置message，继续累积新的数据
                    channel->msg_ = NULL;
                    srs_freep(msg);
                    last_processed_size = accumulated_size;
                }
            }
        }
    }
    
    // 更新统计
    AdapterStatsManager::instance().update_frame_stats(
        connection_id_, true, false, false); // 假设是视频包
    
    return err;
}

srs_error_t QuicFecTsAdapter::parseFrame() {
    // 这个接口主要用于帧级别的解析，对于TS流，feed接口已经处理了
    return srs_success;
}

srs_error_t QuicFecTsAdapter::flush() {
    srs_error_t err = srs_success;
    
    // 强制处理所有FEC组和乱序缓冲区
    if ((err = process_fec_groups()) != srs_success) {
        return err;
    }
    
    if ((err = process_reorder_buffer(connection_id_)) != srs_success) {
        return err;
    }
    
    // 清理输入缓冲区
    {
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        input_buffer_.clear();
    }
    
    return err;
}

void QuicFecTsAdapter::close() {
    if (stream_started_ && on_stop_stream_) {
        on_stop_stream_();
    }
    
    // 清理所有会话
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }
    
    fec_managers_.clear();
    reorder_buffers_.clear();
    
    {
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        input_buffer_.clear();
    }
    
    if (source_bridge_) {
        source_bridge_->close();
    }
    
    ts_context_.reset();
    ts_handler_.reset();
    source_bridge_.reset();
    
    if (!connection_id_.empty()) {
        AdapterStatsManager::instance().remove_connection(connection_id_);
    }
    
    stream_started_ = false;
    current_mode_ = MODE_UNKNOWN;
}

void QuicFecTsAdapter::setOnStartStream(OnStartStreamCallback callback) {
    on_start_stream_ = callback;
}

void QuicFecTsAdapter::setOnStopStream(OnStopStreamCallback callback) {
    on_stop_stream_ = callback;
}

srs_error_t QuicFecTsAdapter::create_quic_session(const std::string& connection_id) {
    // QUIC 会话由 QuicUdpHandler 管理，这里不需要实现
    return srs_success;
}

srs_error_t QuicFecTsAdapter::remove_quic_session(const std::string& connection_id) {
    // QUIC 会话由 QuicUdpHandler 管理，这里不需要实现
    return srs_success;
}

void QuicFecTsAdapter::cleanup_expired_sessions() {
    srs_utime_t now_ms = srs_time_now_cached() / 1000;
    int64_t session_timeout_ms = 300000; // 5分钟超时
    
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    // 清理过期的QUIC会话（如果有）
    std::vector<std::string> expired_sessions;
    for (auto& pair : sessions_) {
        const std::string& id = pair.first;
        QuicSessionInfo* session = pair.second.get();
        if (session && !session->is_active) {
            continue; // 已经标记为非活跃
        }
        if (session && (now_ms - session->last_activity_ms > session_timeout_ms)) {
            expired_sessions.push_back(id);
        }
    }
    
    for (std::vector<std::string>::iterator it = expired_sessions.begin(); it != expired_sessions.end(); ++it) {
        std::string id = *it;
        sessions_.erase(id);
    }
    
    // 清理过期的FEC管理器（对于已经关闭的连接）
    std::vector<std::string> expired_fec;
    for (auto& pair : fec_managers_) {
        const std::string& id = pair.first;
        // 如果对应的会话已过期，清理FEC管理器
        if (sessions_.find(id) == sessions_.end() && id != connection_id_) {
            expired_fec.push_back(id);
        }
    }
    
    for (std::vector<std::string>::iterator it = expired_fec.begin(); it != expired_fec.end(); ++it) {
        std::string id = *it;
        fec_managers_.erase(id);
        reorder_buffers_.erase(id);
    }
}

// 复用 SrsMpegtsOverUdp::on_ts_video 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::on_ts_video(SrsTsMessage* msg, SrsBuffer* avs) {
    srs_error_t err = srs_success;
    
    if (!adapter_ || !adapter_->source_bridge_) {
        return srs_success;
    }
    
    // ts tbn to flv tbn (复用 SrsMpegtsOverUdp 的逻辑)
    uint32_t dts = (uint32_t)(msg->dts_ / 90);
    uint32_t pts = (uint32_t)(msg->pts_ > 0 ? msg->pts_ / 90 : dts);
    
    // send each frame (复用 SrsMpegtsOverUdp 的 demux 逻辑)
    while (!avs->empty()) {
        char *frame = NULL;
        int frame_size = 0;
        int avs_size_before = avs->size();
        if ((err = avc_->annexb_demux(avs, &frame, &frame_size)) != srs_success) {
            srs_warn("annexb_demux failed: avs_size=%d->%d, %s", 
                    avs_size_before, avs->size(), srs_error_desc(err).c_str());
            srs_freep(err);
            // 如果 avs 大小没变，可能是格式问题，需要跳出循环避免死循环
            if (avs->size() == avs_size_before) {
                srs_warn("annexb_demux failed and avs size unchanged, breaking loop");
                break;
            }
            continue; // 继续处理下一个
        }
        
        if (frame_size == 0) {
            continue;
        }
        
        // 5bits, 7.3.1 NAL unit syntax
        SrsAvcNaluType nal_unit_type = (SrsAvcNaluType)(frame[0] & 0x1f);
        
        // ignore the nalu type sps(7), pps(8), aud(9)
        if (nal_unit_type == SrsAvcNaluTypeAccessUnitDelimiter) {
            continue;
        }
        
        // for sps
        if (avc_->is_sps(frame, frame_size)) {
            std::string sps;
            if ((err = avc_->sps_demux(frame, frame_size, sps)) != srs_success) {
                return srs_error_wrap(err, "demux sps");
            }
            
            if (h264_sps_ == sps) {
                continue;
            }
            h264_sps_changed_ = true;
            h264_sps_ = sps;
            
            if ((err = write_h264_sps_pps(dts, pts)) != srs_success) {
                return srs_error_wrap(err, "write sps/pps");
            }
            continue;
        }
        
        // for pps
        if (avc_->is_pps(frame, frame_size)) {
            std::string pps;
            if ((err = avc_->pps_demux(frame, frame_size, pps)) != srs_success) {
                return srs_error_wrap(err, "demux pps");
            }
            
            if (h264_pps_ == pps) {
                continue;
            }
            h264_pps_changed_ = true;
            h264_pps_ = pps;
            
            if ((err = write_h264_sps_pps(dts, pts)) != srs_success) {
                return srs_error_wrap(err, "write sps/pps");
            }
            continue;
        }
        
        // ibp frame (复用 SrsMpegtsOverUdp 的逻辑)
        if ((err = write_h264_ipb_frame(frame, frame_size, dts, pts)) != srs_success) {
            // 如果是因为缺少 SPS/PPS 而 drop，记录但不返回错误，继续处理
            srs_error_t drop_err = err;
            std::string err_desc = srs_error_desc(drop_err);
            if (err_desc.find("drop") != std::string::npos || err_desc.find("sps/pps") != std::string::npos) {
                srs_freep(err);
                continue; // 继续处理下一个帧
            }
            return srs_error_wrap(err, "write frame");
        }
    }
    
    return err;
}

// 复用 SrsMpegtsOverUdp::on_ts_audio 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::on_ts_audio(SrsTsMessage* msg, SrsBuffer* avs) {
    srs_error_t err = srs_success;
    
    if (!adapter_ || !adapter_->source_bridge_) {
        return srs_success;
    }
    
    // ts tbn to flv tbn (复用 SrsMpegtsOverUdp 的逻辑)
    uint32_t dts = (uint32_t)(msg->dts_ / 90);
    
    // send each frame (复用 SrsMpegtsOverUdp 的 demux 逻辑)
    while (!avs->empty()) {
        char *frame = NULL;
        int frame_size = 0;
        SrsRawAacStreamCodec codec;
        if ((err = aac_->adts_demux(avs, &frame, &frame_size, codec)) != srs_success) {
            return srs_error_wrap(err, "demux adts");
        }
        
        // ignore invalid frame
        if (frame_size <= 0) {
            continue;
        }
        
        // generate sh (复用 SrsMpegtsOverUdp 的逻辑)
        if (aac_specific_config_.empty()) {
            std::string sh;
            if ((err = aac_->mux_sequence_header(&codec, sh)) != srs_success) {
                return srs_error_wrap(err, "mux sequence header");
            }
            aac_specific_config_ = sh;
            
            codec.aac_packet_type_ = 0;
            
            if ((err = write_audio_raw_frame((char *)sh.data(), (int)sh.length(), &codec, dts)) != srs_success) {
                return srs_error_wrap(err, "write raw audio frame");
            }
        }
        
        // audio raw data (复用 SrsMpegtsOverUdp 的逻辑)
        codec.aac_packet_type_ = 1;
        if ((err = write_audio_raw_frame(frame, frame_size, &codec, dts)) != srs_success) {
            return srs_error_wrap(err, "write audio raw frame");
        }
    }
    
    return err;
}

// 复用 SrsGbMuxer::mux_h265 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::on_ts_video_hevc(SrsTsMessage* msg, SrsBuffer* avs) {
    srs_error_t err = srs_success;
    
    if (!adapter_ || !adapter_->source_bridge_) {
        return srs_success;
    }
    
    // ts tbn to flv tbn (复用 SrsGbMuxer 的逻辑)
    uint32_t dts = (uint32_t)(msg->dts_ / 90);
    uint32_t pts = (uint32_t)(msg->pts_ > 0 ? msg->pts_ / 90 : dts);
    
    // send each frame (复用 SrsGbMuxer 的 demux 逻辑)
    while (!avs->empty()) {
        char *frame = NULL;
        int frame_size = 0;
        if ((err = hevc_->annexb_demux(avs, &frame, &frame_size)) != srs_success) {
            return srs_error_wrap(err, "demux hevc annexb");
        }
        
        // 6bits, 7.4.2.2 NAL unit header semantics
        // ITU-T-H.265-2021.pdf, page 85.
        // 32: VPS, 33: SPS, 34: PPS ...
        SrsHevcNaluType nt = SrsHevcNaluTypeParse(frame[0]);
        if (nt == SrsHevcNaluType_SEI || nt == SrsHevcNaluType_SEI_SUFFIX || nt == SrsHevcNaluType_ACCESS_UNIT_DELIMITER) {
            continue;
        }
        
        // for vps
        if (hevc_->is_vps(frame, frame_size)) {
            std::string vps;
            if ((err = hevc_->vps_demux(frame, frame_size, vps)) != srs_success) {
                return srs_error_wrap(err, "demux vps");
            }
            
            if (h265_vps_ == vps) {
                continue;
            }
            
            h265_vps_sps_pps_changed_ = true;
            h265_vps_ = vps;
            
            if ((err = write_h265_vps_sps_pps(dts, pts)) != srs_success) {
                return srs_error_wrap(err, "write vps");
            }
            continue;
        }
        
        // for sps
        if (hevc_->is_sps(frame, frame_size)) {
            std::string sps;
            if ((err = hevc_->sps_demux(frame, frame_size, sps)) != srs_success) {
                return srs_error_wrap(err, "demux sps");
            }
            
            if (h265_sps_ == sps) {
                continue;
            }
            h265_vps_sps_pps_changed_ = true;
            h265_sps_ = sps;
            
            if ((err = write_h265_vps_sps_pps(dts, pts)) != srs_success) {
                return srs_error_wrap(err, "write sps");
            }
            continue;
        }
        
        // for pps
        if (hevc_->is_pps(frame, frame_size)) {
            std::string pps;
            if ((err = hevc_->pps_demux(frame, frame_size, pps)) != srs_success) {
                return srs_error_wrap(err, "demux pps");
            }
            
            if (h265_pps_ == pps) {
                continue;
            }
            h265_vps_sps_pps_changed_ = true;
            h265_pps_ = pps;
            
            if ((err = write_h265_vps_sps_pps(dts, pts)) != srs_success) {
                return srs_error_wrap(err, "write pps");
            }
            continue;
        }
        
        // ibp frame
        if ((err = write_h265_ipb_frame(frame, frame_size, dts, pts)) != srs_success) {
            return srs_error_wrap(err, "write frame");
        }
    }
    
    return err;
}

// 复用 SrsMpegtsOverUdp::write_h264_sps_pps 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::write_h264_sps_pps(uint32_t dts, uint32_t pts) {
    srs_error_t err = srs_success;
    
    if (!h264_sps_changed_ || !h264_pps_changed_) {
        return err;
    }
    
    // h264 raw to h264 packet (复用 SrsMpegtsOverUdp 的逻辑)
    std::string sh;
    if ((err = avc_->mux_sequence_header(h264_sps_, h264_pps_, sh)) != srs_success) {
        srs_warn("mux_sequence_header failed: %s", srs_error_desc(err).c_str());
        return srs_error_wrap(err, "mux sequence header");
    }
    
    // h264 packet to flv packet (复用 SrsMpegtsOverUdp 的逻辑)
    int8_t frame_type = SrsVideoAvcFrameTypeKeyFrame;
    int8_t avc_packet_type = SrsVideoAvcFrameTraitSequenceHeader;
    char *flv = NULL;
    int nb_flv = 0;
    if ((err = avc_->mux_avc2flv(sh, frame_type, avc_packet_type, dts, pts, &flv, &nb_flv)) != srs_success) {
        srs_warn("mux_avc2flv failed: %s", srs_error_desc(err).c_str());
        return srs_error_wrap(err, "avc to flv");
    }
    
    // the timestamp in rtmp message header is dts
    uint32_t timestamp = dts;
    if ((err = push_to_live_source(SrsFrameTypeVideo, timestamp, flv, nb_flv)) != srs_success) {
        srs_warn("push_to_live_source failed: %s", srs_error_desc(err).c_str());
        return srs_error_wrap(err, "push sequence header");
    }
    
    // reset sps and pps (复用 SrsMpegtsOverUdp 的逻辑)
    h264_sps_changed_ = false;
    h264_pps_changed_ = false;
    h264_sps_pps_sent_ = true;
    
    return err;
}

// 复用 SrsMpegtsOverUdp::write_h264_ipb_frame 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::write_h264_ipb_frame(char *frame, int frame_size, uint32_t dts, uint32_t pts) {
    srs_error_t err = srs_success;
    
    // when sps or pps not sent, ignore the packet (复用 SrsMpegtsOverUdp 的逻辑)
    if (!h264_sps_pps_sent_) {
        return srs_error_new(ERROR_H264_DROP_BEFORE_SPS_PPS, "drop sps/pps");
    }
    
    // 5bits, 7.3.1 NAL unit syntax (复用 SrsMpegtsOverUdp 的逻辑)
    SrsAvcNaluType nal_unit_type = (SrsAvcNaluType)(frame[0] & 0x1f);
    
        // for IDR frame, the frame is keyframe (复用 SrsMpegtsOverUdp 的逻辑)
        SrsVideoAvcFrameType frame_type = SrsVideoAvcFrameTypeInterFrame;
        if (nal_unit_type == SrsAvcNaluTypeIDR) {
            frame_type = SrsVideoAvcFrameTypeKeyFrame;
        }
        
        std::string ibp;
        if ((err = avc_->mux_ipb_frame(frame, frame_size, ibp)) != srs_success) {
            srs_warn("mux_ipb_frame failed: %s", srs_error_desc(err).c_str());
            return srs_error_wrap(err, "mux frame");
        }
        
        int8_t avc_packet_type = SrsVideoAvcFrameTraitNALU;
        char *flv = NULL;
        int nb_flv = 0;
        if ((err = avc_->mux_avc2flv(ibp, frame_type, avc_packet_type, dts, pts, &flv, &nb_flv)) != srs_success) {
            srs_warn("mux_avc2flv failed: %s", srs_error_desc(err).c_str());
            return srs_error_wrap(err, "mux avc to flv");
        }
        
        // the timestamp in rtmp message header is dts
        uint32_t timestamp = dts;
        if ((err = push_to_live_source(SrsFrameTypeVideo, timestamp, flv, nb_flv)) != srs_success) {
            srs_warn("push_to_live_source failed: %s", srs_error_desc(err).c_str());
            return srs_error_wrap(err, "push video frame");
        }
    
    // 更新统计
    bool is_keyframe = (frame_type == SrsVideoAvcFrameTypeKeyFrame);
    AdapterStatsManager::instance().update_frame_stats(
        adapter_->connection_id_, true, is_keyframe, false);
    
    return err;
}

// 复用 SrsGbMuxer::write_h265_vps_sps_pps 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::write_h265_vps_sps_pps(uint32_t dts, uint32_t pts) {
    srs_error_t err = srs_success;
    
    if (!h265_vps_sps_pps_changed_) {
        return err;
    }
    
    if (h265_vps_.empty() || h265_sps_.empty() || h265_pps_.empty()) {
        return err;
    }
    
    std::vector<std::string> h265_pps;
    h265_pps.push_back(h265_pps_);
    
    // h265 raw to h265 packet (复用 SrsGbMuxer 的逻辑)
    std::string sh;
    if ((err = hevc_->mux_sequence_header(h265_vps_, h265_sps_, h265_pps, sh)) != srs_success) {
        return srs_error_wrap(err, "hevc mux sequence header");
    }
    
    // h265 packet to flv packet (复用 SrsGbMuxer 的逻辑)
    int8_t frame_type = SrsVideoAvcFrameTypeKeyFrame;
    int8_t hevc_packet_type = SrsVideoAvcFrameTraitSequenceHeader;
    char *flv = NULL;
    int nb_flv = 0;
    if ((err = hevc_->mux_hevc2flv(sh, frame_type, hevc_packet_type, dts, pts, &flv, &nb_flv)) != srs_success) {
        return srs_error_wrap(err, "hevc to flv");
    }
    
    // the timestamp in rtmp message header is dts
    uint32_t timestamp = dts;
    if ((err = push_to_live_source(SrsFrameTypeVideo, timestamp, flv, nb_flv)) != srs_success) {
        return srs_error_wrap(err, "push sequence header");
    }
    
    // reset vps/sps/pps (复用 SrsGbMuxer 的逻辑)
    h265_vps_sps_pps_changed_ = false;
    h265_vps_sps_pps_sent_ = true;
    
    return err;
}

// 复用 SrsGbMuxer::write_h265_ipb_frame 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::write_h265_ipb_frame(char *frame, int frame_size, uint32_t dts, uint32_t pts) {
    srs_error_t err = srs_success;
    
    // when vps/sps/pps not sent, ignore the packet (复用 SrsGbMuxer 的逻辑)
    if (!h265_vps_sps_pps_sent_) {
        return srs_error_new(ERROR_H264_DROP_BEFORE_SPS_PPS, "drop for no vps/sps/pps");
    }
    
    SrsHevcNaluType nt = SrsHevcNaluTypeParse(frame[0]);
    
    // F.3.29 intra random access point (IRAP) picture
    // ITU-T-H.265-2021.pdf, page 462.
    SrsVideoAvcFrameType frame_type = SrsVideoAvcFrameTypeInterFrame;
    if (SrsIsIRAP(nt)) {
        frame_type = SrsVideoAvcFrameTypeKeyFrame;
    }
    
    std::string ipb;
    if ((err = hevc_->mux_ipb_frame(frame, frame_size, ipb)) != srs_success) {
        return srs_error_wrap(err, "hevc mux ipb frame");
    }
    
    int8_t hevc_packet_type = SrsVideoAvcFrameTraitNALU;
    char *flv = NULL;
    int nb_flv = 0;
    if ((err = hevc_->mux_hevc2flv(ipb, frame_type, hevc_packet_type, dts, pts, &flv, &nb_flv)) != srs_success) {
        return srs_error_wrap(err, "mux hevc to flv");
    }
    
    // the timestamp in rtmp message header is dts
    uint32_t timestamp = dts;
    if ((err = push_to_live_source(SrsFrameTypeVideo, timestamp, flv, nb_flv)) != srs_success) {
        return srs_error_wrap(err, "push video frame");
    }
    
    // 更新统计
    bool is_keyframe = (frame_type == SrsVideoAvcFrameTypeKeyFrame);
    AdapterStatsManager::instance().update_frame_stats(
        adapter_->connection_id_, true, is_keyframe, false);
    
    return err;
}

// 复用 SrsMpegtsOverUdp::write_audio_raw_frame 的逻辑
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::write_audio_raw_frame(char *frame, int frame_size, SrsRawAacStreamCodec *codec, uint32_t dts) {
    srs_error_t err = srs_success;
    
    char *data = NULL;
    int size = 0;
    if ((err = aac_->mux_aac2flv(frame, frame_size, codec, dts, &data, &size)) != srs_success) {
        return srs_error_wrap(err, "mux aac to flv");
    }
    
    if ((err = push_to_live_source(SrsFrameTypeAudio, dts, data, size)) != srs_success) {
        return srs_error_wrap(err, "push audio frame");
    }
    
    // 更新统计
    AdapterStatsManager::instance().update_frame_stats(
        adapter_->connection_id_, false, false, false);
    
    return err;
}

// 推送到 live source（替代 SrsMpegtsOverUdp::rtmp_write_packet）
srs_error_t QuicFecTsAdapter::TsHandlerAdapter::push_to_live_source(SrsFrameType type, uint32_t timestamp, char *data, int size) {
    srs_error_t err = srs_success;
    
    if (!adapter_ || !adapter_->source_bridge_) {
        srs_warn("Source bridge not initialized");
        return srs_error_new(ERROR_NO_SOURCE, "Source bridge not initialized");
    }
    
    // 获取 live source
    SrsSharedPtr<SrsLiveSource> source = adapter_->source_bridge_->get_source();
    if (!source.get()) {
        srs_warn("Source not available");
        return srs_error_new(ERROR_NO_SOURCE, "Source not available");
    }
    
    // 创建 SrsMediaPacket（FLV 格式数据，复用 SrsMpegtsOverUdp 的逻辑）
    SrsMediaPacket *msg = new SrsMediaPacket();
    msg->message_type_ = type;
    msg->timestamp_ = timestamp;
    
    // 复制 FLV 数据
    char *buf = new char[size];
    memcpy(buf, data, size);
    msg->wrap(buf, size);
    
    // 推送到 live source（替代 RTMP 推送）
    if ((err = source->on_frame(msg)) != srs_success) {
        srs_freep(msg);
        return srs_error_wrap(err, "on_frame");
    }
    
    return err;
}
