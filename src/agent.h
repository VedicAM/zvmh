#ifndef AGENT_H
#define AGENT_H

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "provider/provider.h"
#include "message/message.h"
#include "tool/registry.h"
#include "system/registry.h"
#include "server/client.h"
#include "tool/msg.h"
#include "tool/peers.h"

class StreamSink {
public:
    virtual ~StreamSink() = default;
    virtual void header(const std::string& provider, const std::string& model) {}
    virtual void text_delta(const std::string& text) {}
    virtual void tool_start(const std::string& name) {}
    virtual void summary(const std::string& model, int prompt_tokens, int completion_tokens) {}
    virtual void tool_call(const std::string& name, const nlohmann::json& args) {}
    virtual void tool_result(const std::string& result, bool is_error) {}
    virtual void warning(const std::string& text) {}
};

class Agent {
private:
    std::unique_ptr<Provider> provider_;
    std::vector<Message> messages_;
    std::string system_prompt_;
    Registry registry_;
    sysctx::SystemContextRegistry sys_context_registry_;
    std::vector<sysctx::RegistrationHandle> sys_context_handles_;
    sysctx::Snapshot sys_context_snapshot_;
    std::string sys_context_text_;
    bool sys_context_initialized_ = false;

    std::string build_system_prompt(StreamSink& sink);

    std::unique_ptr<swarm::ServerClient> swarm_;
    std::deque<std::string> swarm_inbox_;
    std::mutex swarm_inbox_mu_;
    std::function<void(const std::string&)> swarm_realtime_;

    std::string drain_swarm_text();

public:
    explicit Agent(std::unique_ptr<Provider> provider);

    int run_once(const std::string& prompt);
    int run_turn(const std::string& prompt, StreamSink& sink);
    int run_tui();

    // Takes ownership of a connected ServerClient; registers the msg/peers tools
    // and starts forwarding inbound swarm traffic through the reader thread.
    void attach_swarm(std::unique_ptr<swarm::ServerClient> client);
    // Swap the callback invoked (on the reader thread) for each inbound swarm
    // line; the UI uses this for live rendering, so it must be thread-safe.
    void set_swarm_realtime(std::function<void(const std::string&)> realtime);
    bool swarm_connected() const { return swarm_ && swarm_->connected(); }
    swarm::ServerClient* swarm() const { return swarm_.get(); }

    std::string provider_name() const { return provider_->name(); }
    std::string model() const { return provider_->model(); }
    void set_model(const std::string& model) { provider_->set_model(model); }
    TokenUsage usage() const { return provider_->last_usage(); }
    int context_window() { return provider_->context_window(provider_->model()); }
    std::vector<ToolDefinition> tools() const { return registry_.definitions(); }
    Tool* tool(const std::string& name) const { return registry_.get(name); }
    void clear_messages() { messages_.clear(); }

    void add_message(const Message& message);
    void set_system_prompt(const std::string& prompt);
};

#endif