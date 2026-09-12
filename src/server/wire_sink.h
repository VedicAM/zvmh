#ifndef WIRE_SINK_H
#define WIRE_SINK_H

#include <functional>
#include <string>
#include <nlohmann/json.hpp>
#include "../agent.h"

// StreamSink that serializes every callback into a `stream` wire frame and
// forwards it through a caller-supplied sender (the server writes to the
// owning client's socket). Also emits the `ctx` and `turn_done` bookends of a
// server-hosted turn.
//
// A failed send permanently flips `alive_` and subsequent callbacks become
// no-ops, so the turn loop unwinds naturally when the client disconnects
// mid-turn instead of throwing.
class WireSink : public StreamSink {
public:
    explicit WireSink(std::function<bool(const nlohmann::json&)> send)
        : send_(std::move(send)) {}

    bool alive() const { return alive_; }

    void header(const std::string& provider, const std::string& model) override {
        emit({{"event", "header"}, {"provider", provider}, {"model", model}});
    }
    void text_delta(const std::string& text) override {
        emit({{"event", "text_delta"}, {"text", text}});
    }
    void tool_start(const std::string& name) override {
        emit({{"event", "tool_start"}, {"name", name}});
    }
    void tool_call(const std::string& name, const nlohmann::json& args) override {
        emit({{"event", "tool_call"}, {"name", name}, {"args", args}});
    }
    void tool_result(const std::string& result, bool is_error) override {
        emit({{"event", "tool_result"}, {"text", result}, {"is_error", is_error}});
    }
    void summary(const std::string& model, int prompt_tokens, int completion_tokens) override {
        emit({{"event", "summary"}, {"model", model},
              {"prompt", prompt_tokens}, {"completion", completion_tokens}});
    }
    void warning(const std::string& text) override {
        emit({{"event", "warning"}, {"text", text}});
    }

    void ctx(int used, int total) {
        emit({{"type", "ctx"}, {"used", used}, {"total", total}});
    }

    void done(int rc) {
        emit({{"type", "turn_done"}, {"rc", rc}});
    }

private:
    void emit(const nlohmann::json& fields) {
        if (!alive_) return;
        nlohmann::json frame = {{"type", "stream"}};
        frame.update(fields);
        if (!send_(frame)) alive_ = false;
    }

    std::function<bool(const nlohmann::json&)> send_;
    bool alive_ = true;
};

#endif