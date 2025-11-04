#include "adapter_stats.hpp"
#include <srs_app_http_api.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_protocol_json.hpp>
#include <srs_core_autofree.hpp>
#include <srs_core_time.hpp>
#include <mutex>
#include <algorithm>

// AdapterStatsManager实现
AdapterStatsManager& AdapterStatsManager::instance() {
    static AdapterStatsManager instance;
    return instance;
}

void AdapterStatsManager::add_connection(const std::string& id, const std::string& protocol,
                                        const std::string& vhost, const std::string& app, const std::string& stream,
                                        const std::string& client_ip, int client_port) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::unique_ptr<AdapterConnection> conn(new AdapterConnection(id, protocol, vhost, app, stream, client_ip, client_port));
    connections_[id] = std::move(conn);
    
    global_stats_.total_connections++;
    global_stats_.active_connections++;
    
    srs_trace("Adapter connection added: %s (%s://%s/%s/%s)", 
              id.c_str(), protocol.c_str(), vhost.c_str(), app.c_str(), stream.c_str());
}

void AdapterStatsManager::remove_connection(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = connections_.find(id);
    if (it != connections_.end()) {
        it->second->is_active = false;
        global_stats_.active_connections--;
        connections_.erase(it);
        
        srs_trace("Adapter connection removed: %s", id.c_str());
    }
}

AdapterConnection* AdapterStatsManager::get_connection(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = connections_.find(id);
    if (it != connections_.end()) {
        return it->second.get();
    }
    return nullptr;
}

void AdapterStatsManager::update_frame_stats(const std::string& id, bool is_video, bool is_keyframe, bool is_dropped) {
    AdapterConnection* conn = get_connection(id);
    if (!conn) return;
    
    conn->stats.total_frames++;
    global_stats_.total_frames++;
    
    if (is_video) {
        conn->stats.video_frames++;
        global_stats_.video_frames++;
        
        if (is_keyframe) {
            conn->stats.keyframes++;
            global_stats_.keyframes++;
        }
    } else {
        conn->stats.audio_frames++;
        global_stats_.audio_frames++;
    }
    
    if (is_dropped) {
        conn->stats.dropped_frames++;
        global_stats_.dropped_frames++;
    }
}

void AdapterStatsManager::update_jitter_stats(const std::string& id, bool hit) {
    AdapterConnection* conn = get_connection(id);
    if (!conn) return;
    
    if (hit) {
        conn->stats.jitter_buffer_hits++;
        global_stats_.jitter_buffer_hits++;
    } else {
        conn->stats.jitter_buffer_misses++;
        global_stats_.jitter_buffer_misses++;
    }
}

void AdapterStatsManager::update_zero_copy_stats(const std::string& id, bool hit) {
    AdapterConnection* conn = get_connection(id);
    if (!conn) return;
    
    if (hit) {
        conn->stats.zero_copy_hits++;
        global_stats_.zero_copy_hits++;
    } else {
        conn->stats.zero_copy_misses++;
        global_stats_.zero_copy_misses++;
    }
}

void AdapterStatsManager::update_error_stats(const std::string& id, const std::string& error_type) {
    AdapterConnection* conn = get_connection(id);
    if (!conn) return;
    
    if (error_type == "parse") {
        conn->stats.parse_errors++;
        global_stats_.parse_errors++;
    } else if (error_type == "timestamp") {
        conn->stats.timestamp_errors++;
        global_stats_.timestamp_errors++;
    } else if (error_type == "codec") {
        conn->stats.codec_errors++;
        global_stats_.codec_errors++;
    }
    
    conn->stats.failed_connections++;
    global_stats_.failed_connections++;
}

void AdapterStatsManager::update_first_frame_time(const std::string& id) {
    AdapterConnection* conn = get_connection(id);
    if (!conn) return;
    
    if (conn->first_frame_time == conn->connect_time) {
        conn->first_frame_time = std::chrono::steady_clock::now();
        conn->stats.first_frame_time_ms = conn->get_first_frame_latency_ms();
    }
}

std::map<std::string, AdapterConnection*> AdapterStatsManager::get_all_connections() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::map<std::string, AdapterConnection*> result;
    for (auto& pair : connections_) {
        result[pair.first] = pair.second.get();
    }
    return result;
}

AdapterStats AdapterStatsManager::get_global_stats() {
    // 由于AdapterStats现在使用非atomic类型，可以直接拷贝
    std::lock_guard<std::mutex> lock(mutex_);
    return global_stats_;
}

std::string AdapterStatsManager::to_json() {
    // 使用SRS的JSON API（直接使用原始指针，set/add方法会管理所有权）
    SrsJsonObject* root = SrsJsonAny::object();
    SrsJsonArray* connections = SrsJsonAny::array();
    SrsJsonObject* global_stats = SrsJsonAny::object();
    
    // 全局统计
    std::lock_guard<std::mutex> lock(mutex_);
    global_stats->set("total_connections", SrsJsonAny::integer(global_stats_.total_connections));
    global_stats->set("active_connections", SrsJsonAny::integer(global_stats_.active_connections));
    global_stats->set("failed_connections", SrsJsonAny::integer(global_stats_.failed_connections));
    global_stats->set("total_frames", SrsJsonAny::integer(global_stats_.total_frames));
    global_stats->set("video_frames", SrsJsonAny::integer(global_stats_.video_frames));
    global_stats->set("audio_frames", SrsJsonAny::integer(global_stats_.audio_frames));
    global_stats->set("keyframes", SrsJsonAny::integer(global_stats_.keyframes));
    global_stats->set("dropped_frames", SrsJsonAny::integer(global_stats_.dropped_frames));
    global_stats->set("drop_rate_percent", SrsJsonAny::number(global_stats_.get_drop_rate()));
    global_stats->set("jitter_hit_rate_percent", SrsJsonAny::number(global_stats_.get_jitter_hit_rate()));
    global_stats->set("zero_copy_hit_rate_percent", SrsJsonAny::number(global_stats_.get_zero_copy_hit_rate()));
    global_stats->set("uptime_ms", SrsJsonAny::integer(global_stats_.get_uptime_ms()));
    
    // 连接列表
    auto conns = get_all_connections();
    for (auto& pair : conns) {
        SrsJsonObject* conn = SrsJsonAny::object();
        AdapterConnection* c = pair.second;
        
        conn->set("id", SrsJsonAny::str(c->connection_id.c_str()));
        conn->set("protocol", SrsJsonAny::str(c->protocol.c_str()));
        conn->set("vhost", SrsJsonAny::str(c->vhost.c_str()));
        conn->set("app", SrsJsonAny::str(c->app.c_str()));
        conn->set("stream", SrsJsonAny::str(c->stream.c_str()));
        conn->set("client_ip", SrsJsonAny::str(c->client_ip.c_str()));
        conn->set("client_port", SrsJsonAny::integer(c->client_port));
        conn->set("is_active", SrsJsonAny::boolean(c->is_active.load()));
        conn->set("connection_duration_ms", SrsJsonAny::integer(c->get_connection_duration_ms()));
        conn->set("first_frame_latency_ms", SrsJsonAny::integer(c->get_first_frame_latency_ms()));
        
        // 连接统计
        SrsJsonObject* stats = SrsJsonAny::object();
        stats->set("total_frames", SrsJsonAny::integer(c->stats.total_frames));
        stats->set("video_frames", SrsJsonAny::integer(c->stats.video_frames));
        stats->set("audio_frames", SrsJsonAny::integer(c->stats.audio_frames));
        stats->set("keyframes", SrsJsonAny::integer(c->stats.keyframes));
        stats->set("dropped_frames", SrsJsonAny::integer(c->stats.dropped_frames));
        stats->set("drop_rate_percent", SrsJsonAny::number(c->stats.get_drop_rate()));
        stats->set("jitter_hit_rate_percent", SrsJsonAny::number(c->stats.get_jitter_hit_rate()));
        stats->set("zero_copy_hit_rate_percent", SrsJsonAny::number(c->stats.get_zero_copy_hit_rate()));
        stats->set("parse_errors", SrsJsonAny::integer(c->stats.parse_errors));
        stats->set("timestamp_errors", SrsJsonAny::integer(c->stats.timestamp_errors));
        stats->set("codec_errors", SrsJsonAny::integer(c->stats.codec_errors));
        
        conn->set("stats", stats);  // set方法会管理stats的所有权
        connections->append(conn);  // append方法会管理conn的所有权
    }
    
    root->set("global_stats", global_stats);  // set方法会管理global_stats的所有权
    root->set("connections", connections);  // set方法会管理connections的所有权
    root->set("timestamp", SrsJsonAny::integer((int64_t)(srs_time_now_cached() / 1000)));
    
    std::string result = root->dumps();
    srs_freep(root);  // 释放root对象（它会自动释放其包含的所有子对象）
    return result;
}

std::string AdapterStatsManager::get_connection_json(const std::string& id) {
    AdapterConnection* conn = get_connection(id);
    if (!conn) {
        SrsJsonObject* empty_obj = SrsJsonAny::object();
        std::string result = empty_obj->dumps();
        srs_freep(empty_obj);
        return result;
    }
    
    // 使用SRS的JSON API（直接使用原始指针，set方法会管理所有权）
    SrsJsonObject* root = SrsJsonAny::object();
    root->set("id", SrsJsonAny::str(conn->connection_id.c_str()));
    root->set("protocol", SrsJsonAny::str(conn->protocol.c_str()));
    root->set("vhost", SrsJsonAny::str(conn->vhost.c_str()));
    root->set("app", SrsJsonAny::str(conn->app.c_str()));
    root->set("stream", SrsJsonAny::str(conn->stream.c_str()));
    root->set("client_ip", SrsJsonAny::str(conn->client_ip.c_str()));
    root->set("client_port", SrsJsonAny::integer(conn->client_port));
    root->set("is_active", SrsJsonAny::boolean(conn->is_active.load()));
    root->set("connection_duration_ms", SrsJsonAny::integer(conn->get_connection_duration_ms()));
    root->set("first_frame_latency_ms", SrsJsonAny::integer(conn->get_first_frame_latency_ms()));
    
    // 详细统计
    SrsJsonObject* stats = SrsJsonAny::object();
    stats->set("total_frames", SrsJsonAny::integer(conn->stats.total_frames));
    stats->set("video_frames", SrsJsonAny::integer(conn->stats.video_frames));
    stats->set("audio_frames", SrsJsonAny::integer(conn->stats.audio_frames));
    stats->set("keyframes", SrsJsonAny::integer(conn->stats.keyframes));
    stats->set("dropped_frames", SrsJsonAny::integer(conn->stats.dropped_frames));
    stats->set("drop_rate_percent", SrsJsonAny::number(conn->stats.get_drop_rate()));
    stats->set("jitter_hit_rate_percent", SrsJsonAny::number(conn->stats.get_jitter_hit_rate()));
    stats->set("zero_copy_hit_rate_percent", SrsJsonAny::number(conn->stats.get_zero_copy_hit_rate()));
    stats->set("parse_errors", SrsJsonAny::integer(conn->stats.parse_errors));
    stats->set("timestamp_errors", SrsJsonAny::integer(conn->stats.timestamp_errors));
    stats->set("codec_errors", SrsJsonAny::integer(conn->stats.codec_errors));
    stats->set("cpu_usage_percent", SrsJsonAny::number(conn->stats.cpu_usage_percent));
    stats->set("memory_usage_bytes", SrsJsonAny::integer(conn->stats.memory_usage_bytes));
    stats->set("peak_memory_usage_bytes", SrsJsonAny::integer(conn->stats.peak_memory_usage_bytes));
    
    root->set("stats", stats);  // set方法会管理stats的所有权
    root->set("timestamp", SrsJsonAny::integer((int64_t)(srs_time_now_cached() / 1000)));
    
    std::string result = root->dumps();
    srs_freep(root);  // 释放root对象（它会自动释放其包含的所有子对象）
    return result;
}

// HTTP API处理器实现
SrsAdapterHttpApiHandler::SrsAdapterHttpApiHandler() {
}

SrsAdapterHttpApiHandler::~SrsAdapterHttpApiHandler() {
}

srs_error_t SrsAdapterHttpApiHandler::serve_http(ISrsHttpResponseWriter* w, ISrsHttpMessage* r) {
    srs_error_t err = srs_success;
    
    std::string path = r->path();
    
    if (path == "/api/v1/adapters") {
        err = handle_adapters_api(w, r);
    } else if (path.find("/api/v1/adapters/") == 0) {
        std::string id = path.substr(18); // 移除 "/api/v1/adapters/"
        err = handle_connection_api(w, r, id);
    } else if (path == "/api/v1/adapters/stats") {
        err = handle_stats_api(w, r);
    } else {
        return srs_error_new(ERROR_SOURCE_NOT_FOUND, "Not found");
    }
    
    return err;
}

srs_error_t SrsAdapterHttpApiHandler::handle_adapters_api(ISrsHttpResponseWriter* w, ISrsHttpMessage* r) {
    std::string json_data = AdapterStatsManager::instance().to_json();
    
    // 使用SRS标准API响应格式
    SrsUniquePtr<SrsJsonObject> obj(SrsJsonAny::object());
    obj->set("code", SrsJsonAny::integer(ERROR_SUCCESS));
    
    // 解析JSON字符串为JSON对象
    SrsJsonAny* data_json = SrsJsonAny::loads(json_data);
    if (data_json) {
        obj->set("data", data_json);  // set方法会管理data_json的所有权
    } else {
        // 如果解析失败，作为字符串存储
        obj->set("data", SrsJsonAny::str(json_data.c_str()));
    }
    
    return srs_api_response(w, r, obj->dumps());
}

srs_error_t SrsAdapterHttpApiHandler::handle_connection_api(ISrsHttpResponseWriter* w, ISrsHttpMessage* r, const std::string& id) {
    std::string json_data = AdapterStatsManager::instance().get_connection_json(id);
    
    if (json_data == "{}") {
        SrsUniquePtr<SrsJsonObject> obj(SrsJsonAny::object());
        obj->set("code", SrsJsonAny::integer(ERROR_SOURCE_NOT_FOUND));
        obj->set("data", SrsJsonAny::str("Connection not found"));
        return srs_api_response(w, r, obj->dumps());
    }
    
    // 使用SRS标准API响应格式
    SrsUniquePtr<SrsJsonObject> obj(SrsJsonAny::object());
    obj->set("code", SrsJsonAny::integer(ERROR_SUCCESS));
    
    SrsJsonAny* data_json = SrsJsonAny::loads(json_data);
    if (data_json) {
        obj->set("data", data_json);  // set方法会管理data_json的所有权
    } else {
        obj->set("data", SrsJsonAny::str(json_data.c_str()));
    }
    
    return srs_api_response(w, r, obj->dumps());
}

srs_error_t SrsAdapterHttpApiHandler::handle_stats_api(ISrsHttpResponseWriter* w, ISrsHttpMessage* r) {
    AdapterStats global_stats = AdapterStatsManager::instance().get_global_stats();
    
    // 使用SRS JSON API构建响应
    SrsUniquePtr<SrsJsonObject> obj(SrsJsonAny::object());
    obj->set("code", SrsJsonAny::integer(ERROR_SUCCESS));
    
    SrsJsonObject* data = SrsJsonAny::object();
    data->set("total_connections", SrsJsonAny::integer(global_stats.total_connections));
    data->set("active_connections", SrsJsonAny::integer(global_stats.active_connections));
    data->set("failed_connections", SrsJsonAny::integer(global_stats.failed_connections));
    data->set("total_frames", SrsJsonAny::integer(global_stats.total_frames));
    data->set("video_frames", SrsJsonAny::integer(global_stats.video_frames));
    data->set("audio_frames", SrsJsonAny::integer(global_stats.audio_frames));
    data->set("keyframes", SrsJsonAny::integer(global_stats.keyframes));
    data->set("dropped_frames", SrsJsonAny::integer(global_stats.dropped_frames));
    data->set("drop_rate_percent", SrsJsonAny::number(global_stats.get_drop_rate()));
    data->set("jitter_hit_rate_percent", SrsJsonAny::number(global_stats.get_jitter_hit_rate()));
    data->set("zero_copy_hit_rate_percent", SrsJsonAny::number(global_stats.get_zero_copy_hit_rate()));
    data->set("uptime_ms", SrsJsonAny::integer(global_stats.get_uptime_ms()));
    data->set("timestamp", SrsJsonAny::integer((int64_t)(srs_time_now_cached() / 1000)));
    
    obj->set("data", data);  // set方法会管理data的所有权
    
    return srs_api_response(w, r, obj->dumps());
}
