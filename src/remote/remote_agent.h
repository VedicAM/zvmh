#ifndef REMOTE_AGENT_H
#define REMOTE_AGENT_H

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "../agent.h"
#include "../server/client.h"
#include "../server/swarm_peer.h"

namespace remote {

class StubTool;

// Thin-client Agent facade. The real runtime (provider, tool loop, history,
// system context) lives on the server; this class is the client-side handle
// that mirrors it across the wire so the TUI and the -m CLI keep operating on
// `Agent&` unchanged.
//
// Threading: the ServerClient reader thread funnels every frame in here.
// `state`/`ctx`/`agents_list`/stream-summary populate the query caches; `stream`
// callbacks are forwarded to the active turn's StreamSink; `turn_done` releases
// the condition variable that run_turn waits on.
class RemoteAgent : public Agent {
public:
    RemoteAgent(const std::string& host, int port);
    ~RemoteAgent() override;

    // Connects to the server and awaits the hello_ack + state. Returns false on
    // any failure.
    bool connect(const std::string& repo, const std::string& name);

    // ---- Agent overrides (all server-backed) ----

    int run_turn(const std::string& prompt, StreamSink& sink) override;
    void attach_swarm(std::unique_ptr<swarm::SwarmPeer> peer, bool with_tools) override;
    void set_swarm_realtime(std::function<void(const std::string&)> realtime) override;

    std::string provider_name() const override;
    std::string model() const override;
    void set_model(const std::string& model) override;
    TokenUsage usage() const override;
    int context_window() override;
    std::vector<ToolDefinition> tools() const override;
    Tool* tool(const std::string& name) const override;
    void clear_messages() override;
    bool swarm_connected() const override;
    swarm::SwarmPeer* swarm() const override;
    std::string swarm_id() const { return client_->id(); }

    bool connected() const { return connected_.load(); }

    // ---- swarm surface the TUI uses (/agents, /msg) ----

    class RemotePeer : public swarm::SwarmPeer {
    public:
        explicit RemotePeer(RemoteAgent* owner) : owner_(owner) {}

        bool connected() const override { return owner_->swarm_connected(); }
        void set_handler(MessageHandler) override {}
        bool knows_peer(const std::string& id) const override;
        bool send_message(const std::string& to, const std::string& text) override;
        bool request_peers() override;
        std::vector<nlohmann::json> peers() const override;
        bool register_read(const std::string&) override { return true; }
        bool report_write(const std::string&) override { return true; }
        std::string id() const override { return owner_->swarm_id(); }

    private:
        RemoteAgent* owner_;
    };

private:
    void handle_frame(const std::string& type, const nlohmann::json& msg);
    void handle_stream_event(const nlohmann::json& msg, StreamSink& sink);
    bool knows_peer_id(const std::string& id) const;
    bool swarm_send_message(const std::string& to, const std::string& text);
    bool swarm_request_peers();

    std::function<void(const std::string&)> realtime_;
    std::unique_ptr<swarm::ServerClient> client_;
    std::unique_ptr<swarm::SwarmPeer> peer_;

    mutable std::mutex cache_mu_;      // query caches (provider, model, tools, peers)
    std::string cached_provider_;
    std::string cached_model_;
    std::vector<ToolDefinition> cached_tools_;
    mutable std::vector<std::unique_ptr<StubTool>> tool_stubs_;
    std::vector<nlohmann::json> cached_peers_;
    TokenUsage cached_usage_;
    int cached_ctx_total_ = -1;

    std::atomic<bool> connected_{false};

    // Turn rendezvous: run_turn waits, reader thread signals.
    std::mutex turn_mu_;
    std::condition_variable turn_cv_;
    bool turn_active_ = false;
    int turn_rc_ = 1;
    StreamSink* active_sink_ = nullptr;
};

}  // namespace remote

#endif