#include "frame_to_source_bridge.hpp"
#include <srs_kernel_log.hpp>
#include <srs_kernel_codec.hpp>
#include <cstring>

FrameToSourceBridge::FrameToSourceBridge() {
}

FrameToSourceBridge::~FrameToSourceBridge() {
    close();
}

srs_error_t FrameToSourceBridge::initialize(const std::string& vhost, const std::string& app, const std::string& stream) {
    srs_error_t err = srs_success;
    
    vhost_ = vhost;
    app_ = app;
    stream_ = stream;
    
    if ((err = create_source()) != srs_success) {
        return srs_error_wrap(err, "create source");
    }
    
    return err;
}

srs_error_t FrameToSourceBridge::create_source() {
    srs_error_t err = srs_success;
    
    if (!_srs_sources) {
        return srs_error_new(ERROR_SYSTEM_IO_INVALID, "Source manager not ready");
    }
    
    SrsRequest* req = new SrsRequest();
    req->vhost_ = vhost_;
    req->app_ = app_;
    req->stream_ = stream_;
    req->tcUrl_ = "rtmp://" + vhost_ + "/" + app_;
    req->pageUrl_ = "";
    req->swfUrl_ = "";
    
    SrsSharedPtr<SrsLiveSource> temp_source;
    if ((err = _srs_sources->fetch_or_create(req, temp_source)) != srs_success) {
        srs_freep(req);
        return srs_error_wrap(err, "fetch_or_create source");
    }
    
    source_ = temp_source;
    srs_freep(req);
    
    return err;
}

srs_error_t FrameToSourceBridge::push_frame(const StdFrame& frame) {
    srs_error_t err = srs_success;
    
    if (!source_.get()) {
        return srs_error_new(ERROR_NO_SOURCE, "Source not initialized");
    }
    
    SrsMediaPacket* packet = NULL;
    if ((err = convert_frame_to_media_packet(frame, packet)) != srs_success) {
        return srs_error_wrap(err, "convert frame");
    }
    
    if ((err = source_->on_frame(packet)) != srs_success) {
        srs_freep(packet);
        return srs_error_wrap(err, "on_frame");
    }
    
    return err;
}

srs_error_t FrameToSourceBridge::convert_frame_to_media_packet(const StdFrame& frame, SrsMediaPacket*& packet) {
    srs_error_t err = srs_success;
    
    packet = new SrsMediaPacket();
    
    // 确定帧类型
    if (frame.h.codec == "H264" || frame.h.codec == "H265") {
        packet->message_type_ = SrsFrameTypeVideo;
    } else if (frame.h.codec == "AAC" || frame.h.codec == "OPUS" || 
               frame.h.codec == "PCM_ALAW" || frame.h.codec == "PCM_ULAW") {
        packet->message_type_ = SrsFrameTypeAudio;
    } else {
        packet->message_type_ = SrsFrameTypeScript;
    }
    
    packet->timestamp_ = frame.h.dts_ms;
    
    if (!frame.payload.empty()) {
        char *buf = new char[frame.payload.size()];
        memcpy(buf, frame.payload.data(), frame.payload.size());
        packet->wrap(buf, (int)frame.payload.size());
    }
    
    return err;
}

void FrameToSourceBridge::close() {
    source_ = SrsSharedPtr<SrsLiveSource>(NULL);
}

