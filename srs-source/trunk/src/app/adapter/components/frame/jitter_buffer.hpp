#pragma once
#include "../../common/std_frame.hpp"
#include <srs_kernel_error.hpp>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <atomic>

// JitterBuffer配置
struct JitterBufferConfig {
    int64_t window_ms = 200;        // 乱序窗口大小（毫秒）- 符合文档要求200-500ms范围
    int64_t max_delay_ms = 500;     // 最大延迟（毫秒）- 符合文档要求200-500ms范围
    bool enable_reorder = true;      // 是否启用乱序重排
    bool drop_late_frames = true;   // 是否丢弃迟到帧
    size_t max_frames = 50;         // 最大缓冲帧数
    int64_t flush_interval_ms = 10; // 刷新间隔（毫秒）
};

// JitterBuffer统计信息（使用非atomic类型以便拷贝）
struct JitterBufferStats {
    uint64_t total_frames;
    uint64_t reordered_frames;
    uint64_t dropped_frames;
    uint64_t late_frames;
    uint64_t duplicate_frames;
    int64_t max_jitter_ms;
    int64_t avg_jitter_ms;
    
    JitterBufferStats()
        : total_frames(0), reordered_frames(0), dropped_frames(0),
          late_frames(0), duplicate_frames(0), max_jitter_ms(0), avg_jitter_ms(0) {
    }
};

// JitterBuffer类 - 实现乱序重排和抖动缓冲
class JitterBuffer {
public:
    explicit JitterBuffer(const JitterBufferConfig& config = JitterBufferConfig());
    ~JitterBuffer();
    
    // 推送帧到缓冲区
    srs_error_t push(const StdFrame& frame);
    
    // 从缓冲区弹出帧（按DTS排序）
    srs_error_t pop(StdFrame& frame, int timeout_ms = 1000);
    
    // 刷新缓冲区（处理剩余帧）
    srs_error_t flush();
    
    // 清空缓冲区
    void clear();
    
    // 获取当前缓冲区大小
    size_t size() const;
    
    // 获取统计信息
    JitterBufferStats get_stats() const;
    
    // 重置统计信息
    void reset_stats();
    
    // 更新配置
    void update_config(const JitterBufferConfig& config);
    
    // 检查是否为空
    bool empty() const;
    
    // 获取最早帧的时间戳
    int64_t get_earliest_dts() const;
    
    // 获取最晚帧的时间戳
    int64_t get_latest_dts() const;

private:
    // 帧包装器（包含接收时间）
    struct FrameWrapper {
        StdFrame frame;
        int64_t receive_time_ms;
        bool is_duplicate;
        
        FrameWrapper(const StdFrame& f, int64_t recv_time) 
            : frame(f), receive_time_ms(recv_time), is_duplicate(false) {}
    };
    
    // 比较器（用于优先队列，按DTS排序）
    struct FrameComparator {
        bool operator()(const FrameWrapper& a, const FrameWrapper& b) const {
            return a.frame.h.dts_ms > b.frame.h.dts_ms; // 最小堆
        }
    };
    
    // 内部方法
    srs_error_t process_frame(const StdFrame& frame);
    bool is_frame_late(int64_t dts_ms) const;
    bool is_frame_duplicate(int64_t dts_ms) const;
    void update_jitter_stats(int64_t expected_dts, int64_t actual_dts);
    int64_t get_current_time_ms() const;
    
    // 配置
    JitterBufferConfig config_;
    
    // 缓冲区
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::priority_queue<FrameWrapper, std::vector<FrameWrapper>, FrameComparator> buffer_;
    
    // 状态跟踪
    int64_t last_output_dts_;
    int64_t last_receive_time_;
    std::map<int64_t, int64_t> dts_history_; // 用于重复检测
    
    // 统计信息（内部使用atomic类型以便线程安全）
    struct AtomicStats {
        std::atomic<uint64_t> total_frames;
        std::atomic<uint64_t> reordered_frames;
        std::atomic<uint64_t> dropped_frames;
        std::atomic<uint64_t> late_frames;
        std::atomic<uint64_t> duplicate_frames;
        std::atomic<int64_t> max_jitter_ms;
        std::atomic<int64_t> avg_jitter_ms;
        
        AtomicStats() 
            : total_frames(0), reordered_frames(0), dropped_frames(0),
              late_frames(0), duplicate_frames(0), max_jitter_ms(0), avg_jitter_ms(0) {
        }
        
        void reset() {
            total_frames = 0;
            reordered_frames = 0;
            dropped_frames = 0;
            late_frames = 0;
            duplicate_frames = 0;
            max_jitter_ms = 0;
            avg_jitter_ms = 0;
        }
    };
    AtomicStats stats_;
    
    // 时间基准
    std::chrono::steady_clock::time_point start_time_;
    
    // 内部状态
    bool is_flushing_;
    bool is_closed_;
};

// JitterBuffer实现（inline以支持头文件包含）
inline JitterBuffer::JitterBuffer(const JitterBufferConfig& config) 
    : config_(config), last_output_dts_(0), last_receive_time_(0), 
      is_flushing_(false), is_closed_(false) {
    start_time_ = std::chrono::steady_clock::now();
}

inline JitterBuffer::~JitterBuffer() {
    clear();
}

inline srs_error_t JitterBuffer::push(const StdFrame& frame) {
    if (is_closed_) {
        return srs_error_new(ERROR_SOCKET_CLOSED, "JitterBuffer is closed");
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 更新统计
    stats_.total_frames++;
    
    // 检查重复帧
    if (is_frame_duplicate(frame.h.dts_ms)) {
        stats_.duplicate_frames++;
        return srs_success; // 忽略重复帧
    }
    
    // 检查迟到帧
    if (is_frame_late(frame.h.dts_ms)) {
        stats_.late_frames++;
        if (config_.drop_late_frames) {
            stats_.dropped_frames++;
            return srs_success; // 丢弃迟到帧
        }
    }
    
    // 检查乱序
    if (last_output_dts_ > 0 && frame.h.dts_ms < last_output_dts_) {
        stats_.reordered_frames++;
    }
    
    // 更新抖动统计
    update_jitter_stats(last_output_dts_, frame.h.dts_ms);
    
    // 添加到缓冲区
    int64_t current_time = get_current_time_ms();
    FrameWrapper wrapper(frame, current_time);
    
    buffer_.push(wrapper);
    last_receive_time_ = current_time;
    
    // 记录DTS历史
    dts_history_[frame.h.dts_ms] = current_time;
    
    // 通知等待的线程
    condition_.notify_one();
    
    return srs_success;
}

inline srs_error_t JitterBuffer::pop(StdFrame& frame, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // 等待条件满足
    bool timeout = !condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                      [this] { return !buffer_.empty() || is_closed_; });
    
    if (timeout) {
        return srs_error_new(ERROR_SOCKET_TIMEOUT, "JitterBuffer pop timeout");
    }
    
    if (is_closed_) {
        return srs_error_new(ERROR_SOCKET_CLOSED, "JitterBuffer is closed");
    }
    
    if (buffer_.empty()) {
        return srs_error_new(ERROR_SYSTEM_IO_INVALID, "JitterBuffer is empty");
    }
    
    // 获取最早帧
    FrameWrapper wrapper = buffer_.top();
    buffer_.pop();
    
    frame = wrapper.frame;
    last_output_dts_ = frame.h.dts_ms;
    
    return srs_success;
}

inline srs_error_t JitterBuffer::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    is_flushing_ = true;
    
    // 处理剩余帧
    while (!buffer_.empty()) {
        FrameWrapper wrapper = buffer_.top();
        buffer_.pop();
        
        // 这里可以添加刷新时的特殊处理逻辑
        // 比如发送所有剩余帧，或者丢弃非关键帧等
    }
    
    is_flushing_ = false;
    
    return srs_success;
}

inline void JitterBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    while (!buffer_.empty()) {
        buffer_.pop();
    }
    
    last_output_dts_ = 0;
    last_receive_time_ = 0;
    dts_history_.clear();
    is_flushing_ = false;
}

inline size_t JitterBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.size();
}

inline JitterBufferStats JitterBuffer::get_stats() const {
    JitterBufferStats result;
    result.total_frames = stats_.total_frames.load();
    result.reordered_frames = stats_.reordered_frames.load();
    result.dropped_frames = stats_.dropped_frames.load();
    result.late_frames = stats_.late_frames.load();
    result.duplicate_frames = stats_.duplicate_frames.load();
    result.max_jitter_ms = stats_.max_jitter_ms.load();
    result.avg_jitter_ms = stats_.avg_jitter_ms.load();
    return result;
}

inline void JitterBuffer::reset_stats() {
    stats_.reset();
}

inline void JitterBuffer::update_config(const JitterBufferConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

inline bool JitterBuffer::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.empty();
}

inline int64_t JitterBuffer::get_earliest_dts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_.empty()) {
        return 0;
    }
    return buffer_.top().frame.h.dts_ms;
}

inline int64_t JitterBuffer::get_latest_dts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_.empty()) {
        return 0;
    }
    
    // 由于是优先队列，需要遍历找到最晚的DTS
    int64_t latest = 0;
    auto temp_queue = buffer_;
    while (!temp_queue.empty()) {
        latest = std::max(latest, temp_queue.top().frame.h.dts_ms);
        temp_queue.pop();
    }
    return latest;
}

inline bool JitterBuffer::is_frame_late(int64_t dts_ms) const {
    if (last_output_dts_ == 0) {
        return false; // 第一帧不算迟到
    }
    
    int64_t current_time = get_current_time_ms();
    int64_t expected_time = last_output_dts_ + config_.max_delay_ms;
    
    return dts_ms < expected_time;
}

inline bool JitterBuffer::is_frame_duplicate(int64_t dts_ms) const {
    return dts_history_.find(dts_ms) != dts_history_.end();
}

inline void JitterBuffer::update_jitter_stats(int64_t expected_dts, int64_t actual_dts) {
    if (expected_dts > 0) {
        int64_t jitter = actual_dts - expected_dts;
        
        // 更新最大抖动
        int64_t current_max = stats_.max_jitter_ms.load();
        while (jitter > current_max && 
               !stats_.max_jitter_ms.compare_exchange_weak(current_max, jitter)) {
            // CAS循环
        }
        
        // 更新平均抖动（简化计算）
        int64_t current_avg = stats_.avg_jitter_ms.load();
        int64_t new_avg = (current_avg + jitter) / 2;
        stats_.avg_jitter_ms.store(new_avg);
    }
}

inline int64_t JitterBuffer::get_current_time_ms() const {
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_);
    return duration.count();
}
