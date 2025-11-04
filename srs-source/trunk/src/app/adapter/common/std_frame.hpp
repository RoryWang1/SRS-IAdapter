#pragma once
#include <string>
#include <vector>
#include <stdint.h>

// 标准帧公共字段定义
struct StdFrameCommon {
    std::string codec;      // "H264"/"H265"/"AAC"/"OPUS"/"PCM_ALAW"/"PCM_ULAW"
    int64_t dts_ms = 0;     // 解码时间戳（毫秒）- 单调递增
    int64_t pts_ms = 0;     // 显示时间戳（毫秒）- 对于B帧：dts < pts
    bool keyframe = false;  // 视频关键帧标记（IDR/CRA等）
    std::vector<uint8_t> extradata; // 编解码参数（SPS/PPS/ASC等）
    std::string stream_id;  // 路由键（vhost/app/stream）
    
    // 扩展字段
    uint32_t width = 0;     // 视频宽度
    uint32_t height = 0;     // 视频高度
    uint32_t sample_rate = 0; // 音频采样率
    uint32_t channels = 0;   // 音频声道数
    uint32_t bitrate = 0;    // 码率（bps）
    
    // 时间戳相关
    int64_t duration_ms = 0; // 帧持续时间（毫秒）
    bool has_pts = false;    // 是否有PTS（某些协议可能只有DTS）
};

// 标准帧结构
struct StdFrame {
    StdFrameCommon h;
    std::vector<uint8_t> payload; // Annex-B NALU 或 AAC RAW 等
    
    // 构造函数
    StdFrame() = default;
    
    StdFrame(const std::string& codec, int64_t dts_ms, int64_t pts_ms = 0) {
        h.codec = codec;
        h.dts_ms = dts_ms;
        h.pts_ms = pts_ms;
        h.has_pts = (pts_ms != 0);
    }
    
    // 设置视频参数
    void set_video_params(uint32_t width, uint32_t height, bool keyframe = false) {
        h.width = width;
        h.height = height;
        h.keyframe = keyframe;
    }
    
    // 设置音频参数
    void set_audio_params(uint32_t sample_rate, uint32_t channels) {
        h.sample_rate = sample_rate;
        h.channels = channels;
    }
    
    // 设置编解码参数
    void set_extradata(const std::vector<uint8_t>& data) {
        h.extradata = data;
    }
    
    // 设置负载数据
    void set_payload(const std::vector<uint8_t>& data) {
        payload = data;
    }
    // 移动语义设置负载，尽量避免拷贝
    void set_payload(std::vector<uint8_t>& data) {
        payload.swap(data);
    }
    
    // 设置路由
    void set_stream_id(const std::string& vhost, const std::string& app, const std::string& stream) {
        h.stream_id = vhost + "/" + app + "/" + stream;
    }
    
    // 验证帧的有效性
    bool is_valid() const {
        if (h.codec.empty() || payload.empty()) {
            return false;
        }
        
        // 验证时间戳
        if (h.has_pts && h.pts_ms < h.dts_ms) {
            return false; // B帧的pts应该大于dts
        }
        
        return true;
    }
    
    // 获取帧大小
    size_t size() const {
        return payload.size();
    }
    
    // 是否为关键帧
    bool is_keyframe() const {
        return h.keyframe;
    }
    
    // 是否为视频帧
    bool is_video() const {
        return h.codec == "H264" || h.codec == "H265";
    }
    
    // 是否为音频帧
    bool is_audio() const {
        return h.codec == "AAC" || h.codec == "OPUS" || 
               h.codec == "PCM_ALAW" || h.codec == "PCM_ULAW";
    }
};

// 时间戳转换工具
class TimestampConverter {
public:
    // 90kHz时间戳转换为毫秒
    static int64_t ts90k_to_ms(int64_t ts_90k) {
        return ts_90k / 90;
    }
    
    // 毫秒转换为90kHz时间戳
    static int64_t ms_to_ts90k(int64_t ms) {
        return ms * 90;
    }
    
    // 音频采样时间戳转换为毫秒
    static int64_t samples_to_ms(uint32_t samples, uint32_t sample_rate) {
        return (int64_t)samples * 1000 / sample_rate;
    }
    
    // 毫秒转换为音频采样时间戳
    static uint32_t ms_to_samples(int64_t ms, uint32_t sample_rate) {
        return (uint32_t)(ms * sample_rate / 1000);
    }
    
    // 验证B帧时间戳关系
    static bool validate_b_frame_timing(int64_t dts_ms, int64_t pts_ms) {
        return pts_ms >= dts_ms; // B帧的pts应该大于等于dts
    }
    
    // 计算帧持续时间（用于音频）
    static int64_t calculate_duration_ms(uint32_t samples, uint32_t sample_rate) {
        return samples_to_ms(samples, sample_rate);
    }
};

// 编解码器工具
class CodecUtils {
public:
    // 获取编解码器类型
    static bool is_video_codec(const std::string& codec) {
        return codec == "H264" || codec == "H265";
    }
    
    static bool is_audio_codec(const std::string& codec) {
        return codec == "AAC" || codec == "OPUS" || 
               codec == "PCM_ALAW" || codec == "PCM_ULAW";
    }
    
    // 获取默认采样率
    static uint32_t get_default_sample_rate(const std::string& codec) {
        if (codec == "AAC") return 44100;
        if (codec == "OPUS") return 48000;
        if (codec == "PCM_ALAW" || codec == "PCM_ULAW") return 8000;
        return 0;
    }
    
    // 获取默认声道数
    static uint32_t get_default_channels(const std::string& codec) {
        if (codec == "AAC" || codec == "OPUS") return 2;
        if (codec == "PCM_ALAW" || codec == "PCM_ULAW") return 1;
        return 0;
    }
};
