#pragma once
#include "../../common/std_frame.hpp"
#include <srs_app_rtmp_source.hpp>
#include <srs_app_factory.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_packet.hpp>
#include <memory>
#include <string>

extern SrsLiveSourceManager *_srs_sources;

// Source 创建器 - 负责创建 SrsLiveSource 和推送帧（保留 push_frame 供 myproto_adapter 使用）
// quic_fec_ts_adapter 直接使用 get_source() 获取 source 并推送 FLV 格式数据
class FrameToSourceBridge {
public:
    FrameToSourceBridge();
    ~FrameToSourceBridge();
    
    srs_error_t initialize(const std::string& vhost, const std::string& app, const std::string& stream);
    SrsSharedPtr<SrsLiveSource> get_source() const { return source_; }
    
    // 保留给 myproto_adapter 使用（使用 StdFrame）
    srs_error_t push_frame(const StdFrame& frame);
    
    void close();

private:
    SrsSharedPtr<SrsLiveSource> source_;
    std::string vhost_;
    std::string app_;
    std::string stream_;
    
    srs_error_t create_source();
    srs_error_t convert_frame_to_media_packet(const StdFrame& frame, SrsMediaPacket*& packet);
};
