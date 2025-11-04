#pragma once
#include <srs_core.hpp>
#include <srs_app_http_api.hpp>
#include <srs_kernel_log.hpp>
#include <map>
#include <string>
#include <atomic>
#include <chrono>

// Adapter统计信息（使用非atomic类型以便拷贝返回）
struct AdapterStats {
    // 连接统计
    int64_t total_connections;
    int64_t active_connections;
    int64_t failed_connections;
    
    // 帧统计
    int64_t total_frames;
    int64_t video_frames;
    int64_t audio_frames;
    int64_t keyframes;
    int64_t dropped_frames;
    
    // 时间统计
    int64_t first_frame_time_ms;
    int64_t avg_frame_interval_ms;
    int64_t max_frame_interval_ms;
    
    // 质量统计
    int64_t jitter_buffer_hits;
    int64_t jitter_buffer_misses;
    int64_t out_of_order_frames;
    int64_t zero_copy_hits;
    int64_t zero_copy_misses;
    
    // 错误统计
    int64_t parse_errors;
    int64_t timestamp_errors;
    int64_t codec_errors;
    
    // 性能统计
    double cpu_usage_percent;
    int64_t memory_usage_bytes;
    int64_t peak_memory_usage_bytes;
    
    // 时间戳
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_update_time;
    
    AdapterStats() 
        : total_connections(0), active_connections(0), failed_connections(0),
          total_frames(0), video_frames(0), audio_frames(0), keyframes(0), dropped_frames(0),
          first_frame_time_ms(0), avg_frame_interval_ms(0), max_frame_interval_ms(0),
          jitter_buffer_hits(0), jitter_buffer_misses(0), out_of_order_frames(0),
          zero_copy_hits(0), zero_copy_misses(0),
          parse_errors(0), timestamp_errors(0), codec_errors(0),
          cpu_usage_percent(0.0), memory_usage_bytes(0), peak_memory_usage_bytes(0) {
        start_time = std::chrono::steady_clock::now();
        last_update_time = start_time;
    }
    
    // 计算运行时间
    int64_t get_uptime_ms() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
    }
    
    // 计算丢帧率
    double get_drop_rate() const {
        if (total_frames == 0) return 0.0;
        return (double)dropped_frames / total_frames * 100.0;
    }
    
    // 计算JitterBuffer命中率
    double get_jitter_hit_rate() const {
        int64_t total = jitter_buffer_hits + jitter_buffer_misses;
        if (total == 0) return 0.0;
        return (double)jitter_buffer_hits / total * 100.0;
    }
    
    // 计算近零拷贝命中率
    double get_zero_copy_hit_rate() const {
        int64_t total = zero_copy_hits + zero_copy_misses;
        if (total == 0) return 0.0;
        return (double)zero_copy_hits / total * 100.0;
    }
};

// Adapter连接信息
struct AdapterConnection {
    std::string connection_id;
    std::string protocol;
    std::string vhost;
    std::string app;
    std::string stream;
    std::string client_ip;
    int client_port;
    std::chrono::steady_clock::time_point connect_time;
    std::chrono::steady_clock::time_point first_frame_time;
    std::atomic<bool> is_active;
    AdapterStats stats;
    
    AdapterConnection(const std::string& id, const std::string& proto, 
                     const std::string& v, const std::string& a, const std::string& s,
                     const std::string& ip, int port)
        : connection_id(id), protocol(proto), vhost(v), app(a), stream(s),
          client_ip(ip), client_port(port), is_active(true) {
        connect_time = std::chrono::steady_clock::now();
        first_frame_time = connect_time;
    }
    
    int64_t get_connection_duration_ms() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - connect_time).count();
    }
    
    int64_t get_first_frame_latency_ms() const {
        if (first_frame_time == connect_time) return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(first_frame_time - connect_time).count();
    }
};

// Adapter统计管理器
class AdapterStatsManager {
public:
    static AdapterStatsManager& instance();
    
    // 连接管理
    void add_connection(const std::string& id, const std::string& protocol,
                       const std::string& vhost, const std::string& app, const std::string& stream,
                       const std::string& client_ip, int client_port);
    void remove_connection(const std::string& id);
    AdapterConnection* get_connection(const std::string& id);
    
    // 统计更新
    void update_frame_stats(const std::string& id, bool is_video, bool is_keyframe, bool is_dropped);
    void update_jitter_stats(const std::string& id, bool hit);
    void update_zero_copy_stats(const std::string& id, bool hit);
    void update_error_stats(const std::string& id, const std::string& error_type);
    void update_first_frame_time(const std::string& id);
    
    // 获取统计信息
    std::map<std::string, AdapterConnection*> get_all_connections();
    AdapterStats get_global_stats();
    
    // HTTP API支持
    std::string to_json();
    std::string get_connection_json(const std::string& id);
    
private:
    std::map<std::string, std::unique_ptr<AdapterConnection> > connections_;
    AdapterStats global_stats_;
    mutable std::mutex mutex_;
    
    AdapterStatsManager() = default;
};

// HTTP API处理器
class SrsAdapterHttpApiHandler : public ISrsHttpHandler {
public:
    SrsAdapterHttpApiHandler();
    virtual ~SrsAdapterHttpApiHandler();
    
public:
    virtual srs_error_t serve_http(ISrsHttpResponseWriter* w, ISrsHttpMessage* r);
    
private:
    srs_error_t handle_adapters_api(ISrsHttpResponseWriter* w, ISrsHttpMessage* r);
    srs_error_t handle_connection_api(ISrsHttpResponseWriter* w, ISrsHttpMessage* r, const std::string& id);
    srs_error_t handle_stats_api(ISrsHttpResponseWriter* w, ISrsHttpMessage* r);
};

