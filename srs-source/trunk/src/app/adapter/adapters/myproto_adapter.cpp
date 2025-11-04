#include "myproto_adapter.hpp"
#include "../components/frame/frame_bus.hpp"
#include "../components/parameter/parameter_set_manager.hpp"
#include "../components/frame/jitter_buffer.hpp"
#include "../components/frame/frame_to_source_bridge.hpp"
#include "../core/adapter_stats.hpp"
#include <srs_kernel_log.hpp>
#include <srs_kernel_buffer.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_kernel_error.hpp>
#include "../../../core/srs_core_time.hpp"
#include "../../../protocol/srs_protocol_raw_avc.hpp"
#include <sstream>
#include <iomanip>
#include <cstdlib>

// MyProtoAdapter implementation

MyProtoAdapter::MyProtoAdapter() 
    : state_(STATE_HEADER), buffer_(nullptr), stream_started_(false),
      last_dts_ms_(0), last_pts_ms_(0), base_timestamp_ms_(0), 
      has_base_timestamp_(false), b_frame_delay_ms_(40), client_port_(0),
      hot_start_(false), drop_b_in_low_latency_(false) {
    buffer_ = nullptr;
    param_manager_.reset(new ParameterSetManager());
    source_bridge_.reset(new FrameToSourceBridge());
    
    srs_utime_t now_us = srs_time_now_cached();
    int64_t now_ms = now_us / 1000;
    int64_t now_sec = now_ms / 1000;
    int ms_part = now_ms % 1000;
    
    std::stringstream ss;
    ss << "myproto_" << now_sec 
       << "_" << std::setfill('0') << std::setw(3) << ms_part
       << "_" << (rand() % 10000);
    connection_id_ = ss.str();

    last_frame_wallclock_ms_ = 0;
    heartbeat_interval_ms_ = 5000;
    drop_b_in_low_latency_ = false;
}

MyProtoAdapter::~MyProtoAdapter() {
    close();
}

srs_error_t MyProtoAdapter::start(const AdapterInit& init) {
    init_ = init;
    state_ = STATE_HEADER;
    stream_started_ = false;
    has_base_timestamp_ = false;
    last_dts_ms_ = 0;
    last_pts_ms_ = 0;
    b_frame_buffer_.clear();
    
    b_frame_delay_ms_ = init.get_int_param("b_frame_delay_ms", 40);
    heartbeat_interval_ms_ = init.get_int_param("heartbeat_interval_ms", 5000);
    drop_b_in_low_latency_ = init.get_bool_param("low_latency_drop_b", false);
    hot_start_ = init.get_bool_param("hot_start", false);
    
    srs_error_t err = srs_success;
    if ((err = source_bridge_->initialize(init.vhost, init.app, init.stream)) != srs_success) {
        return srs_error_wrap(err, "initialize source bridge");
    }
    
    AdapterStatsManager::instance().add_connection(
        connection_id_, "myproto", init.vhost, init.app, init.stream, client_ip_, client_port_);
    
    srs_trace("MyProto adapter started: %s/%s/%s, B-frame delay: %lldms, heartbeat: %lldms, connection: %s", 
              init.vhost.c_str(), init.app.c_str(), init.stream.c_str(), b_frame_delay_ms_, heartbeat_interval_ms_, connection_id_.c_str());
    
    return err;
}

srs_error_t MyProtoAdapter::feed(const uint8_t* data, size_t nbytes) {
    srs_error_t err = srs_success;
    
    srs_utime_t now_us = srs_time_now_cached();
    int64_t now_ms = now_us / 1000;
    if (last_frame_wallclock_ms_ > 0 && heartbeat_interval_ms_ > 0) {
        if (now_ms - last_frame_wallclock_ms_ > heartbeat_interval_ms_) {
            if (stream_started_) {
                stream_started_ = false;
                if (on_stop_stream_) on_stop_stream_();
                srs_warn("myproto heartbeat timeout, pause stream until keyframe. gap=%lldms", now_ms - last_frame_wallclock_ms_);
            }
        }
    }
    
    input_buffer_.insert(input_buffer_.end(), data, data + nbytes);
    
    if (input_buffer_.empty()) {
        return srs_success;
    }
    srs_freep(buffer_);
    buffer_ = new SrsBuffer((char*)input_buffer_.data(), (int)input_buffer_.size());
    
    while (buffer_->left() > 0) {
        switch (state_) {
            case STATE_HEADER:
                if ((err = parseHeader()) != srs_success) {
                    return err;
                }
                break;
                
            case STATE_PAYLOAD:
                if ((err = parsePayload()) != srs_success) {
                    return err;
                }
                break;
                
            case STATE_COMPLETE:
                if ((err = createStdFrame()) != srs_success) {
                    return err;
                }
                state_ = STATE_HEADER;
                break;
        }
    }
    
    int processed = buffer_->pos();
    if (processed > 0) {
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + processed);
    }
    
    return err;
}

srs_error_t MyProtoAdapter::parseHeader() {
    if (buffer_->left() < (int)sizeof(MyProtoFrame)) {
        return srs_success;
    }
    
    frame_header_.magic = buffer_->read_4bytes();
    frame_header_.length = buffer_->read_4bytes();
    frame_header_.type = buffer_->read_1bytes();
    frame_header_.codec = buffer_->read_1bytes();
    frame_header_.timestamp = buffer_->read_8bytes();
    frame_header_.flags = buffer_->read_1bytes();
    frame_header_.width = buffer_->read_4bytes();
    frame_header_.height = buffer_->read_4bytes();
    frame_header_.sample_rate = buffer_->read_4bytes();
    frame_header_.channels = buffer_->read_4bytes();
    
    if (frame_header_.magic != 0x12345678) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "Invalid magic number");
    }
    
    if (frame_header_.length > 1024 * 1024) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "Payload too large");
    }
    
    payload_buffer_.clear();
    payload_buffer_.reserve(frame_header_.length);
    state_ = STATE_PAYLOAD;
    
    return srs_success;
}

srs_error_t MyProtoAdapter::parsePayload() {
    size_t needed = frame_header_.length - payload_buffer_.size();
    int available = buffer_->left();
    size_t to_read = std::min(needed, (size_t)available);
    
    if (to_read > 0) {
        payload_buffer_.insert(payload_buffer_.end(), 
                              buffer_->head(), 
                              buffer_->head() + to_read);
        buffer_->skip((int)to_read);
    }
    
    if (payload_buffer_.size() >= frame_header_.length) {
        state_ = STATE_COMPLETE;
    }
    
    return srs_success;
}

srs_error_t MyProtoAdapter::createStdFrame() {
    srs_error_t err = srs_success;
    
    StdFrame frame;
    frame.set_stream_id(init_.vhost, init_.app, init_.stream);
    
    int64_t timestamp_ms = frame_header_.timestamp / 1000;
    
    if (!has_base_timestamp_) {
        base_timestamp_ms_ = timestamp_ms;
        has_base_timestamp_ = true;
    }
    
    int64_t dts_ms = timestamp_ms - base_timestamp_ms_;
    int64_t pts_ms = dts_ms;
    
    bool is_b_frame = (frame_header_.flags & 0x02) != 0;
    if (is_b_frame) {
        pts_ms = dts_ms + b_frame_delay_ms_;
        stats_.b_frames++;
    } else {
        if (last_dts_ms_ > 0 && dts_ms > last_dts_ms_) {
            pts_ms = dts_ms;
        }
    }
    
    if (!validateTimestamp(dts_ms, pts_ms)) {
        stats_.invalid_timestamps++;
        srs_warn("Invalid timestamp: dts=%lld, pts=%lld", dts_ms, pts_ms);
        return srs_success;
    }
    
    frame.h.dts_ms = dts_ms;
    frame.h.pts_ms = pts_ms;
    frame.h.has_pts = true;
    
    if (frame_header_.type == 0) {
        if (frame_header_.codec == 0) {
            frame.h.codec = "H264";
        } else if (frame_header_.codec == 1) {
            frame.h.codec = "H265";
        }
        
        frame.set_video_params(frame_header_.width, frame_header_.height, 
                               (frame_header_.flags & 0x01) != 0);
        
        if (frame.h.keyframe) {
            stats_.keyframes++;
        }
        stats_.video_frames++;
    } else {
        if (frame_header_.codec == 2) {
            frame.h.codec = "AAC";
        } else if (frame_header_.codec == 3) {
            frame.h.codec = "OPUS";
        }
        
        frame.set_audio_params(frame_header_.sample_rate, frame_header_.channels);
        stats_.audio_frames++;
    }
    
    if (!payload_buffer_.empty()) {
        if (frame.h.codec == "AAC" && payload_buffer_.size() >= 7) {
            if (payload_buffer_[0] == 0xFF && (payload_buffer_[1] & 0xF0) == 0xF0) {
                int8_t protection_absent = (payload_buffer_[1] & 0x01);
                int adts_header_size = protection_absent ? 7 : 9;
                
                if (payload_buffer_.size() > adts_header_size) {
                    std::vector<uint8_t> raw_aac(payload_buffer_.begin() + adts_header_size, payload_buffer_.end());
                    frame.set_payload(raw_aac);
                } else {
                    frame.set_payload(payload_buffer_);
                    srs_warn("AAC payload too short for ADTS header removal, size: %zu", payload_buffer_.size());
                }
            } else {
                frame.set_payload(payload_buffer_);
            }
        } else {
            if ((frame.h.codec == "H264" || frame.h.codec == "H265") && payload_buffer_.size() > 0) {
                bool has_annexb_start = false;
                if (payload_buffer_.size() >= 4) {
                    has_annexb_start = (payload_buffer_[0] == 0x00 && payload_buffer_[1] == 0x00 && 
                                        payload_buffer_[2] == 0x00 && payload_buffer_[3] == 0x01);
                }
                if (!has_annexb_start && payload_buffer_.size() >= 3) {
                    has_annexb_start = (payload_buffer_[0] == 0x00 && payload_buffer_[1] == 0x00 && 
                                        payload_buffer_[2] == 0x01);
                }
                
                if (!has_annexb_start) {
                    std::vector<uint8_t> annexb_payload;
                    annexb_payload.push_back(0x00);
                    annexb_payload.push_back(0x00);
                    annexb_payload.push_back(0x00);
                    annexb_payload.push_back(0x01);
                    annexb_payload.insert(annexb_payload.end(), payload_buffer_.begin(), payload_buffer_.end());
                    frame.set_payload(annexb_payload);
                } else {
                    frame.set_payload(payload_buffer_);
                }
            } else {
                frame.set_payload(payload_buffer_);
            }
        }
        AdapterStatsManager::instance().update_zero_copy_stats(connection_id_, true);
    } else {
        AdapterStatsManager::instance().update_zero_copy_stats(connection_id_, false);
    }
    
    if ((err = processParameterSets(frame)) != srs_success) {
        return err;
    }
    
    if (!stream_started_) {
        if (hot_start_) {
            stream_started_ = true;
            AdapterStatsManager::instance().update_first_frame_time(connection_id_);
            if (on_start_stream_) {
                on_start_stream_(init_.vhost, init_.app, init_.stream);
            }
        } else {
            if (frame.h.keyframe) {
                stream_started_ = true;
                AdapterStatsManager::instance().update_first_frame_time(connection_id_);
                if (on_start_stream_) {
                    on_start_stream_(init_.vhost, init_.app, init_.stream);
                }
            } else {
                return srs_success;
            }
        }
    }
    
    if (hot_start_ && stream_started_ && frame.h.keyframe) {
        if ((err = resend_parameter_sets(frame)) != srs_success) {
            srs_warn("Failed to resend parameter sets in hot start mode: %s", srs_error_desc(err).c_str());
            srs_freep(err);
        }
    }
    
    if (is_b_frame) {
        if ((err = processBFrameTiming(frame)) != srs_success) {
            return err;
        }
    }
    
    if (drop_b_in_low_latency_ && is_b_frame && !frame.h.keyframe) {
        AdapterStatsManager::instance().update_frame_stats(connection_id_, true, false, true);
        return srs_success;
    }

    stats_.total_frames++;
    last_dts_ms_ = dts_ms;
    last_pts_ms_ = pts_ms;
    srs_utime_t now_us = srs_time_now_cached();
    last_frame_wallclock_ms_ = now_us / 1000;
    
    if ((err = source_bridge_->push_frame(frame)) != srs_success) {
        srs_warn("Failed to push frame to SrsSource: %s", srs_error_desc(err).c_str());
        srs_freep(err);
    }
    
    bool is_video = (frame.h.codec == "H264" || frame.h.codec == "H265");
    AdapterStatsManager::instance().update_frame_stats(connection_id_, is_video, frame.h.keyframe, false);
    
    return srs_success;
}

srs_error_t MyProtoAdapter::processBFrameTiming(StdFrame& frame) {
    if (frame.h.dts_ms >= frame.h.pts_ms) {
        frame.h.pts_ms = frame.h.dts_ms + b_frame_delay_ms_;
    }
    
    return srs_success;
}

srs_error_t MyProtoAdapter::processParameterSets(const StdFrame& frame) {
    if (!frame.h.keyframe || frame.payload.empty()) {
        return srs_success;
    }
    
    if (frame.h.codec == "H264") {
        const uint8_t* data = frame.payload.data();
        size_t size = frame.payload.size();
        
        for (size_t i = 0; i < size - 4; i++) {
            if (data[i] == 0x00 && data[i+1] == 0x00 && 
                data[i+2] == 0x00 && data[i+3] == 0x01) {
                
                uint8_t nalu_type = data[i+4] & 0x1F;
                
                if (nalu_type == 7) {
                    std::vector<uint8_t> sps_data(data + i + 4, data + size);
                    param_manager_->update_parameter_set(ParameterSetType::SPS, sps_data);
                    stats_.parameter_set_updates++;
                } else if (nalu_type == 8) {
                    std::vector<uint8_t> pps_data(data + i + 4, data + size);
                    param_manager_->update_parameter_set(ParameterSetType::PPS, pps_data);
                    stats_.parameter_set_updates++;
                }
            }
        }
    }
    
    return srs_success;
}

srs_error_t MyProtoAdapter::resend_parameter_sets(const StdFrame& frame) {
    srs_error_t err = srs_success;
    
    std::vector<ParameterSetInfo> param_sets = param_manager_->get_all_parameter_sets();
    
    if (param_sets.empty()) {
        return srs_success;
    }
    
    if (frame.h.codec == "H264" || frame.h.codec == "H265") {
        for (const auto& param : param_sets) {
            if (param.type == ParameterSetType::SPS || 
                param.type == ParameterSetType::PPS || 
                param.type == ParameterSetType::VPS) {
                
                StdFrame param_frame;
                param_frame.h = frame.h;
                param_frame.h.codec = frame.h.codec;
                param_frame.h.keyframe = true;
                param_frame.h.dts_ms = frame.h.dts_ms;
                param_frame.h.pts_ms = frame.h.pts_ms;
                param_frame.h.has_pts = frame.h.has_pts;
                param_frame.set_payload(param.data);
                
                if ((err = source_bridge_->push_frame(param_frame)) != srs_success) {
                    srs_warn("Failed to resend parameter set in hot start mode: %s", srs_error_desc(err).c_str());
                    srs_freep(err);
                }
            }
        }
    }
    
    return err;
}

bool MyProtoAdapter::validateTimestamp(int64_t dts_ms, int64_t pts_ms) const {
    if (dts_ms < 0 || pts_ms < 0) {
        return false;
    }
    
    if (last_dts_ms_ > 0 && dts_ms < last_dts_ms_) {
        return false;
    }
    
    if (pts_ms < dts_ms) {
        return false;
    }
    
    return true;
}

srs_error_t MyProtoAdapter::parseFrame() {
    return srs_success;
}

srs_error_t MyProtoAdapter::flush() {
    b_frame_buffer_.clear();
    return srs_success;
}

void MyProtoAdapter::close() {
    if (stream_started_ && on_stop_stream_) {
        on_stop_stream_();
    }
    
    if (buffer_) {
        delete buffer_;
        buffer_ = nullptr;
    }
    
    payload_buffer_.clear();
    b_frame_buffer_.clear();
    state_ = STATE_HEADER;
    stream_started_ = false;
    has_base_timestamp_ = false;
    
    if (param_manager_) {
        param_manager_->clear_all();
    }
    
    if (source_bridge_) {
        source_bridge_->close();
    }
    
    if (!connection_id_.empty()) {
        AdapterStatsManager::instance().remove_connection(connection_id_);
    }
}

void MyProtoAdapter::setOnStartStream(OnStartStreamCallback callback) {
    on_start_stream_ = callback;
}

void MyProtoAdapter::setOnStopStream(OnStopStreamCallback callback) {
    on_stop_stream_ = callback;
}
