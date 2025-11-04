#pragma once
#include <string>
#include <vector>
#include <map>
#include <stdint.h>
#ifdef __cplusplus
#if __cplusplus >= 201103L || defined(__GXX_EXPERIMENTAL_CXX0X__)
#include <functional>
#define HAS_STD_FUNCTION 1
#else
#define HAS_STD_FUNCTION 0
#endif
#else
#define HAS_STD_FUNCTION 0
#endif

// SRS核心头文件
#include <srs_core.hpp>
#include <srs_kernel_error.hpp>

// 包含标准帧定义
#include "../common/std_frame.hpp"

// 初始化参数结构
struct AdapterInit {
    std::string vhost;
    std::string app;
    std::string stream;
    std::map<std::string, std::string> kv; // 协议私有参数
    
    // 构造函数
    AdapterInit() = default;
    AdapterInit(const std::string& v, const std::string& a, const std::string& s) 
        : vhost(v), app(a), stream(s) {}
    
    // 获取参数
    std::string get_param(const std::string& key, const std::string& default_value = "") const {
        auto it = kv.find(key);
        return (it != kv.end()) ? it->second : default_value;
    }
    
    // 设置参数
    void set_param(const std::string& key, const std::string& value) {
        kv[key] = value;
    }
    
    // 获取整数参数
    int get_int_param(const std::string& key, int default_value = 0) const {
        auto it = kv.find(key);
        if (it != kv.end()) {
            try {
                return std::stoi(it->second);
            } catch (...) {
                return default_value;
            }
        }
        return default_value;
    }
    
    // 获取布尔参数
    bool get_bool_param(const std::string& key, bool default_value = false) const {
        auto it = kv.find(key);
        if (it != kv.end()) {
            return it->second == "true" || it->second == "1" || it->second == "on";
        }
        return default_value;
    }
};

// 流事件回调
// 前向声明IAdapter类
class IAdapter;

// 流事件回调
#if HAS_STD_FUNCTION
typedef std::function<void(const std::string& vhost, const std::string& app, const std::string& stream)> OnStartStreamCallback;
typedef std::function<void()> OnStopStreamCallback;
typedef std::function<IAdapter*()> AdapterFactory;
#else
// C++98兼容：使用函数指针
typedef void (*OnStartStreamCallback)(const std::string& vhost, const std::string& app, const std::string& stream);
typedef void (*OnStopStreamCallback)();
typedef IAdapter* (*AdapterFactory)();
#endif

// Adapter抽象接口
class IAdapter {
public:
    virtual ~IAdapter() {}
    
    // 初始化适配器
    virtual srs_error_t start(const AdapterInit& init) = 0;
    
    // 喂入原始数据
    virtual srs_error_t feed(const uint8_t* data, size_t nbytes) = 0;
    
    // 解析并产出标准帧
    virtual srs_error_t parseFrame() = 0;
    
    // 刷新缓冲区
    virtual srs_error_t flush() = 0;
    
    // 关闭适配器
    virtual void close() = 0;
    
    // 设置回调函数
    virtual void setOnStartStream(OnStartStreamCallback callback) = 0;
    virtual void setOnStopStream(OnStopStreamCallback callback) = 0;
};

