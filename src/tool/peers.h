#ifndef TOOL_PEERS_H
#define TOOL_PEERS_H

#include "tool.h"
#include "../server/client.h"
#include <chrono>
#include <thread>

// List the other agents currently connected to the swarm server.
class PeersTool : public Tool {
public:
    explicit PeersTool(swarm::ServerClient* client) : client_(client) {}

    const char* name() const override { return "peers"; }
    const char* description() const override {
        return R"(List the other agents connected to the swarm server (id, name, repository) and return them as text.
The id is what the msg tool accepts as its "to" target.

WHEN TO USE
- before sending a msg to an agent so you have a valid id
- confirming which agents share your repository versus the wider swarm

WHEN NOT TO USE
- you already know the target id: just msg it
- no swarm is running: check connect status or start `zvmh --server start`

DO NOT USE FOR
- checking whether a local file changed: use read to compare hashes
- enumerating files or machines: this only lists connected zvmh agents

USAGE
- returns one line per peer: 'id name (repo)'; your own id is included
- the list is refreshed on connect and on each agents_list response; ask again to re-query
- an empty list means you are alone on the server

EXAMPLES
- {}
- {"refresh": true})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"refresh", {
                    {"type", "boolean"},
                    {"description", "Re-query the server before returning (default false)"}
                }}
            }},
            {"required", nlohmann::json::array()}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        if (!client_ || !client_->connected()) {
            return "Error: not connected to a swarm server (start one with `zvmh --server start`, connect with `--connect`)";
        }
        bool refresh = input.value("refresh", false);
        if (refresh) {
            client_->request_peers();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::vector<nlohmann::json> peers = client_->peers();
        if (peers.empty()) {
            return "No peers connected (you are alone on the server)";
        }
        std::string out = "Connected agents:\n";
        for (auto& p : peers) {
            out += "  " + p.value("id", "?") + "  " + p.value("name", "?") +
                   "  (" + p.value("repo", "?") + ")\n";
        }
        out.pop_back();
        return out;
    }

private:
    swarm::ServerClient* client_;
};

#endif