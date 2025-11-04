#pragma once
#include "../../common/std_frame.hpp"
#include "jitter_buffer.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>

// 帧总线类 - 用于缓冲和派发标准帧（集成JitterBuffer）
class FrameBus {
public:
    FrameBus(size_t max_size = 100, const JitterBufferConfig& jitter_config = JitterBufferConfig());
    ~FrameBus();
    
    // 推送帧到总线
    srs_error_t push(const StdFrame& frame);
    
    // 从总线弹出帧
    srs_error_t pop(StdFrame& frame, int timeout_ms = 1000);
    
    // 获取当前队列大小
    size_t size() const;
    
    // 清空队列
    void clear();
    
    // 设置最大队列大小
    void set_max_size(size_t max_size);
    
    // 获取JitterBuffer统计信息
    JitterBufferStats get_jitter_stats() const;
    
    // 重置统计信息
    void reset_stats();
    
    // 更新JitterBuffer配置
    void update_jitter_config(const JitterBufferConfig& config);
    
    // 刷新缓冲区
    srs_error_t flush();

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<StdFrame> queue_;
    size_t max_size_;
    
    // JitterBuffer集成
    std::unique_ptr<JitterBuffer> jitter_buffer_;
    bool use_jitter_buffer_;
    
    // 统计信息
    std::atomic<uint64_t> total_pushed_;
    std::atomic<uint64_t> total_popped_;
    std::atomic<uint64_t> total_dropped_;
};
