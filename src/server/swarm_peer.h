#ifndef SWARM_PEER_H
#define SWARM_PEER_H

#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// The surface an Agent uses to participate in a swarm. Two implementations:
//
//  - ServerClient (server/client.h): a real NDJSON-over-TCP connection to a
//    SwarmsServer daemon.
//  - SwarmsServer::PeerView (server/server.h): hosted fully in-process inside
//    the daemon that runs the agent's own turn loop. register_read/report_write
//    drive the conflict machinery directly instead of crossing a socket.
//
// The agent's tool loop only needs this interface, so the same Agent runtime
// works whether the swarm is local to the process or remote.

namespace swarm {

class SwarmPeer {
public:
    using MessageHandler = std::function<void(const std::string& type, const nlohmann::json& msg)>;

    virtual ~SwarmPeer() = default;

    virtual bool connected() const = 0;
    virtual void set_handler(MessageHandler handler) = 0;
    virtual bool knows_peer(const std::string& id) const = 0;
    virtual bool send_message(const std::string& to, const std::string& text) = 0;
    virtual bool request_peers() = 0;
    virtual std::vector<nlohmann::json> peers() const = 0;
    virtual bool register_read(const std::string& path) = 0;
    virtual bool report_write(const std::string& path) = 0;
    virtual std::string id() const = 0;
};

}  // namespace swarm

#endif