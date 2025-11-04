#include "reorder_buffer.hpp"
#include <srs_kernel_log.hpp>
#include <srs_kernel_ts.hpp>
#include <srs_core_time.hpp>
#include <algorithm>
#include <cstring>

ReorderBuffer::ReorderBuffer(const ReorderBufferConfig& config)
    : config_(config), expected_sequence_(0) {
    stats_ = Stats();
}

ReorderBuffer::~ReorderBuffer() {
    clear();
}

srs_error_t ReorderBuffer::add_packet(uint64_t seq_num, const uint8_t* data, size_t size,
                                       int64_t timestamp_ms, bool is_keyframe) {
    srs_error_t err = srs_success;
    
    stats_.total_packets++;
    
    // 检查是否重复
    if (buffer_.find(seq_num) != buffer_.end()) {
        stats_.duplicate_packets++;
        return srs_success; // 忽略重复包
    }
    
    // 检查缓冲区大小
    size_t current_size = get_buffer_size();
    if (current_size + size > config_.max_buffer_size) {
        stats_.dropped_packets++;
        srs_warn("Reorder buffer full, dropping packet seq=%llu", seq_num);
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "Reorder buffer full");
    }
    
    // 如果是第一个包，初始化期望序列号
    if (buffer_.empty() && expected_sequence_ == 0) {
        expected_sequence_ = seq_num;
        srs_trace("Initialize expected_sequence to %llu", seq_num);
    }
    
    // 检查是否乱序
    if (seq_num < expected_sequence_) {
        stats_.out_of_order_packets++;
        // 旧包，可能已经超时，忽略
        return srs_success;
    }
    
    // 创建片段
    std::unique_ptr<TsPacketFragment> fragment(new TsPacketFragment());
    fragment->data.assign(data, data + size);
    fragment->sequence_number = seq_num;
    fragment->timestamp_ms = timestamp_ms;
    fragment->is_keyframe = is_keyframe;
    
    // 检查是否188字节对齐
    if (size % SRS_TS_PACKET_SIZE == 0) {
        fragment->is_complete = true;
    }
    
    buffer_[seq_num] = std::move(fragment);
    
    return err;
}

srs_error_t ReorderBuffer::get_ordered_packets(std::vector<std::vector<uint8_t> >& packets,
                                                 int64_t current_time_ms) {
    srs_error_t err = srs_success;
    
    packets.clear();
    
    // 从期望的序列号开始，连续输出包
    while (buffer_.find(expected_sequence_) != buffer_.end()) {
        auto it = buffer_.find(expected_sequence_);
        TsPacketFragment* fragment = it->second.get();
        
        // 检查是否过期（关键帧有额外宽限）
        if (is_packet_expired(*fragment, current_time_ms)) {
            buffer_.erase(it);
            stats_.dropped_packets++;
            expected_sequence_++;
            continue;
        }
        
        // 对齐TS包
        std::vector<std::vector<uint8_t> > aligned;
        if ((err = align_ts_packets(fragment->data, aligned)) != srs_success) {
            srs_warn("Failed to align TS packets: seq=%llu, size=%zu, %s", 
                    expected_sequence_, fragment->data.size(), srs_error_desc(err).c_str());
            srs_freep(err);
            buffer_.erase(it);
            expected_sequence_++;
            continue;
        }
        
        static uint64_t aligned_count = 0;
        aligned_count++;
        if (aligned_count <= 10 || aligned_count % 100 == 0) {
            srs_trace("TS packets aligned: seq=%llu, input_size=%zu, output_packets=%zu", 
                     expected_sequence_, fragment->data.size(), aligned.size());
        }
        
        // 添加到输出
        for (auto& pkt : aligned) {
            packets.push_back(std::move(pkt));
        }
        
        buffer_.erase(it);
        expected_sequence_++;
        
        if (expected_sequence_ < it->first) {
            stats_.reordered_packets++;
        }
    }
    
    return err;
}

bool ReorderBuffer::has_ready_packets() const {
    return buffer_.find(expected_sequence_) != buffer_.end();
}

size_t ReorderBuffer::get_buffer_size() const {
    size_t total = 0;
    for (const auto& pair : buffer_) {
        total += pair.second->data.size();
    }
    return total;
}

void ReorderBuffer::clear() {
    buffer_.clear();
    expected_sequence_ = 0;
}

bool ReorderBuffer::is_packet_expired(const TsPacketFragment& fragment, 
                                       int64_t current_time_ms) const {
    int64_t age_ms = current_time_ms - fragment.timestamp_ms;
    int64_t deadline_ms = config_.reorder_window_ms;
    
    // 关键帧宽限
    if (config_.enable_keyframe_relax && fragment.is_keyframe) {
        deadline_ms += config_.keyframe_relax_ms;
    }
    
    return age_ms > deadline_ms;
}

srs_error_t ReorderBuffer::align_ts_packets(const std::vector<uint8_t>& data,
                                             std::vector<std::vector<uint8_t> >& aligned_packets) {
    srs_error_t err = srs_success;
    
    aligned_packets.clear();
    
    if (data.empty()) {
        return srs_success;
    }
    
    // 查找第一个同步字节 0x47
    size_t start_pos = 0;
    for (size_t i = 0; i < data.size() && i < 188; i++) {
        if (data[i] == 0x47) {
            start_pos = i;
            break;
        }
    }
    
    // 如果没找到同步字节，尝试使用整个数据作为单个包
    if (start_pos == 0 && data[0] != 0x47) {
        // 可能数据不完整，返回空
        if (data.size() < SRS_TS_PACKET_SIZE) {
            return srs_success;
        }
        // 假设从0开始
        start_pos = 0;
    }
    
    // 按188字节对齐切分
    size_t pos = start_pos;
    while (pos + SRS_TS_PACKET_SIZE <= data.size()) {
        std::vector<uint8_t> packet(data.begin() + pos, 
                                    data.begin() + pos + SRS_TS_PACKET_SIZE);
        aligned_packets.push_back(std::move(packet));
        pos += SRS_TS_PACKET_SIZE;
    }
    
    return err;
}

void ReorderBuffer::reset_stats() {
    stats_ = Stats();
}

