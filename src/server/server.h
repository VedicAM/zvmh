#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "protocol.h"
#include "../agent.h"

namespace swarm {

// Poller baseline for the on-disk watch of a single file. The poller is the
// safety net for changes that never route through a `write` message (bash-side
// overwrites, human edits): it stats watched paths every poll_ms_ and only
// re-hashes when mtime or size moved.
struct WatchedFile {
    bool watched = false;
    bool exists = false;
    uint64_t mtime = 0;
    uint64_t size = 0;
    std::string hash;
};

// The swarm daemon — and, since the agent runtime moved server-side, the host
// for every connected client's Agent. Binds a listening socket on host:port,
// runs one detached reader thread per client, a background poller thread, and
// a main accept loop that yields on SIGTERM/SIGINT (so `--server stop` = send
// SIGTERM). Each hello creates an Agent slot (provider + registry + history +
// system context) whose turns run on detached threads, streamed back to the
// client through a WireSink.
//
// `solo` servers (session-provisioned on kSessionPort) self-terminate when no
// client is connected and no turn is running, so a hard-killed session leaves
// no permanent daemon.
class SwarmsServer {
public:
    SwarmsServer(const std::string& host, int port, int poll_ms,
                 const std::string& api_key, bool solo)
        : host_(host), port_(port), poll_ms_(poll_ms),
          api_key_(api_key), solo_(solo) {}

    int run();

    std::string host() const { return host_; }
    int port() const { return port_; }

    // In-process swarm view handed to a hosted Agent so msg/peers/conflict are
    // serviced directly from the daemon's own state. Implements SwarmPeer, so
    // the agent's tool loop cannot tell it apart from a real socket client.
    // inject() simulates an inbound frame (called by the daemon when another
    // client messages/writes this agent).
    class PeerView : public SwarmPeer {
    public:
        PeerView(SwarmsServer* owner, std::string client_id);

        bool connected() const override { return true; }
        void set_handler(MessageHandler handler) override;
        bool knows_peer(const std::string& id) const override;
        bool send_message(const std::string& to, const std::string& text) override;
        bool request_peers() override;
        std::vector<nlohmann::json> peers() const override;
        bool register_read(const std::string& path) override;
        bool report_write(const std::string& path) override;
        std::string id() const override { return client_id_; }

        void inject(const std::string& type, const nlohmann::json& msg);

    private:
        SwarmsServer* owner_;
        std::string client_id_;
        mutable std::mutex mu_;
        MessageHandler handler_;
    };

private:
    struct AgentSlot {
        std::unique_ptr<Agent> agent;
        bool busy = false;             // always accessed under mu_
        PeerView* peer_view = nullptr; // in-process swarm view; owned via agent
    };

    struct Client {
        std::string id;
        std::string name;
        std::string repo;
        int fd = -1;
    };

    int listen_socket();
    void handle_client(int fd);
    void poller_loop();

    // Wrappers around the mu_-locked state and per-agent slots. A turn may
    // touch an Agent slot only while that slot's busy flag is set, which
    // serializes turn-thread access against clients_/agents_ bookkeeping.
    void hello_register(const std::string& id, const std::string& name,
                        const std::string& repo, int fd,
                        std::vector<nlohmann::json>& peers);
    void create_agent_slot(const std::string& id, const std::string& repo);
    void send_state(const std::string& id);
    void schedule_prompt(const std::string& id, const std::string& text);
    void clear_agent_history(const std::string& id);
    void set_agent_model(const std::string& id, const std::string& model);
    void deliver_inbox(const std::string& id, const std::string& type,
                       const nlohmann::json& msg);
    void route_message(const std::string& from_id, const std::string& to,
                       const std::string& text);
    void server_register_read(const std::string& id, const std::string& path);
    void server_report_write(const std::string& id, const std::string& path);
    void maybe_shutdown_if_idle();

    // Assumes mu_ is NOT held. Mutates conflict/read state, then delivers.
    void notify_file_changed(const std::string& path, const std::string& new_hash,
                             const std::string& by, const std::string& at);
    // Assumes mu_ IS held.
    void update_conflict_state_locked(const std::string& path, const std::string& new_hash,
                                      const std::string& by, const std::string& at,
                                      std::vector<std::string>& notify_out);
    void refresh_watch_locked(const std::string& path);
    void remove_client_locked(const std::string& id);
    bool send_to(const std::string& id, const nlohmann::json& msg);
    void shutdown_clients();

    std::string host_;
    int port_;
    int poll_ms_;
    std::string api_key_;
    bool solo_ = false;

    std::mutex mu_;        // protects all maps below
    std::mutex send_mu_;   // serializes every socket write (one global lock; swarm traffic is tiny)
    std::map<std::string, Client> clients_;             // id -> connected client
    std::map<std::string, std::vector<std::string>> repo_members_;   // repo -> ids
    std::map<std::string, std::map<std::string, std::string>> read_hashes_;  // path -> id -> hash
    std::map<std::string, WatchedFile> file_state_;     // path -> poller baseline
    std::map<std::string, AgentSlot> agents_;           // id -> hosted agent runtime
    std::atomic<bool> closing_{false};
    std::thread poller_;
};

}  // namespace swarm

#endif