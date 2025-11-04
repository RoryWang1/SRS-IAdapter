#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include "std_frame.hpp"
#include "test_recorder.hpp"
#include "test_player.hpp"

// 端到端测试结果
struct E2ETestResult {
    std::string test_name;
    bool success;
    std::string error_message;
    double duration_seconds;
    size_t frames_recorded;
    size_t frames_played;
    size_t bytes_recorded;
    size_t bytes_played;
    
    // 性能指标
    double fps_recorded;
    double fps_played;
    double bitrate_mbps;
    double latency_ms;
    
    // 质量指标
    double frame_drop_rate;
    double timestamp_accuracy;
    bool keyframe_preserved;
    bool codec_preserved;
};

// 测试配置
struct E2ETestConfig {
    std::string test_name;
    std::string input_file;
    std::string output_dir;
    RecorderFactory::RecorderType recorder_type;
    PlayerFactory::PlayerType player_type;
    
    // 测试参数
    int64_t test_duration_ms;
    bool enable_recording;
    bool enable_playback;
    bool enable_latency_test;
    bool enable_quality_test;
    
    // 性能参数
    int max_fps;
    int64_t max_latency_ms;
    double max_frame_drop_rate;
    
    // 质量参数
    bool verify_keyframes;
    bool verify_codecs;
    bool verify_timestamps;
};

// 端到端测试器
class E2ETester {
private:
    std::string test_output_dir_;
    std::atomic<bool> test_running_;
    std::mutex test_mutex_;
    
    // 测试统计
    std::atomic<size_t> total_tests_;
    std::atomic<size_t> passed_tests_;
    std::atomic<size_t> failed_tests_;
    
public:
    E2ETester();
    ~E2ETester();
    
    // 初始化测试环境
    bool initialize(const std::string& output_dir);
    void cleanup();
    
    // 运行单个测试
    E2ETestResult run_test(const E2ETestConfig& config);
    
    // 运行测试套件
    std::vector<E2ETestResult> run_test_suite(const std::vector<E2ETestConfig>& configs);
    
    // 生成测试报告
    std::string generate_report(const std::vector<E2ETestResult>& results);
    
    // 测试统计
    size_t get_total_tests() const { return total_tests_.load(); }
    size_t get_passed_tests() const { return passed_tests_.load(); }
    size_t get_failed_tests() const { return failed_tests_.load(); }
    double get_success_rate() const;
    
private:
    // 内部测试方法
    E2ETestResult run_recording_test(const E2ETestConfig& config);
    E2ETestResult run_playback_test(const E2ETestConfig& config);
    E2ETestResult run_latency_test(const E2ETestConfig& config);
    E2ETestResult run_quality_test(const E2ETestConfig& config);
    
    // 辅助方法
    bool create_test_output_dir();
    std::string generate_test_filename(const std::string& test_name, 
                                     RecorderFactory::RecorderType type);
    bool verify_file_exists(const std::string& file_path);
    bool verify_file_size(const std::string& file_path, size_t min_size);
    
    // 性能测试
    double calculate_fps(size_t frame_count, double duration_seconds);
    double calculate_bitrate(size_t bytes, double duration_seconds);
    double calculate_latency(const std::vector<StdFrame>& frames);
    
    // 质量测试
    bool verify_keyframes(const std::vector<StdFrame>& frames);
    bool verify_codecs(const std::vector<StdFrame>& frames);
    bool verify_timestamps(const std::vector<StdFrame>& frames);
    double calculate_frame_drop_rate(size_t expected_frames, size_t actual_frames);
};

// 测试数据生成器
class TestDataGenerator {
public:
    // 生成测试视频帧
    static std::vector<StdFrame> generate_test_video_frames(
        const std::string& codec, 
        int width, int height, 
        int frame_count, 
        int64_t duration_ms);
    
    // 生成测试音频帧
    static std::vector<StdFrame> generate_test_audio_frames(
        const std::string& codec, 
        int sample_rate, int channels, 
        int frame_count, 
        int64_t duration_ms);
    
    // 生成混合流（视频+音频）
    static std::vector<StdFrame> generate_test_mixed_frames(
        const std::string& video_codec, const std::string& audio_codec,
        int video_width, int video_height,
        int audio_sample_rate, int audio_channels,
        int frame_count, int64_t duration_ms);
    
    // 保存测试数据到文件
    static bool save_test_frames(const std::vector<StdFrame>& frames, 
                                const std::string& output_path);
    
    // 从文件加载测试数据
    static std::vector<StdFrame> load_test_frames(const std::string& input_path);
    
private:
    static StdFrame create_video_frame(const std::string& codec, 
                                      int width, int height, 
                                      int64_t dts_ms, int64_t pts_ms, 
                                      bool keyframe);
    static StdFrame create_audio_frame(const std::string& codec, 
                                      int sample_rate, int channels, 
                                      int64_t dts_ms, int64_t pts_ms);
    static std::vector<uint8_t> generate_dummy_payload(size_t size);
};

// 测试验证器
class TestValidator {
public:
    // 验证录制结果
    static bool validate_recording(const E2ETestResult& result, 
                                  const E2ETestConfig& config);
    
    // 验证回放结果
    static bool validate_playback(const E2ETestResult& result, 
                                 const E2ETestConfig& config);
    
    // 验证性能指标
    static bool validate_performance(const E2ETestResult& result, 
                                    const E2ETestConfig& config);
    
    // 验证质量指标
    static bool validate_quality(const E2ETestResult& result, 
                                const E2ETestConfig& config);
    
    // 综合验证
    static bool validate_test_result(const E2ETestResult& result, 
                                   const E2ETestConfig& config);
};

// 测试报告生成器
class TestReportGenerator {
public:
    // 生成HTML报告
    static std::string generate_html_report(const std::vector<E2ETestResult>& results);
    
    // 生成JSON报告
    static std::string generate_json_report(const std::vector<E2ETestResult>& results);
    
    // 生成CSV报告
    static std::string generate_csv_report(const std::vector<E2ETestResult>& results);
    
    // 保存报告到文件
    static bool save_report(const std::string& report, 
                           const std::string& file_path);
    
private:
    static std::string format_duration(double seconds);
    static std::string format_bytes(size_t bytes);
    static std::string format_fps(double fps);
    static std::string format_bitrate(double bitrate_mbps);
    static std::string get_status_color(bool success);
};








