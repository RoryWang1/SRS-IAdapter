#include "adapter_manager.hpp"
#include <srs_kernel_log.hpp>

AdapterManager& AdapterManager::instance() {
    static AdapterManager instance;
    return instance;
}

void AdapterManager::register_factory(const std::string& name, AdapterFactory factory) {
    factories_[name] = factory;
    srs_trace("Registered adapter factory: %s", name.c_str());
}

IAdapter* AdapterManager::create(const std::string& name) {
    auto it = factories_.find(name);
    if (it == factories_.end()) {
        srs_error("Adapter factory not found: %s", name.c_str());
        return nullptr;
    }
    
    IAdapter* adapter = it->second();
    if (adapter) {
        srs_trace("Created adapter instance: %s", name.c_str());
    }
    return adapter;
}

srs_error_t AdapterManager::route_and_start(const std::string& caster_name,
                                           const AdapterInit& init) {
    srs_error_t err = srs_success;
    
    IAdapter* adapter = create(caster_name);
    if (!adapter) {
        return srs_error_new(ERROR_RTMP_MESSAGE_CREATE, 
                            "Failed to create adapter: %s", caster_name.c_str());
    }
    
    err = adapter->start(init);
    if (err != srs_success) {
        adapter->close();
        delete adapter;
        return srs_error_wrap(err, "Failed to start adapter: %s", caster_name.c_str());
    }
    
    srs_trace("Adapter started successfully: %s -> %s/%s/%s", 
              caster_name.c_str(), init.vhost.c_str(), init.app.c_str(), init.stream.c_str());
    
    return err;
}

std::vector<std::string> AdapterManager::get_registered_adapters() const {
    std::vector<std::string> names;
    for (const auto& pair : factories_) {
        names.push_back(pair.first);
    }
    return names;
}
