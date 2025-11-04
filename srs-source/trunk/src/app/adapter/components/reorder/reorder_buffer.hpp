#pragma once
#include <vector>
#include <stdint.h>
#include <map>
#include <deque>
#include <srs_core.hpp>
#include <srs_kernel_error.hpp>

// 乱序缓冲配置
struct ReorderBufferConfig {
    int64_t reorder_window_ms;      // 重排窗口时间（毫秒）
    bool enable_keyframe_relax;     // 关键帧宽限（关键帧可以等待更久）
    int64_t keyframe_relax_ms;      // 关键帧额外等待时间
    size_t max_buffer_size;         // 最大缓冲大小（字节）
    
    ReorderBufferConfig() 
        : reorder_window_ms(200), enable_keyframe_relax(true), 
          keyframe_relax_ms(100), max_buffer_size(10 * 1024 * 1024) {} // 10MB
};

// TS包片段（可能包含多个188字节的TS包）
struct TsPacketFragment {
    std::vector<uint8_t> data;
    uint64_t sequence_number;       // 序列号
    int64_t timestamp_ms;           // 时间戳
    bool is_keyframe;               // 是否包含关键帧
    bool is_complete;               // 是否完整（188字节对齐）
    
    TsPacketFragment() 
        : sequence_number(0), timestamp_ms(0), 
          is_keyframe(false), is_complete(false) {}
};

// 乱序缓冲区 - 用于TS包的乱序重排
class ReorderBuffer {
public:
    ReorderBuffer(const ReorderBufferConfig& config);
    ~ReorderBuffer();
    
    // 添加TS包片段
    srs_error_t add_packet(uint64_t seq_num, const uint8_t* data, size_t size,
                           int64_t timestamp_ms, bool is_keyframe);
    
    // 获取有序的TS包（按序列号排序）
    srs_error_t get_ordered_packets(std::vector<std::vector<uint8_t> >& packets,
                                     int64_t current_time_ms);
    
    // 检查是否有可输出的包
    bool has_ready_packets() const;
    
    // 获取缓冲区大小
    size_t get_buffer_size() const;
    
    // 清空缓冲区
    void clear();
    
    // 获取统计信息
    struct Stats {
        uint64_t total_packets;
        uint64_t out_of_order_packets;
        uint64_t dropped_packets;
        uint64_t duplicate_packets;
        uint64_t reordered_packets;
        
        Stats() : total_packets(0), out_of_order_packets(0), 
                  dropped_packets(0), duplicate_packets(0), reordered_packets(0) {}
    };
    
    Stats get_stats() const { return stats_; }
    void reset_stats();
    
    // 设置下一个期望的序列号
    void set_expected_sequence(uint64_t seq) { expected_sequence_ = seq; }

private:
    ReorderBufferConfig config_;
    std::map<uint64_t, std::unique_ptr<TsPacketFragment> > buffer_;
    uint64_t expected_sequence_;    // 下一个期望的序列号
    Stats stats_;
    
    // 检查包是否过期
    bool is_packet_expired(const TsPacketFragment& fragment, int64_t current_time_ms) const;
    
    // 对齐TS包（确保188字节对齐）
    srs_error_t align_ts_packets(const std::vector<uint8_t>& data,
                                  std::vector<std::vector<uint8_t> >& aligned_packets);
};

