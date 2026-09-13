#ifndef AGENT_H
#define AGENT_H

#include <atomic>
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
#include "server/swarm_peer.h"
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

    // When non-empty, relative tool paths (file_path/path/pattern/workdir) are
    // resolved against this base before execution. The server-hosted runtime
    // sets it to the client-declared repo so agents on one daemon operate on
    // their own workspaces even though threads cannot chdir per-agent.
    std::string tool_cwd_base_;
    void rewrite_tool_paths(nlohmann::json& input) const;

    std::unique_ptr<swarm::SwarmPeer> swarm_;
    std::deque<std::string> swarm_inbox_;
    std::mutex swarm_inbox_mu_;
    std::function<void(const std::string&)> swarm_realtime_;

    // Abort signal for the running turn: set by cancel_turn() from any thread,
    // consulted by the tool loop at every step boundary and by the provider
    // (via Provider::*_cancel) mid-request.
    std::atomic<bool> cancel_requested_{false};

    std::string drain_swarm_text();

public:
    // run_turn() result code for a cancelled turn (nothing was executed and
    // the partial history was rolled back).
    static constexpr int kCancelled = 2;

    virtual ~Agent() = default;
    explicit Agent(std::unique_ptr<Provider> provider = nullptr);

    virtual int run_once(const std::string& prompt);
    virtual int run_turn(const std::string& prompt, StreamSink& sink);
    virtual int run_tui();

    // Takes ownership of a connected swarm peer; registers the msg/peers tools
    // when with_tools is true (server-hosted runtime) and consumes inbound
    // traffic. The client-side RemoteAgent overrides this to a no-op.
    virtual void attach_swarm(std::unique_ptr<swarm::SwarmPeer> peer, bool with_tools = false);
    // Swap the callback invoked (reader thread) for each inbound swarm line;
    // the UI uses this for live rendering, so it must be thread-safe.
    virtual void set_swarm_realtime(std::function<void(const std::string&)> realtime);
    virtual bool swarm_connected() const { return swarm_ && swarm_->connected(); }
    virtual swarm::SwarmPeer* swarm() const { return swarm_.get(); }

    virtual std::string provider_name() const { return provider_ ? provider_->name() : ""; }
    virtual std::string model() const { return provider_ ? provider_->model() : ""; }
    virtual void set_model(const std::string& model) {
        if (provider_) provider_->set_model(model);
    }
    virtual TokenUsage usage() const { return provider_ ? provider_->last_usage() : TokenUsage(); }
    virtual int context_window() {
        return provider_ ? provider_->context_window(provider_->model()) : -1;
    }
    virtual std::vector<ToolDefinition> tools() const { return registry_.definitions(); }
    virtual Tool* tool(const std::string& name) const { return registry_.get(name); }
    virtual void clear_messages() { messages_.clear(); }

    bool has_provider() const { return provider_ != nullptr; }
    void set_tool_cwd_base(const std::string& base) { tool_cwd_base_ = base; }

    // Abort the running turn (thread-safe). The base implementation signals the
    // in-process runtime and aborts the provider's in-flight request.
    virtual void cancel_turn() {
        cancel_requested_.store(true);
        if (provider_) provider_->request_cancel();
    }
    bool cancel_requested() const { return cancel_requested_.load(); }

    void add_message(const Message& message);
    void set_system_prompt(const std::string& prompt);

protected:
    // Called by the swarm inbound path (message/conflict/shutdown notices).
    // ServerClient's reader thread and RemoteAgent's handler both funnel into
    // this so the live-render callback stays single-threaded behind the mutex.
    void notify_swarm_line(const std::string& line);
};

#endif