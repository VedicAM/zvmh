#ifndef SERVER_CLIENT_H
#define SERVER_CLIENT_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "protocol.h"

namespace swarm {

// Client side of the swarm protocol. Owns one TCP connection to a SwarmsServer:
// sends hello on connect, runs a background reader thread, and exposes the
// actions the agent/tools need (register_read, report_write, send_message,
// request_peers). Inbound traffic after hello_ack is forwarded to a caller-set
// handler (called from the reader thread) and peer snapshots are kept current.
class ServerClient {
public:
    using MessageHandler = std::function<void(const std::string& type, const nlohmann::json& msg)>;

    ServerClient(const std::string& host, int port) : host_(host), port_(port) {}
    ~ServerClient();

    ServerClient(const ServerClient&) = delete;
    ServerClient& operator=(const ServerClient&) = delete;

    // TCP connect + hello exchange. Blocks until hello_ack (5s deadline).
    bool connect(const std::string& repo, const std::string& name);

    void set_handler(MessageHandler h) {
        std::lock_guard<std::mutex> lk(mu_);
        handler_ = std::move(h);
    }

    bool register_read(const std::string& path);
    bool report_write(const std::string& path);
    bool send_message(const std::string& to, const std::string& text);
    bool request_peers();
    bool knows_peer(const std::string& id) const {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : peers_) {
            if (p.value("id", "") == id) return true;
        }
        return false;
    }

    void disconnect();
    bool connected() const { return running_.load(); }
    std::string id() const { return id_; }
    std::string repo() const { return repo_; }
    std::string name() const { return name_; }
    std::vector<nlohmann::json> peers() const {
        std::lock_guard<std::mutex> lk(mu_);
        return peers_;
    }

    const std::string& host() const { return host_; }
    int port() const { return port_; }

private:
    void reader_loop();

    std::string host_;
    int port_;
    std::string repo_;
    std::string name_;
    std::string id_;
    int fd_ = -1;

    std::atomic<bool> running_{false};
    std::thread reader_;

    mutable std::mutex mu_;          // protects peers_ + handler_
    std::mutex send_mu_;             // serializes socket writes
    std::vector<nlohmann::json> peers_;
    MessageHandler handler_;
};

}  // namespace swarm

#endif