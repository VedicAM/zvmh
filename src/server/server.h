#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "protocol.h"

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

// The swarm daemon. Binds a listening socket on host:port, runs one detached
// reader thread per client, a background poller thread, and a main accept loop
// that yields on SIGTERM/SIGINT (so `--server stop` = send SIGTERM). Conflict
// detection is metadata-only: per-path read-hashes are registered by `read`
// messages and fanned out as `conflict` on `write` or on-disk change.
class SwarmsServer {
public:
    SwarmsServer(const std::string& host, int port, int poll_ms)
        : host_(host), port_(port), poll_ms_(poll_ms) {}

    int run();

    std::string host() const { return host_; }
    int port() const { return port_; }

private:
    struct Client {
        std::string id;
        std::string name;
        std::string repo;
        int fd = -1;
    };

    int listen_socket();
    void handle_client(int fd);
    void poller_loop();

    // Assumes mu_ is NOT held. Mutates conflict/read state, then sends.
    void notify_file_changed(const std::string& path, const std::string& new_hash,
                             const std::string& by, const std::string& at);
    // Assumes mu_ IS held.
    void update_conflict_state_locked(const std::string& path, const std::string& new_hash,
                                      const std::string& by, const std::string& at,
                                      std::vector<std::string>& notify_out);
    void refresh_watch_locked(const std::string& path);
    void remove_client_locked(const std::string& id);
    void send_to(const std::string& id, const nlohmann::json& msg);
    void shutdown_clients();

    std::string host_;
    int port_;
    int poll_ms_;

    std::mutex mu_;        // protects all maps below
    std::mutex send_mu_;   // serializes every socket write (one global lock; swarm traffic is tiny)
    std::map<std::string, Client> clients_;             // id -> connected client
    std::map<std::string, std::vector<std::string>> repo_members_;   // repo -> ids
    std::map<std::string, std::map<std::string, std::string>> read_hashes_;  // path -> id -> hash
    std::map<std::string, WatchedFile> file_state_;     // path -> poller baseline
    std::atomic<bool> closing_{false};
    std::thread poller_;
};

}  // namespace swarm

#endif