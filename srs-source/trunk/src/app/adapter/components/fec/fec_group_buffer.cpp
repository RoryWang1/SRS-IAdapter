#include "fec_group_buffer.hpp"
#include <srs_core_time.hpp>
#include <algorithm>
#include <cstring>

FecGroupBuffer::FecGroupBuffer(uint32_t group_id, const FecGroupConfig& config)
    : group_id_(group_id), config_(config) {
    srs_utime_t now_us = srs_time_now_cached();
    create_time_ms_ = now_us / 1000;
}

FecGroupBuffer::~FecGroupBuffer() {
    blocks_.clear();
}

srs_error_t FecGroupBuffer::add_block(uint32_t block_index, const uint8_t* data, size_t size,
                                       bool is_parity, int64_t timestamp_ms,
                                       uint64_t seq_num, bool is_keyframe) {
    srs_error_t err = srs_success;
    
    // 检查索引范围
    if (is_parity) {
        if (block_index >= config_.n - config_.k) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                               "Parity block index out of range: %d >= %d", 
                               block_index, config_.n - config_.k);
        }
        // 修复块的索引需要映射到 n-k 范围内
        block_index = config_.k + block_index;
    } else {
        if (block_index >= config_.k) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                               "Data block index out of range: %d >= %d", 
                               block_index, config_.k);
        }
    }
    
    // 检查是否已存在
    if (blocks_.find(block_index) != blocks_.end()) {
        // 已存在，跳过（或更新？）
        return srs_success;
    }
    
    // 创建新块
    std::unique_ptr<FecBlock> block(new FecBlock());
    block->group_id = group_id_;
    block->block_index = block_index;
    block->data = new uint8_t[size];
    memcpy(block->data, data, size);
    block->data_size = size;
    block->is_parity = is_parity;
    block->timestamp_ms = timestamp_ms;
    block->received = true;
    block->seq_num = seq_num;
    block->is_keyframe = is_keyframe;
    
    blocks_[block_index] = std::move(block);
    
    return err;
}

bool FecGroupBuffer::can_repair() const {
    if (is_complete()) {
        return true;
    }
    return has_enough_blocks_for_repair();
}

bool FecGroupBuffer::has_enough_blocks_for_repair() const {
    // 需要至少k个块（原始块+修复块）才能修复
    size_t received_count = blocks_.size();
    return received_count >= config_.k;
}

bool FecGroupBuffer::is_complete() const {
    // 检查是否所有k个原始块都已收到
    size_t data_blocks = 0;
    for (const auto& pair : blocks_) {
        if (!pair.second->is_parity) {
            data_blocks++;
        }
    }
    return data_blocks >= config_.k;
}

srs_error_t FecGroupBuffer::repair(std::vector<std::unique_ptr<FecBlock> >& restored_blocks) {
    srs_error_t err = srs_success;
    
    if (!can_repair()) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, 
                           "Cannot repair: not enough blocks");
    }
    
    // 如果已经完整，直接返回所有原始块（包含元数据）
    if (is_complete()) {
        for (std::map<uint32_t, std::unique_ptr<FecBlock> >::const_iterator it = blocks_.begin();
             it != blocks_.end(); ++it) {
            if (!it->second->is_parity) {
                std::unique_ptr<FecBlock> copy(new FecBlock());
                copy->group_id = it->second->group_id;
                copy->block_index = it->second->block_index;
                copy->data = new uint8_t[it->second->data_size];
                memcpy(copy->data, it->second->data, it->second->data_size);
                copy->data_size = it->second->data_size;
                copy->is_parity = false;
                copy->timestamp_ms = it->second->timestamp_ms;
                copy->received = true;
                copy->seq_num = it->second->seq_num;  // 保留序列号
                copy->is_keyframe = it->second->is_keyframe;  // 保留关键帧标志
                restored_blocks.push_back(std::move(copy));
            }
        }
        return err;
    }
    
    // 尝试使用 RS FEC 修复（如果支持且块数足够）
    // RS FEC 可以修复多个缺失块，而 XOR 只能修复1个
    std::vector<uint32_t> missing = get_missing_blocks();
    size_t total_received = blocks_.size();
    size_t num_parity = total_received - (config_.k - missing.size());
    
    // 如果缺失块数 <= 校验块数，可以使用 RS FEC
    if (missing.size() > 0 && missing.size() <= num_parity && total_received >= config_.k) {
        srs_error_t rs_err = repair_rs(restored_blocks);
        if (rs_err == srs_success) {
            return rs_err;
        }
        // 如果 RS 修复失败，fallback 到 XOR（如果只缺失1个块）
        srs_freep(rs_err);
    }
    
    // Fallback 到 XOR FEC（仅支持修复1个缺失块）
    if (missing.size() == 1) {
        return repair_xor(restored_blocks);
    }
    
    // 无法修复
    return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                        "Cannot repair: Missing %zu blocks, but only %zu parity blocks available",
                        missing.size(), num_parity);
}

srs_error_t FecGroupBuffer::repair_xor(std::vector<std::unique_ptr<FecBlock> >& restored_blocks) {
    srs_error_t err = srs_success;
    
    // 简单的XOR FEC实现
    // 假设所有块大小相同（实际中可能需要对齐处理）
    size_t block_size = 0;
    for (const auto& pair : blocks_) {
        if (block_size == 0) {
            block_size = pair.second->data_size;
        } else if (pair.second->data_size != block_size) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                               "Block size mismatch: %zu vs %zu",
                               block_size, pair.second->data_size);
        }
    }
    
    if (block_size == 0) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "No blocks to repair");
    }
    
    // 找出缺失的原始块索引
    std::vector<uint32_t> missing = get_missing_blocks();
    
    // 对于每个缺失的块，尝试用修复块恢复
    // 这里简化实现：假设只有一个修复块，直接用XOR恢复
    // 实际RS FEC需要更复杂的矩阵运算
    
    // 找到修复块
    std::vector<FecBlock*> parity_blocks;
    std::vector<FecBlock*> data_blocks;
    
    for (const auto& pair : blocks_) {
        if (pair.second->is_parity) {
            parity_blocks.push_back(pair.second.get());
        } else {
            data_blocks.push_back(pair.second.get());
        }
    }
    
    // 简单XOR修复：如果缺失1个块，且有1个修复块和k-1个原始块，可以恢复
    if (missing.size() == 1 && parity_blocks.size() >= 1 && 
        data_blocks.size() == config_.k - 1) {
        
        uint32_t missing_idx = missing[0];
        FecBlock* parity = parity_blocks[0];
        
        // 创建恢复的块
        std::unique_ptr<FecBlock> restored(new FecBlock());
        restored->group_id = group_id_;
        restored->block_index = missing_idx;
        restored->data = new uint8_t[block_size];
        restored->data_size = block_size;
        restored->is_parity = false;
        restored->received = true;
        // 从数据块中继承元数据（使用第一个数据块的元数据作为参考）
        if (!data_blocks.empty()) {
            restored->seq_num = data_blocks[0]->seq_num + (missing_idx > data_blocks[0]->block_index ? missing_idx - data_blocks[0]->block_index : 0);
            // 如果任何数据块是关键帧，则修复的块也可能是关键帧
            for (FecBlock* block : data_blocks) {
                if (block->is_keyframe) {
                    restored->is_keyframe = true;
                    break;
                }
            }
        }
        restored->timestamp_ms = parity->timestamp_ms;
        
        // XOR操作：restored = parity XOR (所有已知原始块)
        memcpy(restored->data, parity->data, block_size);
        for (FecBlock* block : data_blocks) {
            for (size_t i = 0; i < block_size; i++) {
                restored->data[i] ^= block->data[i];
            }
        }
        
        restored_blocks.push_back(std::move(restored));
        
        // 同时添加所有已收到的原始块
        for (FecBlock* block : data_blocks) {
            std::unique_ptr<FecBlock> copy(new FecBlock());
            copy->group_id = block->group_id;
            copy->block_index = block->block_index;
            copy->data = new uint8_t[block->data_size];
            memcpy(copy->data, block->data, block->data_size);
            copy->data_size = block->data_size;
            copy->is_parity = false;
            copy->timestamp_ms = block->timestamp_ms;
            copy->received = true;
            copy->seq_num = block->seq_num;
            copy->is_keyframe = block->is_keyframe;
            restored_blocks.push_back(std::move(copy));
        }
        
        return err;
    }
    
    // 如果无法用简单XOR修复，返回错误
    // 实际中应该集成RS FEC库
    return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                        "XOR FEC cannot repair this group. Missing: %zu, Parity: %zu, Data: %zu",
                        missing.size(), parity_blocks.size(), data_blocks.size());
}

// Reed-Solomon FEC 修复实现
srs_error_t FecGroupBuffer::repair_rs(std::vector<std::unique_ptr<FecBlock> >& restored_blocks) {
    srs_error_t err = srs_success;
    
    // 检查所有块大小是否相同
    size_t block_size = 0;
    for (const auto& pair : blocks_) {
        if (block_size == 0) {
            block_size = pair.second->data_size;
        } else if (pair.second->data_size != block_size) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                               "Block size mismatch: %zu vs %zu",
                               block_size, pair.second->data_size);
        }
    }
    
    if (block_size == 0) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "No blocks to repair");
    }
    
    // 分离数据块和校验块
    std::vector<FecBlock*> parity_blocks;
    std::vector<FecBlock*> data_blocks;
    std::vector<uint32_t> available_indices;  // 已收到的数据块索引
    std::vector<uint32_t> missing_indices = get_missing_blocks();  // 缺失的数据块索引
    
    for (const auto& pair : blocks_) {
        if (pair.second->is_parity) {
            parity_blocks.push_back(pair.second.get());
        } else {
            data_blocks.push_back(pair.second.get());
            available_indices.push_back(pair.second->block_index);
        }
    }
    
    if (missing_indices.empty()) {
        // 没有缺失块，直接返回已收到的数据块
        for (FecBlock* block : data_blocks) {
            std::unique_ptr<FecBlock> copy(new FecBlock());
            copy->group_id = block->group_id;
            copy->block_index = block->block_index;
            copy->data = new uint8_t[block->data_size];
            memcpy(copy->data, block->data, block->data_size);
            copy->data_size = block->data_size;
            copy->is_parity = false;
            copy->timestamp_ms = block->timestamp_ms;
            copy->received = true;
            copy->seq_num = block->seq_num;
            copy->is_keyframe = block->is_keyframe;
            restored_blocks.push_back(std::move(copy));
        }
        return err;
    }
    
    // 检查是否有足够的校验块
    if (parity_blocks.size() < missing_indices.size()) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                           "Not enough parity blocks: need %zu, have %zu",
                           missing_indices.size(), parity_blocks.size());
    }
    
    // 构建范德蒙德矩阵（用于 RS 解码）
    std::vector<std::vector<uint8_t> > matrix;
    if ((err = build_vandermonde_matrix(matrix, available_indices, missing_indices, block_size)) != srs_success) {
        return srs_error_wrap(err, "build vandermonde matrix");
    }
    
    // 构建右侧向量（使用校验块）
    std::vector<uint8_t*> rhs;
    for (size_t i = 0; i < missing_indices.size() && i < parity_blocks.size(); i++) {
        rhs.push_back(parity_blocks[i]->data);
    }
    
    // 求解线性方程组
    std::vector<uint8_t*> solution;
    solution.resize(missing_indices.size());
    for (size_t i = 0; i < missing_indices.size(); i++) {
        solution[i] = new uint8_t[block_size];
        memset(solution[i], 0, block_size);
    }
    
    if ((err = solve_linear_system(matrix, rhs, solution, block_size, missing_indices.size())) != srs_success) {
        for (size_t i = 0; i < solution.size(); i++) {
            delete[] solution[i];
        }
        return srs_error_wrap(err, "solve linear system");
    }
    
    // 创建恢复的数据块
    for (size_t i = 0; i < missing_indices.size(); i++) {
        std::unique_ptr<FecBlock> restored(new FecBlock());
        restored->group_id = group_id_;
        restored->block_index = missing_indices[i];
        restored->data = solution[i];  // 所有权转移
        restored->data_size = block_size;
        restored->is_parity = false;
        restored->received = true;
        
        // 从可用数据块推断元数据
        if (!data_blocks.empty()) {
            restored->seq_num = data_blocks[0]->seq_num + 
                (missing_indices[i] > data_blocks[0]->block_index ? 
                 missing_indices[i] - data_blocks[0]->block_index : 0);
            for (FecBlock* block : data_blocks) {
                if (block->is_keyframe) {
                    restored->is_keyframe = true;
                    break;
                }
            }
        }
        restored->timestamp_ms = parity_blocks[0]->timestamp_ms;
        
        restored_blocks.push_back(std::move(restored));
    }
    
    // 添加所有已收到的原始块（保留元数据）
    for (size_t i = 0; i < data_blocks.size(); ++i) {
        FecBlock* block = data_blocks[i];
        std::unique_ptr<FecBlock> copy(new FecBlock());
        copy->group_id = block->group_id;
        copy->block_index = block->block_index;
        copy->data = new uint8_t[block->data_size];
        memcpy(copy->data, block->data, block->data_size);
        copy->data_size = block->data_size;
        copy->is_parity = false;
        copy->timestamp_ms = block->timestamp_ms;
        copy->received = true;
        copy->seq_num = block->seq_num;  // 保留序列号
        copy->is_keyframe = block->is_keyframe;  // 保留关键帧标志
        restored_blocks.push_back(std::move(copy));
    }
    
    return err;
}

// Galois Field 查找表（GF(2^8)，使用原始多项式 x^8 + x^4 + x^3 + x^2 + 1）
// 生成对数表和反对数表以优化乘法运算
namespace {
    // 生成 alpha 的对数表（alpha = 2）
    uint8_t gf_log_table[256];
    uint8_t gf_exp_table[512];  // 512 = 256*2，处理溢出
    
    bool gf_tables_initialized = false;
    
    void init_gf_tables() {
        if (gf_tables_initialized) return;
        
        // 初始化 exp 表：exp[i] = alpha^i mod 255
        uint8_t val = 1;
        gf_exp_table[0] = 1;
        for (int i = 1; i < 255; i++) {
            val = (val << 1) ^ ((val & 0x80) ? 0x1D : 0);  // 原始多项式 0x1D
            gf_exp_table[i] = val;
            gf_log_table[val] = i;
        }
        gf_exp_table[255] = gf_exp_table[0];  // alpha^255 = alpha^0 = 1
        
        // 复制前255个值到256-510以处理溢出
        for (int i = 255; i < 512; i++) {
            gf_exp_table[i] = gf_exp_table[i - 255];
        }
        
        // log(0) 未定义，设为特殊值
        gf_log_table[0] = 255;  // 表示错误
        
        gf_tables_initialized = true;
    }
    
    // 使用查找表快速计算 GF 乘法
    inline uint8_t gf_mul_fast(uint8_t a, uint8_t b) {
        if (a == 0 || b == 0) return 0;
        if (a == 1) return b;
        if (b == 1) return a;
        
        int sum = gf_log_table[a] + gf_log_table[b];
        if (sum >= 255) sum -= 255;
        return gf_exp_table[sum];
    }
    
    // 计算 GF 逆元
    inline uint8_t gf_inv(uint8_t a) {
        if (a == 0) return 0;  // 错误情况
        if (a == 1) return 1;
        int log_a = gf_log_table[a];
        int inv_log = 255 - log_a;
        return gf_exp_table[inv_log];
    }
}

// Galois Field 乘法（GF(2^8)，使用查找表优化）
uint8_t FecGroupBuffer::gf_mul(uint8_t a, uint8_t b) {
    // 确保查找表已初始化
    static bool initialized = false;
    if (!initialized) {
        init_gf_tables();
        initialized = true;
    }
    return gf_mul_fast(a, b);
}

// 构建范德蒙德矩阵（用于 RS 编码/解码）
srs_error_t FecGroupBuffer::build_vandermonde_matrix(
    std::vector<std::vector<uint8_t> >& matrix,
    const std::vector<uint32_t>& available_indices,
    const std::vector<uint32_t>& missing_indices,
    size_t block_size) {
    
    srs_error_t err = srs_success;
    
    size_t num_unknowns = missing_indices.size();
    size_t num_equations = num_unknowns;
    
    if (num_unknowns == 0) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE, "No missing blocks to solve");
    }
    
    matrix.resize(num_equations);
    for (size_t i = 0; i < num_equations; i++) {
        matrix[i].resize(num_unknowns);
    }
    
    // 构建范德蒙德矩阵：matrix[i][j] = alpha^(available_indices[i] * missing_indices[j])
    // 简化版本：使用索引作为幂次
    for (size_t i = 0; i < num_equations; i++) {
        if (i >= available_indices.size()) {
            return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                               "Not enough available indices: need %zu, have %zu",
                               num_equations, available_indices.size());
        }
        uint32_t avail_idx = available_indices[i];
        for (size_t j = 0; j < num_unknowns; j++) {
            uint32_t miss_idx = missing_indices[j];
            // 使用简化公式：alpha^(avail_idx * miss_idx) mod 255
            // 确保查找表已初始化
            static bool initialized = false;
            if (!initialized) {
                init_gf_tables();
                initialized = true;
            }
            
            uint8_t power = (avail_idx * miss_idx) % 255;
            if (power == 0) {
                matrix[i][j] = 1;
            } else {
                // 使用查找表快速计算 alpha^power
                matrix[i][j] = gf_exp_table[power];
            }
        }
    }
    
    return err;
}

// 高斯消元法求解线性方程组（GF(2^8)）
srs_error_t FecGroupBuffer::solve_linear_system(
    const std::vector<std::vector<uint8_t> >& matrix,
    const std::vector<uint8_t*>& rhs,
    std::vector<uint8_t*>& solution,
    size_t block_size,
    size_t num_unknowns) {
    
    srs_error_t err = srs_success;
    
    if (matrix.size() != num_unknowns || rhs.size() != num_unknowns) {
        return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                           "Matrix/RHS size mismatch: matrix=%zu, rhs=%zu, unknowns=%zu",
                           matrix.size(), rhs.size(), num_unknowns);
    }
    
    // 对每个字节位置独立求解
    for (size_t byte_pos = 0; byte_pos < block_size; byte_pos++) {
        // 构建增广矩阵 [A|b]
        std::vector<std::vector<uint8_t> > aug;
        aug.resize(num_unknowns);
        for (size_t i = 0; i < num_unknowns; i++) {
            aug[i].resize(num_unknowns + 1);
            for (size_t j = 0; j < num_unknowns; j++) {
                aug[i][j] = matrix[i][j];
            }
            aug[i][num_unknowns] = rhs[i][byte_pos];
        }
        
        // 高斯消元（行简化阶梯形）
        for (size_t col = 0; col < num_unknowns; col++) {
            // 找到主元
            size_t pivot_row = col;
            for (size_t row = col + 1; row < num_unknowns; row++) {
                if (aug[row][col] != 0) {
                    pivot_row = row;
                    break;
                }
            }
            
            // 交换行
            if (pivot_row != col) {
                std::swap(aug[col], aug[pivot_row]);
            }
            
            // 如果主元为0，方程组可能无解或无穷解
            if (aug[col][col] == 0) {
                return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                                   "Singular matrix at column %zu, byte %zu",
                                   col, byte_pos);
            }
            
            // 归一化主元行（使用 GF 逆元）
            uint8_t pivot_val = aug[col][col];
            if (pivot_val == 0) {
                return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                                   "Pivot is zero at column %zu, byte %zu",
                                   col, byte_pos);
            }
            
            // 计算主元的逆元
            uint8_t pivot_inv = 1;
            if (pivot_val != 1) {
                // 确保查找表已初始化
                static bool initialized = false;
                if (!initialized) {
                    init_gf_tables();
                    initialized = true;
                }
                pivot_inv = gf_inv(pivot_val);
                
                // 归一化主元行
                for (size_t c = col; c <= num_unknowns; c++) {
                    aug[col][c] = gf_mul_fast(aug[col][c], pivot_inv);
                }
            }
            
            // 消元（使用 GF 运算）
            for (size_t row = 0; row < num_unknowns; row++) {
                if (row == col) continue;
                if (aug[row][col] != 0) {
                    uint8_t factor = aug[row][col];  // 主元行已归一化，factor 就是 aug[row][col]
                    for (size_t c = col; c <= num_unknowns; c++) {
                        // aug[row][c] = aug[row][c] - factor * aug[col][c]
                        // 在 GF(2^8) 中，减法等于加法（XOR）
                        aug[row][c] ^= gf_mul_fast(factor, aug[col][c]);
                    }
                }
            }
        }
        
        // 回代求解
        for (size_t i = 0; i < num_unknowns; i++) {
            solution[i][byte_pos] = aug[i][num_unknowns];
        }
    }
    
    return err;
}

bool FecGroupBuffer::is_expired(int64_t current_time_ms) const {
    return (current_time_ms - create_time_ms_) > config_.repair_deadline_ms;
}

size_t FecGroupBuffer::get_received_count() const {
    return blocks_.size();
}

std::vector<uint32_t> FecGroupBuffer::get_missing_blocks() const {
    std::vector<uint32_t> missing;
    for (uint32_t i = 0; i < config_.k; i++) {
        if (blocks_.find(i) == blocks_.end() || blocks_.at(i)->is_parity) {
            missing.push_back(i);
        }
    }
    return missing;
}

// FecRepairManager实现
FecRepairManager::FecRepairManager() : max_groups_(100) {
    stats_ = Stats();
}

FecRepairManager::~FecRepairManager() {
    groups_.clear();
}

void FecRepairManager::set_config(const FecGroupConfig& config) {
    config_ = config;
}

srs_error_t FecRepairManager::create_group_if_needed(uint32_t group_id) {
    srs_error_t err = srs_success;
    
    if (groups_.find(group_id) == groups_.end()) {
        // 检查数量限制
        if (groups_.size() >= max_groups_) {
            // 清理最老的组
            int64_t oldest_time = INT64_MAX;
            uint32_t oldest_id = 0;
            srs_utime_t now_us = srs_time_now_cached();
            int64_t now_ms = now_us / 1000;
            
            for (const auto& pair : groups_) {
                if (pair.second->is_expired(now_ms)) {
                    oldest_id = pair.first;
                    break;
                }
            }
            
            if (oldest_id > 0) {
                groups_.erase(oldest_id);
                stats_.expired_groups++;
            } else {
                return srs_error_new(ERROR_RTMP_MESSAGE_DECODE,
                                   "FEC group buffer full: %zu >= %zu",
                                   groups_.size(), max_groups_);
            }
        }
        
        groups_[group_id] = std::unique_ptr<FecGroupBuffer>(
            new FecGroupBuffer(group_id, config_));
        stats_.total_groups++;
    }
    
    return err;
}

srs_error_t FecRepairManager::add_block(uint32_t group_id, uint32_t block_index,
                                         const uint8_t* data, size_t size,
                                         bool is_parity, int64_t timestamp_ms,
                                         uint64_t seq_num, bool is_keyframe) {
    srs_error_t err = srs_success;
    
    if ((err = create_group_if_needed(group_id)) != srs_success) {
        return err;
    }
    
    return groups_[group_id]->add_block(block_index, data, size, is_parity, timestamp_ms, seq_num, is_keyframe);
}

srs_error_t FecRepairManager::check_and_repair(std::vector<std::vector<uint8_t> >& restored_data) {
    srs_error_t err = srs_success;
    
    srs_utime_t now_us = srs_time_now_cached();
    int64_t now_ms = now_us / 1000;
    
    std::vector<uint32_t> groups_to_remove;
    
    for (auto& pair : groups_) {
        FecGroupBuffer* group = pair.second.get();
        
        // 检查是否超时
        if (group->is_expired(now_ms)) {
            groups_to_remove.push_back(pair.first);
            stats_.expired_groups++;
            continue;
        }
        
        // 检查是否可以修复
        if (group->can_repair()) {
            std::vector<std::unique_ptr<FecBlock> > restored_blocks;
            
            if ((err = group->repair(restored_blocks)) == srs_success) {
                // 将修复后的块转换为数据
                for (auto& block : restored_blocks) {
                    std::vector<uint8_t> data(block->data, block->data + block->data_size);
                    restored_data.push_back(std::move(data));
                }
                
                if (group->is_complete()) {
                    stats_.complete_groups++;
                } else {
                    stats_.repaired_groups++;
                }
                
                groups_to_remove.push_back(pair.first);
            } else {
                stats_.failed_repairs++;
                srs_warn("FEC repair failed for group %d: %s", 
                        pair.first, srs_error_desc(err).c_str());
                srs_freep(err);
            }
        }
    }
    
    // 移除已处理的组
    for (uint32_t id : groups_to_remove) {
        groups_.erase(id);
    }
    
    return srs_success;
}

srs_error_t FecRepairManager::check_and_repair_with_metadata(
    std::vector<std::vector<uint8_t> >& restored_data,
    std::vector<uint64_t>& seq_nums,
    std::vector<bool>& is_keyframes) {
    srs_error_t err = srs_success;
    
    srs_utime_t now_us = srs_time_now_cached();
    int64_t now_ms = now_us / 1000;
    
    std::vector<uint32_t> groups_to_remove;
    
    for (auto& pair : groups_) {
        FecGroupBuffer* group = pair.second.get();
        
        // 检查是否超时
        if (group->is_expired(now_ms)) {
            groups_to_remove.push_back(pair.first);
            stats_.expired_groups++;
            continue;
        }
        
        // 检查是否可以修复
        if (group->can_repair()) {
            std::vector<std::unique_ptr<FecBlock> > restored_blocks;
            
            if ((err = group->repair(restored_blocks)) == srs_success) {
                // 将修复后的块转换为数据，并保留元数据
                for (auto& block : restored_blocks) {
                    std::vector<uint8_t> data(block->data, block->data + block->data_size);
                    restored_data.push_back(std::move(data));
                    seq_nums.push_back(block->seq_num);
                    is_keyframes.push_back(block->is_keyframe);
                }
                
                if (group->is_complete()) {
                    stats_.complete_groups++;
                } else {
                    stats_.repaired_groups++;
                }
                
                groups_to_remove.push_back(pair.first);
            } else {
                stats_.failed_repairs++;
                srs_warn("FEC repair failed for group %d: %s", 
                        pair.first, srs_error_desc(err).c_str());
                srs_freep(err);
            }
        }
    }
    
    // 移除已处理的组
    for (uint32_t id : groups_to_remove) {
        groups_.erase(id);
    }
    
    return srs_success;
}

void FecRepairManager::cleanup_expired(int64_t current_time_ms) {
    std::vector<uint32_t> to_remove;
    
    for (const auto& pair : groups_) {
        if (pair.second->is_expired(current_time_ms)) {
            to_remove.push_back(pair.first);
        }
    }
    
    for (uint32_t id : to_remove) {
        groups_.erase(id);
        stats_.expired_groups++;
    }
}

void FecRepairManager::reset_stats() {
    stats_ = Stats();
}

