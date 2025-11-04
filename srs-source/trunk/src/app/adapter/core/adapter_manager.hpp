#pragma once
#include "iadapter.hpp"
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <functional>

// Adapter管理器类
class AdapterManager {
public:
    static AdapterManager& instance();
    
    // 注册适配器工厂
    void register_factory(const std::string& name, AdapterFactory factory);
    
    // 创建适配器实例
    IAdapter* create(const std::string& name);
    
    // 路由并启动适配器
    srs_error_t route_and_start(const std::string& caster_name,
                                const AdapterInit& init);
    
    // 获取已注册的适配器列表
    std::vector<std::string> get_registered_adapters() const;

private:
    AdapterManager() = default;
    ~AdapterManager() = default;
    AdapterManager(const AdapterManager&) = delete;
    AdapterManager& operator=(const AdapterManager&) = delete;
    
    std::map<std::string, AdapterFactory> factories_;
};

