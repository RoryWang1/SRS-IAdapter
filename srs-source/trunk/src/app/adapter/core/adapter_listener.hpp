#pragma once
#include "iadapter.hpp"
#include <srs_app_listener.hpp>
#include <string>
#include <map>
#include <memory>

// 前向声明
class SrsServer;
class ISrsUdpHandler;
class ISrsIpListener;

// Adapter监听器类 - 使用UDP监听QUIC数据包
class AdapterListener {
public:
    AdapterListener(SrsServer* srs, const std::string& protocol_name);
    virtual ~AdapterListener();

public:
    // 监听接口
    srs_error_t listen(std::string ip, int port);
    void close();

    // 路由配置
    struct Route {
        std::string vhost;
        std::string app;
        std::string stream;
    };

    // 设置固定路由
    void set_fixed_route(const Route& r);
    // 增加端口映射路由
    void add_port_mapping(int port, const Route& r);

private:
    SrsServer* srs_;
    std::string protocol_name_;
    ISrsIpListener* listener_;
    std::unique_ptr<ISrsUdpHandler> handler_;
    Route fixed_route_;
    std::map<int, Route> port_mapping_;
};
