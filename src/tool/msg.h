#ifndef TOOL_MSG_H
#define TOOL_MSG_H

#include "tool.h"
#include "../server/client.h"

// Send a message to a peer agent (or every agent) over the swarm server.
class MsgTool : public Tool {
public:
    explicit MsgTool(swarm::ServerClient* client) : client_(client) {}

    const char* name() const override { return "msg"; }
    const char* description() const override {
        return R"(Send a chat message to a peer agent through the swarm server and return the send result.
The swarm allows multiple agents to collaborate on the same repository. Messages are delivered
promptly to the target(s) on their next turn.

WHEN TO USE
- asking a peer agent to coordinate on a task (who does what)
- sharing an intermediate result, filename, or decision with another agent
- handing work off because a peer already owns the relevant file

WHEN NOT TO USE
- peer has not responded and you need fresh state: use peers first to confirm they are connected
- the message is a command for a peer to run: describe the request in prose; peers interpret it

DO NOT USE FOR
- changing files that another agent is editing: use read to re-check the file instead
- talking to non-agents: the server only delivers to connected zvmh instances

USAGE
- to: exactly "all" (every agent), "repo" (every agent in the same repository), or an agent id
  from the peers tool; unknown ids are dropped silently by the server
- text: plain-text message; multi-line is fine, keep it short enough for a peer's context
- returns the outcome; a peer reply arrives on a later turn as a "[swarm]" notice

EXAMPLES
- {"to": "repo", "text": "Please leave src/main.cpp alone; I am refactoring it."}
- {"to": "all", "text": "Demo time in 5 minutes; freeze edits."}
- {"to": "a3f9", "text": "What hash did you see for CMakeLists.txt?"})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"to", {
                    {"type", "string"},
                    {"description", "\"all\", \"repo\", or an agent id from the peers tool"}
                }},
                {"text", {
                    {"type", "string"},
                    {"description", "Message text to deliver"}
                }}
            }},
            {"required", nlohmann::json::array({"to", "text"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        if (!client_ || !client_->connected()) {
            return "Error: not connected to a swarm server (start one with `zvmh --server start`, connect with `--connect`)";
        }
        std::string to = input["to"].get<std::string>();
        std::string text = input["text"].get<std::string>();
        if (to != "all" && to != "repo" && !client_->knows_peer(to)) {
            return "Error: no peer with id '" + to + "' is connected; use peers to list agents";
        }
        if (!client_->send_message(to, text)) {
            return "Error: failed to send message to swarm server";
        }
        return "Message sent to " + to;
    }

private:
    swarm::ServerClient* client_;
};

#endif