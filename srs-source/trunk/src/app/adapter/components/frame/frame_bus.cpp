#include "frame_bus.hpp"
#include <srs_kernel_log.hpp>
#include <algorithm>

FrameBus::FrameBus(size_t max_size, const JitterBufferConfig& jitter_config) 
    : max_size_(max_size), use_jitter_buffer_(true),
      total_pushed_(0), total_popped_(0), total_dropped_(0) {
    jitter_buffer_.reset(new JitterBuffer(jitter_config));
}

FrameBus::~FrameBus() {
    clear();
}

srs_error_t FrameBus::push(const StdFrame& frame) {
    total_pushed_++;
    
    if (use_jitter_buffer_) {
        // 使用JitterBuffer处理乱序和抖动
        srs_error_t err = jitter_buffer_->push(frame);
        if (err != srs_success) {
            total_dropped_++;
            return err;
        }
    } else {
        // 使用简单队列
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 检查队列是否已满
        if (queue_.size() >= max_size_) {
            total_dropped_++;
            return srs_error_new(ERROR_SYSTEM_STREAM_BUSY, "Frame bus is full");
        }
        
        queue_.push(frame);
        condition_.notify_one();
    }
    
    return srs_success;
}

srs_error_t FrameBus::pop(StdFrame& frame, int timeout_ms) {
    if (use_jitter_buffer_) {
        // 从JitterBuffer获取帧
        srs_error_t err = jitter_buffer_->pop(frame, timeout_ms);
        if (err == srs_success) {
            total_popped_++;
        }
        return err;
    } else {
        // 从简单队列获取帧
        std::unique_lock<std::mutex> lock(mutex_);
        
        if (condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [this] { return !queue_.empty(); })) {
            frame = queue_.front();
            queue_.pop();
            total_popped_++;
            return srs_success;
        }
        
        return srs_error_new(ERROR_SYSTEM_TIME, "Frame bus pop timeout");
    }
}

size_t FrameBus::size() const {
    if (use_jitter_buffer_) {
        return jitter_buffer_->size();
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
}

void FrameBus::clear() {
    if (use_jitter_buffer_) {
        jitter_buffer_->clear();
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            queue_.pop();
        }
    }
    
    total_pushed_ = 0;
    total_popped_ = 0;
    total_dropped_ = 0;
}

void FrameBus::set_max_size(size_t max_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_size_ = max_size;
}

JitterBufferStats FrameBus::get_jitter_stats() const {
    if (use_jitter_buffer_) {
        return jitter_buffer_->get_stats();
    }
    return JitterBufferStats();
}

void FrameBus::reset_stats() {
    if (use_jitter_buffer_) {
        jitter_buffer_->reset_stats();
    }
    
    total_pushed_ = 0;
    total_popped_ = 0;
    total_dropped_ = 0;
}

void FrameBus::update_jitter_config(const JitterBufferConfig& config) {
    if (use_jitter_buffer_) {
        jitter_buffer_->update_config(config);
    }
}

srs_error_t FrameBus::flush() {
    if (use_jitter_buffer_) {
        return jitter_buffer_->flush();
    }
    return srs_success;
}
