#include "remote_agent.h"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <utility>

namespace remote {

// Stand-in Tool so the TUI's `/tools <name>` can show description + schema for
// server-declared tools without a local runtime. execute() is never called
// client-side.
class StubTool : public Tool {
public:
    StubTool(std::string name, std::string description, nlohmann::json schema)
        : name_(std::move(name)), description_(std::move(description)),
          schema_(std::move(schema)) {}

    const char* name() const override { return name_.c_str(); }
    const char* description() const override { return description_.c_str(); }
    nlohmann::json parameters_schema() const override { return schema_; }
    std::string execute(const nlohmann::json&) override {
        return "tools execute on the server, not on this client";
    }

private:
    std::string name_;
    std::string description_;
    nlohmann::json schema_;
};

RemoteAgent::RemoteAgent(const std::string& host, int port)
    : Agent(nullptr),
      client_(std::make_unique<swarm::ServerClient>(host, port)),
      peer_(std::make_unique<RemotePeer>(this)) {}

RemoteAgent::~RemoteAgent() {
    {
        std::lock_guard<std::mutex> lk(turn_mu_);
        turn_active_ = false;
    }
    turn_cv_.notify_all();
    client_->disconnect();
}

bool RemoteAgent::connect(const std::string& repo, const std::string& name) {
    client_->set_handler([this](const std::string& type, const nlohmann::json& msg) {
        handle_frame(type, msg);
    });
    if (!client_->connect(repo, name)) {
        connected_.store(false);
        return false;
    }
    connected_.store(true);
    return true;
}

std::string RemoteAgent::provider_name() const {
    std::lock_guard<std::mutex> lk(cache_mu_);
    return cached_provider_;
}

std::string RemoteAgent::model() const {
    std::lock_guard<std::mutex> lk(cache_mu_);
    return cached_model_;
}

void RemoteAgent::set_model(const std::string& model) {
    {
        std::lock_guard<std::mutex> lk(cache_mu_);
        cached_model_ = model;
    }
    if (connected_.load()) client_->send_model(model);
}

bool RemoteAgent::set_credentials(const std::string& provider, const std::string& api_key) {
    if (!connected_.load()) return false;
    // The server swaps the hosted provider and replies with a fresh `state`
    // frame, which refreshes the cached provider/model in handle_frame().
    return client_->send_auth(provider, api_key);
}

TokenUsage RemoteAgent::usage() const {
    std::lock_guard<std::mutex> lk(cache_mu_);
    return cached_usage_;
}

int RemoteAgent::context_window() {
    std::lock_guard<std::mutex> lk(cache_mu_);
    return cached_ctx_total_;
}

std::vector<ToolDefinition> RemoteAgent::tools() const {
    std::lock_guard<std::mutex> lk(cache_mu_);
    return cached_tools_;
}

Tool* RemoteAgent::tool(const std::string& name) const {
    std::lock_guard<std::mutex> lk(cache_mu_);
    for (auto& stub : tool_stubs_) {
        if (name == stub->name()) return stub.get();
    }
    for (const auto& def : cached_tools_) {
        if (def.name == name) {
            tool_stubs_.push_back(std::make_unique<StubTool>(def.name, def.description, def.input_schema));
            return tool_stubs_.back().get();
        }
    }
    return nullptr;
}

void RemoteAgent::clear_messages() {
    if (connected_.load()) client_->send_clear();
}

bool RemoteAgent::swarm_connected() const {
    return connected_.load() && client_->connected();
}

swarm::SwarmPeer* RemoteAgent::swarm() const {
    return peer_.get();
}

void RemoteAgent::attach_swarm(std::unique_ptr<swarm::SwarmPeer>, bool) {
    // The remote facade *is* the connection.
}

void RemoteAgent::set_swarm_realtime(std::function<void(const std::string&)> realtime) {
    std::lock_guard<std::mutex> lk(turn_mu_);
    realtime_ = std::move(realtime);
}

void RemoteAgent::cancel_turn() {
    bool send = false;
    {
        std::lock_guard<std::mutex> lk(turn_mu_);
        if (turn_active_) {
            cancel_pending_ = true;
            send = connected_.load();
        }
    }
    if (send) client_->send_cancel();
}

int RemoteAgent::run_turn(const std::string& prompt, StreamSink& sink) {
    if (!connected_.load()) {
        sink.warning("not connected to a swarm server");
        return 1;
    }

    {
        std::lock_guard<std::mutex> lk(turn_mu_);
        turn_active_ = true;
        cancel_pending_ = false;
        turn_rc_ = 1;
        active_sink_ = &sink;
    }

    std::unique_lock<std::mutex> lk(turn_mu_);
    bool sent = client_->send_prompt(prompt);
    if (!sent) {
        turn_active_ = false;
        active_sink_ = nullptr;
        lk.unlock();
        sink.warning("failed to send prompt to swarm server");
        return 1;
    }
    turn_cv_.wait(lk, [this] { return !turn_active_; });
    int rc = turn_rc_;
    active_sink_ = nullptr;
    return rc;
}

// ---- swarm surface ----

bool RemoteAgent::knows_peer_id(const std::string& id) const {
    std::lock_guard<std::mutex> lk(cache_mu_);
    for (const auto& p : cached_peers_) {
        if (p.value("id", "") == id) return true;
    }
    return false;
}

bool RemoteAgent::swarm_send_message(const std::string& to, const std::string& text) {
    if (to.empty() || text.empty() || !connected_.load()) return false;
    return client_->send_message(to, text);
}

bool RemoteAgent::swarm_request_peers() {
    if (!connected_.load()) return false;
    return client_->request_peers();
}

bool RemoteAgent::RemotePeer::knows_peer(const std::string& id) const {
    return owner_->knows_peer_id(id);
}

bool RemoteAgent::RemotePeer::send_message(const std::string& to, const std::string& text) {
    return owner_->swarm_send_message(to, text);
}

bool RemoteAgent::RemotePeer::request_peers() {
    return owner_->swarm_request_peers();
}

std::vector<nlohmann::json> RemoteAgent::RemotePeer::peers() const {
    std::lock_guard<std::mutex> lk(owner_->cache_mu_);
    return owner_->cached_peers_;
}

// ---- frame dispatch (ServerClient reader thread) ----

void RemoteAgent::handle_frame(const std::string& type, const nlohmann::json& msg) {
    if (type == "state") {
        std::lock_guard<std::mutex> lk(cache_mu_);
        cached_provider_ = msg.value("provider", "");
        cached_model_ = msg.value("model", "");
        cached_ctx_total_ = msg.value("ctx", -1);
        cached_tools_.clear();
        tool_stubs_.clear();
        if (msg.contains("tools") && msg["tools"].is_array()) {
            for (const auto& t : msg["tools"]) {
                ToolDefinition def;
                def.name = t.value("name", "");
                def.description = t.value("description", "");
                def.input_schema = t.value("parameters", nlohmann::json::object());
                cached_tools_.push_back(std::move(def));
            }
        }
        return;
    }

    if (type == "agents_list") {
        std::lock_guard<std::mutex> lk(cache_mu_);
        cached_peers_.clear();
        if (msg.contains("agents") && msg["agents"].is_array()) {
            cached_peers_ = msg["agents"].get<std::vector<nlohmann::json>>();
        }
        return;
    }

    if (type == "ctx") {
        std::lock_guard<std::mutex> lk(cache_mu_);
        cached_ctx_total_ = msg.value("total", cached_ctx_total_);
        int used = cached_usage_.prompt_tokens.load();
        cached_usage_.prompt_tokens = msg.value("used", used);
        return;
    }

    if (type == "msg" || type == "conflict") {
        std::string line;
        if (type == "msg") {
            line = "[swarm] <" + msg.value("from", "?") + "> " + msg.value("text", "");
        } else {
            line = "[swarm] conflict: " + msg.value("path", "") + " was changed by " +
                   msg.value("by", "?") + " at " + msg.value("at", "?");
        }
        std::function<void(const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lk(turn_mu_);
            cb = realtime_;
        }
        if (cb) cb(line);
        return;
    }

    if (type == "shutdown" || type == "bye") {
        connected_.store(false);
        std::lock_guard<std::mutex> lk(turn_mu_);
        turn_active_ = false;
        turn_cv_.notify_all();
        return;
    }

    if (type == "stream") {
        std::lock_guard<std::mutex> lk(turn_mu_);
        if (!active_sink_ || !turn_active_) return;
        // After a cancel, the server may still flush in-flight chunks before
        // the abort lands; the user asked to stop, so drop the rest of this
        // turn's output.
        if (cancel_pending_) return;
        handle_stream_event(msg, *active_sink_);
        return;
    }

    if (type == "turn_done") {
        std::lock_guard<std::mutex> lk(turn_mu_);
        turn_rc_ = msg.value("rc", 1);
        cancel_pending_ = false;
        turn_active_ = false;
        turn_cv_.notify_all();
    }
}

void RemoteAgent::handle_stream_event(const nlohmann::json& msg, StreamSink& sink) {
    // Called with turn_mu_ held; never re-acquire it here.
    const std::string event = msg.value("event", "");
    if (event == "header") {
        sink.header(msg.value("provider", ""), msg.value("model", ""));
    } else if (event == "text_delta") {
        sink.text_delta(msg.value("text", ""));
    } else if (event == "tool_start") {
        sink.tool_start(msg.value("name", ""));
    } else if (event == "tool_call") {
        sink.tool_call(msg.value("name", ""),
                       msg.value("args", nlohmann::json::object()));
    } else if (event == "tool_result") {
        sink.tool_result(msg.value("text", ""), msg.value("is_error", false));
    } else if (event == "summary") {
        TokenUsage u;
        u.prompt_tokens = msg.value("prompt", 0);
        u.completion_tokens = msg.value("completion", 0);
        {
            std::lock_guard<std::mutex> lk(cache_mu_);
            cached_usage_ = u;
        }
        sink.summary(msg.value("model", ""), u.prompt_tokens, u.completion_tokens);
    } else if (event == "warning") {
        sink.warning(msg.value("text", ""));
    }
}

}  // namespace remote