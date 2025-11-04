#include "adapter_listener.hpp"
#include "adapter_manager.hpp"
#include "../components/quic/quic_udp_handler.hpp"
#include <srs_kernel_log.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_app_factory.hpp>
#include <srs_app_listener.hpp>
#include <map>

extern ISrsAppFactory* _srs_app_factory;

AdapterListener::AdapterListener(SrsServer* srs, const std::string& protocol_name)
    : srs_(srs), protocol_name_(protocol_name), listener_(nullptr) {
    // 创建UDP处理器
    handler_ = std::unique_ptr<ISrsUdpHandler>(new QuicUdpHandler(protocol_name));
}

AdapterListener::~AdapterListener() {
    close();
}

srs_error_t AdapterListener::listen(std::string ip, int port) {
    srs_error_t err = srs_success;
    
    if (!handler_) {
        return srs_error_new(ERROR_SYSTEM_IO_INVALID, "handler not initialized");
    }
    
    // 创建UDP监听器
    listener_ = new SrsUdpListener(handler_.get());
    if (!listener_) {
        return srs_error_new(ERROR_SYSTEM_IO_INVALID, "create udp listener failed");
    }
    
    std::string bind_ip = (ip == "0.0.0.0") ? srs_net_address_any() : ip;
    listener_->set_endpoint(bind_ip, port);
    listener_->set_label(protocol_name_);
    
    // 配置路由到handler
    QuicUdpHandler* quic_handler = dynamic_cast<QuicUdpHandler*>(handler_.get());
    if (quic_handler) {
        QuicUdpHandler::Route route;
        route.vhost = fixed_route_.vhost;
        route.app = fixed_route_.app;
        route.stream = fixed_route_.stream;
        quic_handler->set_fixed_route(route);
        for (const auto& pair : port_mapping_) {
            QuicUdpHandler::Route port_route;
            port_route.vhost = pair.second.vhost;
            port_route.app = pair.second.app;
            port_route.stream = pair.second.stream;
            quic_handler->add_port_mapping(pair.first, port_route);
        }
    }
    
    // 启动监听
    if ((err = listener_->listen()) != srs_success) {
        return srs_error_wrap(err, "udp listen");
    }
    
    srs_trace("Adapter listener started: %s://%s:%d", 
              protocol_name_.c_str(), ip.c_str(), port);
    
    return err;
}

void AdapterListener::close() {
    if (listener_) {
        listener_->close();
        srs_freep(listener_);
    }
    handler_.reset();
}

void AdapterListener::set_fixed_route(const Route& r) {
    fixed_route_ = r;
    QuicUdpHandler* quic_handler = dynamic_cast<QuicUdpHandler*>(handler_.get());
    if (quic_handler) {
        QuicUdpHandler::Route route;
        route.vhost = r.vhost;
        route.app = r.app;
        route.stream = r.stream;
        quic_handler->set_fixed_route(route);
    }
}

void AdapterListener::add_port_mapping(int port, const Route& r) {
    port_mapping_[port] = r;
    QuicUdpHandler* quic_handler = dynamic_cast<QuicUdpHandler*>(handler_.get());
    if (quic_handler) {
        QuicUdpHandler::Route route;
        route.vhost = r.vhost;
        route.app = r.app;
        route.stream = r.stream;
        quic_handler->add_port_mapping(port, route);
    }
}

