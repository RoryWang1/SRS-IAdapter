#include <srs_app_caster_flv.hpp>
#include <srs_app_server.hpp>
#include <srs_app_config.hpp>
#include <srs_app_listener.hpp>
#include <srs_kernel_log.hpp>
#include "adapter/core/adapter_manager.hpp"
#include "adapter/core/adapter_listener.hpp"
#include "adapter/adapters/quic_fec_ts_adapter.hpp"

class SrsQuicFecTsCasterListener : public ISrsListener {
public:
    SrsQuicFecTsCasterListener(SrsServer* srs, SrsConfDirective* conf);
    virtual ~SrsQuicFecTsCasterListener();

public:
    virtual srs_error_t listen();

private:
    SrsServer* srs_;
    SrsConfDirective* conf_;
    AdapterListener* listener_;

    std::string listen_ip_;
    int listen_port_;
    std::string output_url_;
    std::string vhost_;
    std::string app_;
    std::string stream_;
    
    srs_error_t parse_conf();
};

SrsQuicFecTsCasterListener::SrsQuicFecTsCasterListener(SrsServer* srs, SrsConfDirective* conf)
    : srs_(srs), conf_(conf), listener_(nullptr), listen_port_(8443) {
    listen_ip_ = "0.0.0.0";
    output_url_ = "rtmp://127.0.0.1/live/stream";
    vhost_ = "__defaultVhost__";
    app_ = "live";
    stream_ = "stream";
}

SrsQuicFecTsCasterListener::~SrsQuicFecTsCasterListener() {
    if (listener_) {
        listener_->close();
        delete listener_;
    }
}

srs_error_t SrsQuicFecTsCasterListener::listen() {
    srs_error_t err = srs_success;

    static auto factory_func = []() -> IAdapter* {
        return new QuicFecTsAdapter();
    };
    AdapterManager::instance().register_factory("quic_fec_ts", factory_func);

    if ((err = parse_conf()) != srs_success) {
        return srs_error_wrap(err, "parse quic_fec_ts conf");
    }

    listener_ = new AdapterListener(srs_, "quic_fec_ts");
    AdapterListener::Route fixed;
    fixed.vhost = vhost_;
    fixed.app = app_;
    fixed.stream = stream_;
    listener_->set_fixed_route(fixed);
    
    SrsConfDirective* route = conf_->get("route");
    if (route) {
        for (int i = 0; i < (int)route->directives_.size(); ++i) {
            SrsConfDirective* child = route->directives_.at(i);
            if (child->name_ == "mapping") {
                int map_port = 0;
                std::string mvhost = vhost_, mapp = app_, mstream = stream_;
                for (int j = 0; j < (int)child->directives_.size(); ++j) {
                    SrsConfDirective* item = child->directives_.at(j);
                    if (item->name_ == "port" && !item->args_.empty()) map_port = ::atoi(item->arg0().c_str());
                    else if (item->name_ == "vhost" && !item->args_.empty()) mvhost = item->arg0();
                    else if (item->name_ == "app" && !item->args_.empty()) mapp = item->arg0();
                    else if (item->name_ == "stream" && !item->args_.empty()) mstream = item->arg0();
                }
                if (map_port > 0) {
                    AdapterListener::Route r; r.vhost = mvhost; r.app = mapp; r.stream = mstream;
                    listener_->add_port_mapping(map_port, r);
                }
            }
        }
    }

    if ((err = listener_->listen(listen_ip_, listen_port_)) != srs_success) {
        return srs_error_wrap(err, "quic_fec_ts listen");
    }

    srs_trace("QuicFecTs caster listener started on %s:%d", listen_ip_.c_str(), listen_port_);

    return err;
}

srs_error_t SrsQuicFecTsCasterListener::parse_conf() {
    srs_error_t err = srs_success;
    
    // 解析 listen 配置
    // 格式1: listen 9000;  (只有端口)
    // 格式2: listen 0.0.0.0 9000;  (IP 和端口)
    SrsConfDirective* listen = conf_->get("listen");
    if (listen && !listen->args_.empty()) {
        if (listen->args_.size() == 1) {
            // 只有一个参数，假设是端口号
            listen_ip_ = "0.0.0.0";
            listen_port_ = ::atoi(listen->arg0().c_str());
        } else if (listen->args_.size() >= 2) {
            // 两个参数：IP 和端口
            listen_ip_ = listen->arg0();
            listen_port_ = ::atoi(listen->arg1().c_str());
        }
    }
    
    // 解析 output 配置
    SrsConfDirective* output = conf_->get("output");
    if (output && !output->args_.empty()) {
        output_url_ = output->arg0();
        
        // 解析 RTMP URL: rtmp://host:port/app/stream
        size_t pos = output_url_.find("://");
        if (pos != std::string::npos) {
            size_t start = pos + 3;
            size_t slash1 = output_url_.find('/', start);
            if (slash1 != std::string::npos) {
                size_t slash2 = output_url_.find('/', slash1 + 1);
                if (slash2 != std::string::npos) {
                    std::string host_port = output_url_.substr(start, slash1 - start);
                    app_ = output_url_.substr(slash1 + 1, slash2 - slash1 - 1);
                    stream_ = output_url_.substr(slash2 + 1);
                    
                    // 从 host:port 中提取 vhost（这里简化为 host）
                    size_t colon = host_port.find(':');
                    if (colon != std::string::npos) {
                        vhost_ = host_port.substr(0, colon);
                    } else {
                        vhost_ = host_port;
                    }
                }
            }
        }
    }
    
    srs_trace("QuicFecTs caster config: listen=%s:%d, output=%s, vhost=%s, app=%s, stream=%s",
              listen_ip_.c_str(), listen_port_, output_url_.c_str(),
              vhost_.c_str(), app_.c_str(), stream_.c_str());
    
    return err;
}

ISrsListener* srs_create_quic_fec_ts_caster_listener(SrsServer* srs, SrsConfDirective* conf) {
    return new SrsQuicFecTsCasterListener(srs, conf);
}

