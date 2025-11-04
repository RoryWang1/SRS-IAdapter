#pragma once
#include <vector>
#include <stdint.h>
#include <map>
#include <memory>
#include <srs_core.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_log.hpp>

// FEC组块结构
struct FecBlock {
    uint32_t group_id;
    uint32_t block_index;      // 在组内的索引 (0 to k-1)
    uint8_t* data;
    size_t data_size;
    bool is_parity;            // 是否为修复块
    int64_t timestamp_ms;      // 时间戳
    bool received;
    
    // 扩展元数据：用于乱序重排
    uint64_t seq_num;          // 序列号（用于乱序重排）
    bool is_keyframe;          // 是否包含关键帧数据
    
    FecBlock() : group_id(0), block_index(0), data(nullptr), 
                 data_size(0), is_parity(false), timestamp_ms(0), received(false),
                 seq_num(0), is_keyframe(false) {}
    
    ~FecBlock() {
        if (data) {
            delete[] data;
            data = nullptr;
        }
    }
};

// FEC组配置
struct FecGroupConfig {
    uint32_t k;                // 原始块数量
    uint32_t n;                // 总块数量 (包含n-k个修复块)
    int64_t repair_deadline_ms; // 修复截止时间（从组创建开始计算）
    bool enable_keyframe_relax; // 关键帧宽限
    
    FecGroupConfig() : k(8), n(12), repair_deadline_ms(100), enable_keyframe_relax(true) {}
};

// FEC组缓冲区 - 管理一个FEC组的所有块
class FecGroupBuffer {
public:
    FecGroupBuffer(uint32_t group_id, const FecGroupConfig& config);
    ~FecGroupBuffer();
    
    // 添加原始块或修复块
    srs_error_t add_block(uint32_t block_index, const uint8_t* data, size_t size, 
                          bool is_parity, int64_t timestamp_ms,
                          uint64_t seq_num = 0, bool is_keyframe = false);
    
    // 检查是否可以修复
    bool can_repair() const;
    
    // 执行修复，返回修复后的原始块
    srs_error_t repair(std::vector<std::unique_ptr<FecBlock> >& restored_blocks);
    
    // 检查是否超时
    bool is_expired(int64_t current_time_ms) const;
    
    // 检查是否完整（所有k个原始块都已收到）
    bool is_complete() const;
    
    // 获取接收到的块数量
    size_t get_received_count() const;
    
    // 获取缺失的原始块索引
    std::vector<uint32_t> get_missing_blocks() const;
    
    // 获取组ID
    uint32_t get_group_id() const { return group_id_; }
    
    // 获取配置
    const FecGroupConfig& get_config() const { return config_; }

private:
    uint32_t group_id_;
    FecGroupConfig config_;
    std::map<uint32_t, std::unique_ptr<FecBlock> > blocks_;
    int64_t create_time_ms_;
    
    // 简单XOR FEC修复
    srs_error_t repair_xor(std::vector<std::unique_ptr<FecBlock> >& restored_blocks);
    
    // Reed-Solomon FEC修复（支持修复多个缺失块）
    srs_error_t repair_rs(std::vector<std::unique_ptr<FecBlock> >& restored_blocks);
    
    // 检查是否有足够的块进行修复
    bool has_enough_blocks_for_repair() const;
    
    // RS FEC 辅助函数：高斯消元法求解线性方程组
    srs_error_t solve_linear_system(
        const std::vector<std::vector<uint8_t> >& matrix,
        const std::vector<uint8_t*>& rhs,
        std::vector<uint8_t*>& solution,
        size_t block_size,
        size_t num_unknowns);
    
    // RS FEC 辅助函数：Galois Field 乘法（GF(2^8)）
    static uint8_t gf_mul(uint8_t a, uint8_t b);
    
    // RS FEC 辅助函数：构建范德蒙德矩阵（用于RS编码/解码）
    srs_error_t build_vandermonde_matrix(
        std::vector<std::vector<uint8_t> >& matrix,
        const std::vector<uint32_t>& available_indices,
        const std::vector<uint32_t>& missing_indices,
        size_t block_size);
};

// FEC修复管理器 - 管理多个FEC组
class FecRepairManager {
public:
    FecRepairManager();
    ~FecRepairManager();
    
    // 设置配置
    void set_config(const FecGroupConfig& config);
    
    // 添加数据块
    srs_error_t add_block(uint32_t group_id, uint32_t block_index, 
                          const uint8_t* data, size_t size,
                          bool is_parity, int64_t timestamp_ms,
                          uint64_t seq_num = 0, bool is_keyframe = false);
    
    // 检查并修复已就绪的组
    srs_error_t check_and_repair(std::vector<std::vector<uint8_t> >& restored_data);
    
    // 检查并修复已就绪的组（返回带元数据的结果）
    srs_error_t check_and_repair_with_metadata(
        std::vector<std::vector<uint8_t> >& restored_data,
        std::vector<uint64_t>& seq_nums,
        std::vector<bool>& is_keyframes);
    
    // 清理超时的组
    void cleanup_expired(int64_t current_time_ms);
    
    // 获取统计信息
    struct Stats {
        uint64_t total_groups;
        uint64_t repaired_groups;
        uint64_t complete_groups;
        uint64_t expired_groups;
        uint64_t failed_repairs;
        
        Stats() : total_groups(0), repaired_groups(0), complete_groups(0), 
                  expired_groups(0), failed_repairs(0) {}
    };
    
    Stats get_stats() const { return stats_; }
    void reset_stats();
    
    // 设置最大组数量（防止内存溢出）
    void set_max_groups(size_t max_groups) { max_groups_ = max_groups; }
    
private:
    std::map<uint32_t, std::unique_ptr<FecGroupBuffer> > groups_;
    FecGroupConfig config_;
    Stats stats_;
    size_t max_groups_;
    
    srs_error_t create_group_if_needed(uint32_t group_id);
};

