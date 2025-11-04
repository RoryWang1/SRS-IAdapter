#pragma once
#include "../core/iadapter.hpp"
#include "../components/frame/frame_to_source_bridge.hpp"
#include "../core/adapter_stats.hpp"
#include <vector>
#include <memory>
#include <string>
#include <atomic>

// Forward declarations
class ParameterSetManager;
struct StdFrame;

class MyProtoAdapter : public IAdapter {
public:
    MyProtoAdapter();
    virtual ~MyProtoAdapter();

public:
    virtual srs_error_t start(const AdapterInit& init) override;
    virtual srs_error_t feed(const uint8_t* data, size_t nbytes) override;
    virtual srs_error_t parseFrame() override;
    virtual srs_error_t flush() override;
    virtual void close() override;
    
    virtual void setOnStartStream(OnStartStreamCallback callback) override;
    virtual void setOnStopStream(OnStopStreamCallback callback) override;

private:
    enum State {
        STATE_HEADER,
        STATE_PAYLOAD,
        STATE_COMPLETE
    };
    
    struct MyProtoFrame {
        uint32_t magic;
        uint32_t length;
        uint8_t type;
        uint8_t codec;
        uint64_t timestamp;
        uint8_t flags;
        uint32_t width;
        uint32_t height;
        uint32_t sample_rate;
        uint32_t channels;
    };
    
    srs_error_t parseHeader();
    srs_error_t parsePayload();
    srs_error_t createStdFrame();
    srs_error_t processBFrameTiming(StdFrame& frame);
    srs_error_t processParameterSets(const StdFrame& frame);
    srs_error_t resend_parameter_sets(const StdFrame& frame);
    bool validateTimestamp(int64_t dts_ms, int64_t pts_ms) const;

private:
    AdapterInit init_;
    State state_;
    std::vector<uint8_t> input_buffer_;
    class SrsBuffer* buffer_;
    MyProtoFrame frame_header_;
    std::vector<uint8_t> payload_buffer_;
    
    OnStartStreamCallback on_start_stream_;
    OnStopStreamCallback on_stop_stream_;
    
    bool stream_started_;
    
    std::unique_ptr<ParameterSetManager> param_manager_;
    std::unique_ptr<FrameToSourceBridge> source_bridge_;
    
    std::string connection_id_;
    std::string client_ip_;
    int client_port_;
    
    int64_t last_dts_ms_;
    int64_t last_pts_ms_;
    int64_t base_timestamp_ms_;
    bool has_base_timestamp_;

    int64_t last_frame_wallclock_ms_;
    int64_t heartbeat_interval_ms_;

    bool drop_b_in_low_latency_;
    bool hot_start_;
    
    std::vector<StdFrame> b_frame_buffer_;
    int64_t b_frame_delay_ms_;
    
    struct Stats {
        std::atomic<uint64_t> total_frames;
        std::atomic<uint64_t> video_frames;
        std::atomic<uint64_t> audio_frames;
        std::atomic<uint64_t> keyframes;
        std::atomic<uint64_t> b_frames;
        std::atomic<uint64_t> invalid_timestamps;
        std::atomic<uint64_t> parameter_set_updates;
        
        Stats()
            : total_frames(0), video_frames(0), audio_frames(0), keyframes(0),
              b_frames(0), invalid_timestamps(0), parameter_set_updates(0) {
        }
        
        void reset() {
            total_frames = 0;
            video_frames = 0;
            audio_frames = 0;
            keyframes = 0;
            b_frames = 0;
            invalid_timestamps = 0;
            parameter_set_updates = 0;
        }
    } stats_;
};

