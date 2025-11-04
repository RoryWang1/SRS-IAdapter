#pragma once
#include "../../common/std_frame.hpp"
#include <map>
#include <vector>
#include <mutex>
#include <atomic>

// 参数集类型
enum class ParameterSetType {
    SPS,    // H.264 Sequence Parameter Set
    PPS,    // H.264 Picture Parameter Set
    VPS,    // H.265 Video Parameter Set
    ASC,    // AAC Audio Specific Config
    OPUS_HEADER, // Opus header
    UNKNOWN
};

// 参数集信息
struct ParameterSetInfo {
    ParameterSetType type;
    std::vector<uint8_t> data;
    int64_t timestamp_ms;
    bool is_valid;
    
    ParameterSetInfo() : type(ParameterSetType::UNKNOWN), timestamp_ms(0), is_valid(false) {}
    
    ParameterSetInfo(ParameterSetType t, const std::vector<uint8_t>& d, int64_t ts = 0) 
        : type(t), data(d), timestamp_ms(ts), is_valid(true) {}
};

// 参数集管理器
class ParameterSetManager {
public:
    ParameterSetManager();
    ~ParameterSetManager();
    
    // 更新参数集
    void update_parameter_set(ParameterSetType type, const std::vector<uint8_t>& data, int64_t timestamp_ms = 0);
    
    // 获取参数集
    std::vector<uint8_t> get_parameter_set(ParameterSetType type) const;
    
    // 获取所有参数集（用于关键帧）
    std::vector<ParameterSetInfo> get_all_parameter_sets() const;
    
    // 检查参数集是否存在
    bool has_parameter_set(ParameterSetType type) const;
    
    // 检查参数集是否有效
    bool is_parameter_set_valid(ParameterSetType type) const;
    
    // 清除参数集
    void clear_parameter_set(ParameterSetType type);
    
    // 清除所有参数集
    void clear_all();
    
    // 验证参数集
    bool validate_parameter_set(ParameterSetType type, const std::vector<uint8_t>& data) const;
    
    // 获取参数集统计信息（使用非atomic类型以便拷贝返回）
    struct Stats {
        uint64_t total_updates;
        uint64_t valid_updates;
        uint64_t invalid_updates;
        uint64_t sps_count;
        uint64_t pps_count;
        uint64_t vps_count;
        uint64_t asc_count;
        
        Stats()
            : total_updates(0), valid_updates(0), invalid_updates(0),
              sps_count(0), pps_count(0), vps_count(0), asc_count(0) {
        }
    };
    
    Stats get_stats() const;
    void reset_stats();

private:
    mutable std::mutex mutex_;
    std::map<ParameterSetType, ParameterSetInfo> parameter_sets_;
    Stats stats_;
    
    // 内部方法
    void update_stats(ParameterSetType type, bool valid);
    bool validate_h264_sps(const std::vector<uint8_t>& data) const;
    bool validate_h264_pps(const std::vector<uint8_t>& data) const;
    bool validate_h265_vps(const std::vector<uint8_t>& data) const;
    bool validate_h265_sps(const std::vector<uint8_t>& data) const;
    bool validate_h265_pps(const std::vector<uint8_t>& data) const;
    bool validate_aac_asc(const std::vector<uint8_t>& data) const;
};

// 参数集管理器实现
ParameterSetManager::ParameterSetManager() = default;

ParameterSetManager::~ParameterSetManager() = default;

void ParameterSetManager::update_parameter_set(ParameterSetType type, const std::vector<uint8_t>& data, int64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    bool valid = validate_parameter_set(type, data);
    update_stats(type, valid);
    
    if (valid) {
        parameter_sets_[type] = ParameterSetInfo(type, data, timestamp_ms);
    }
}

std::vector<uint8_t> ParameterSetManager::get_parameter_set(ParameterSetType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = parameter_sets_.find(type);
    if (it != parameter_sets_.end() && it->second.is_valid) {
        return it->second.data;
    }
    
    return std::vector<uint8_t>();
}

std::vector<ParameterSetInfo> ParameterSetManager::get_all_parameter_sets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<ParameterSetInfo> result;
    for (const auto& pair : parameter_sets_) {
        if (pair.second.is_valid) {
            result.push_back(pair.second);
        }
    }
    
    return result;
}

bool ParameterSetManager::has_parameter_set(ParameterSetType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = parameter_sets_.find(type);
    return it != parameter_sets_.end() && it->second.is_valid;
}

bool ParameterSetManager::is_parameter_set_valid(ParameterSetType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = parameter_sets_.find(type);
    return it != parameter_sets_.end() && it->second.is_valid;
}

void ParameterSetManager::clear_parameter_set(ParameterSetType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    parameter_sets_.erase(type);
}

void ParameterSetManager::clear_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    parameter_sets_.clear();
}

bool ParameterSetManager::validate_parameter_set(ParameterSetType type, const std::vector<uint8_t>& data) const {
    if (data.empty()) {
        return false;
    }
    
    int type_int = static_cast<int>(type);
    switch (type_int) {
        case static_cast<int>(ParameterSetType::SPS):
            return validate_h264_sps(data);
        case static_cast<int>(ParameterSetType::PPS):
            return validate_h264_pps(data);
        case static_cast<int>(ParameterSetType::VPS):
            return validate_h265_vps(data);
        case static_cast<int>(ParameterSetType::ASC):
            return validate_aac_asc(data);
        default:
            return true; // 其他类型暂时不验证
    }
}

void ParameterSetManager::update_stats(ParameterSetType type, bool valid) {
    stats_.total_updates++;
    
    if (valid) {
        stats_.valid_updates++;
        int type_int = static_cast<int>(type);
        switch (type_int) {
            case static_cast<int>(ParameterSetType::SPS):
                stats_.sps_count++;
                break;
            case static_cast<int>(ParameterSetType::PPS):
                stats_.pps_count++;
                break;
            case static_cast<int>(ParameterSetType::VPS):
                stats_.vps_count++;
                break;
            case static_cast<int>(ParameterSetType::ASC):
                stats_.asc_count++;
                break;
            default:
                break;
        }
    } else {
        stats_.invalid_updates++;
    }
}

bool ParameterSetManager::validate_h264_sps(const std::vector<uint8_t>& data) const {
    // 简化的H.264 SPS验证
    if (data.size() < 4) return false;
    
    // 检查NALU类型（SPS = 7）
    uint8_t nalu_type = data[0] & 0x1F;
    return nalu_type == 7;
}

bool ParameterSetManager::validate_h264_pps(const std::vector<uint8_t>& data) const {
    // 简化的H.264 PPS验证
    if (data.size() < 4) return false;
    
    // 检查NALU类型（PPS = 8）
    uint8_t nalu_type = data[0] & 0x1F;
    return nalu_type == 8;
}

bool ParameterSetManager::validate_h265_vps(const std::vector<uint8_t>& data) const {
    // 简化的H.265 VPS验证
    if (data.size() < 4) return false;
    
    // 检查NALU类型（VPS = 32）
    uint8_t nalu_type = (data[0] >> 1) & 0x3F;
    return nalu_type == 32;
}

bool ParameterSetManager::validate_h265_sps(const std::vector<uint8_t>& data) const {
    // 简化的H.265 SPS验证
    if (data.size() < 4) return false;
    
    // 检查NALU类型（SPS = 33）
    uint8_t nalu_type = (data[0] >> 1) & 0x3F;
    return nalu_type == 33;
}

bool ParameterSetManager::validate_h265_pps(const std::vector<uint8_t>& data) const {
    // 简化的H.265 PPS验证
    if (data.size() < 4) return false;
    
    // 检查NALU类型（PPS = 34）
    uint8_t nalu_type = (data[0] >> 1) & 0x3F;
    return nalu_type == 34;
}

bool ParameterSetManager::validate_aac_asc(const std::vector<uint8_t>& data) const {
    // 简化的AAC ASC验证
    if (data.size() < 2) return false;
    
    // 检查AAC配置的基本结构
    // 这里可以添加更详细的AAC ASC验证逻辑
    return true;
}

ParameterSetManager::Stats ParameterSetManager::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats result;
    result.total_updates = stats_.total_updates;
    result.valid_updates = stats_.valid_updates;
    result.invalid_updates = stats_.invalid_updates;
    result.sps_count = stats_.sps_count;
    result.pps_count = stats_.pps_count;
    result.vps_count = stats_.vps_count;
    result.asc_count = stats_.asc_count;
    return result;
}

void ParameterSetManager::reset_stats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_updates = 0;
    stats_.valid_updates = 0;
    stats_.invalid_updates = 0;
    stats_.sps_count = 0;
    stats_.pps_count = 0;
    stats_.vps_count = 0;
    stats_.asc_count = 0;
}
