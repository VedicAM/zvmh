#include "agent.h"
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include "tui/tui.h"
#include "system/builtins.h"

struct ActiveToolCall {
    std::string id;
    std::string name;
    std::string input_json;
    nlohmann::json input;
};

namespace {

// Collapse runs of whitespace (including newlines) into single spaces so a
// whole tool result fits on one display row.
std::string collapse_ws(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prev_space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            if (!prev_space) out.push_back(' ');
            prev_space = true;
        } else {
            out.push_back(c);
            prev_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string clamp_front(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max) + "…";
}

std::string clamp_tail(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    return "…" + s.substr(s.size() - max);
}

// Primary user-facing argument for the tool-call decoration ("read [file]").
std::string tool_argument(const nlohmann::json& args) {
    static const char* keys[] = {"file_path", "command", "pattern", "path"};
    for (const char* key : keys) {
        auto it = args.find(key);
        if (it != args.end() && it->is_string()) {
            std::string v = it->get<std::string>();
            if (!v.empty()) return clamp_front(collapse_ws(v), 64);
        }
    }
    return "";
}

// Outcome preview shown instead of the full tool result. The model still gets
// the complete result in its history; only the user-facing sink sees this
// compact one-liner, keyed off the tool name.
std::string make_tool_preview(const std::string& name,
                              const std::string& result,
                              bool is_error) {
    if (is_error)
        return clamp_front(collapse_ws(result), 220);

    if (name == "read") {
        size_t lines = 0;
        for (char c : result)
            if (c == '\n') ++lines;
        if (!result.empty() && result.back() != '\n') ++lines;
        return std::to_string(lines) + " lines";
    }

    if (name == "grep") {
        size_t matches = 0;
        for (char c : result)
            if (c == '\n') ++matches;
        if (matches == 0)
            return "no matches";
        return std::to_string(matches) +
            (matches == 1 ? " match" : " matches");
    }

    if (name == "bash") {
        std::string out = collapse_ws(result);
        if (out.empty()) return "ok";
        return clamp_tail(out, 160);
    }

    return clamp_front(collapse_ws(result), 160);
}

class StdoutSink : public StreamSink {
public:
    void header(const std::string& provider, const std::string& model) override {
        std::cout << "\n  \033[2m[" << provider << " · " << model << "]\033[0m\n";
        line_open_ = false;
    }
    void text_delta(const std::string& text) override {
        std::cout << text << std::flush;
        line_open_ = !text.empty() && text.back() != '\n';
    }
    void tool_start(const std::string&) override {
        // Folded into tool_call: one line per invocation with the bundled
        // argument, result preview appended to the same line.
    }
    void tool_call(const std::string& name, const nlohmann::json& args) override {
        if (line_open_)
            std::cout << "\n";
        std::string display = "  \033[33m▸ \033[1m" + name + "\033[0m";
        std::string arg = tool_argument(args);
        if (!arg.empty())
            display += " \033[2m[" + arg + "]\033[0m";
        std::cout << display << std::flush;
        line_open_ = true;
    }
    void tool_result(const std::string& preview, bool is_error) override {
        std::string color = is_error ? "\033[31m" : "\033[90m";
        std::cout << " \033[2m·\033[0m " << color << preview << "\033[0m\n" << std::flush;
        line_open_ = false;
    }
    void summary(const std::string& model, int prompt_tokens, int completion_tokens) override {
        std::cout << "  \033[2m" << std::string(52, '-') << "\033[0m\n"
                  << "  \033[2mmodel: \033[0m" << model
                  << "  \033[2m| tokens in: \033[0m" << prompt_tokens
                  << "  \033[2m| out: \033[0m" << completion_tokens << "\n";
        line_open_ = false;
    }
    void warning(const std::string& text) override {
        std::cerr << "  \033[31m" << text << "\033[0m\n";
        line_open_ = false;
    }

private:
    // True while the last emitted bytes sit mid-row without a trailing \n,
    // so the next tool line can start on a fresh row without inserting a
    // blank one between consecutive tool calls.
    bool line_open_ = false;
};
}  // namespace

Agent::Agent(std::unique_ptr<Provider> provider)
: provider_(std::move(provider)), system_prompt_("You are an advanced AI coding assistant.\n- Write production-quality code\n- Consider edge cases\n- Optimize for readability\n- Document your approach") {
    register_builtin_tools(registry_);
    sys_context_handles_ = register_system_context_builtins(sys_context_registry_);
}

void Agent::set_system_prompt(const std::string& prompt) {
    system_prompt_ = prompt;
}

void Agent::add_message(const Message& message) {
    messages_.push_back(message);
}

std::string Agent::build_system_prompt(StreamSink& sink) {
    std::string system = system_prompt_;
    sysctx::SystemContext context = sys_context_registry_.load();

    if (!sys_context_initialized_) {
        auto result = sysctx::initialize(context);
        if (auto* gen = std::get_if<sysctx::Generation>(&result)) {
            sys_context_text_ = gen->baseline;
            sys_context_snapshot_ = gen->snapshot;
            sys_context_initialized_ = true;
            if (!gen->baseline.empty()) system += "\n\n" + gen->baseline;
        } else if (const auto* blocked = std::get_if<sysctx::InitializationBlocked>(&result)) {
            sink.warning(blocked->message());
        }
        return system;
    }

    sysctx::ReconcileResult reconciled = sysctx::reconcile(context, sys_context_snapshot_);
    switch (reconciled.tag) {
        case sysctx::ReconcileResult::Tag::Unchanged:
            if (!sys_context_text_.empty()) system += "\n\n" + sys_context_text_;
            break;
        case sysctx::ReconcileResult::Tag::Updated:
            sys_context_text_ = reconciled.text;
            sys_context_snapshot_ = reconciled.snapshot;
            if (!reconciled.text.empty()) system += "\n\n" + reconciled.text;
            break;
        case sysctx::ReconcileResult::Tag::ReplacementReady:
            if (reconciled.generation) {
                sys_context_text_ = reconciled.generation->baseline;
                sys_context_snapshot_ = reconciled.generation->snapshot;
                if (!reconciled.generation->baseline.empty()) {
                    system += "\n\n" + reconciled.generation->baseline;
                }
            }
            break;
        case sysctx::ReconcileResult::Tag::ReplacementBlocked:
            if (!sys_context_text_.empty()) system += "\n\n" + sys_context_text_;
            break;
        case sysctx::ReconcileResult::Tag::Replace:
            break;
    }
    return system;
}

int Agent::run_once(const std::string& prompt) {
    StdoutSink sink;
    return run_turn(prompt, sink);
}

void Agent::attach_swarm(std::unique_ptr<swarm::SwarmPeer> peer, bool with_tools) {
    if (!peer || !peer->connected()) {
        return;
    }
    swarm_ = std::move(peer);
    if (with_tools) {
        registry_.register_tool<MsgTool>(swarm_.get());
        registry_.register_tool<PeersTool>(swarm_.get());
    }
    swarm_->set_handler([this](const std::string& type, const nlohmann::json& msg) {
        std::string line;
        if (type == "msg") {
            line = "[swarm] <" + msg.value("from", "?") + "> " + msg.value("text", "");
        } else if (type == "conflict") {
            line = "[swarm] conflict: " + msg.value("path", "") + " was changed by " +
                   msg.value("by", "?") + " at " + msg.value("at", "?");
        } else if (type == "shutdown") {
            line = "[swarm] server went away; swarm disconnected";
        } else {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(swarm_inbox_mu_);
            if (swarm_realtime_) swarm_realtime_(line);
            if (swarm_inbox_.size() < 64) swarm_inbox_.push_back(line);
        }
    });
}

void Agent::set_swarm_realtime(std::function<void(const std::string&)> realtime) {
    std::lock_guard<std::mutex> lk(swarm_inbox_mu_);
    swarm_realtime_ = std::move(realtime);
}

void Agent::notify_swarm_line(const std::string& line) {
    std::lock_guard<std::mutex> lk(swarm_inbox_mu_);
    if (swarm_realtime_) swarm_realtime_(line);
}

void Agent::rewrite_tool_paths(nlohmann::json& input) const {
    if (tool_cwd_base_.empty()) return;

    auto fix = [&](const char* key) {
        auto it = input.find(key);
        if (it != input.end() && it->is_string()) {
            std::string v = it->get<std::string>();
            if (!v.empty() && v[0] != '/') {
                *it = tool_cwd_base_ + "/" + v;
            }
        }
    };

    fix("file_path");
    fix("path");
    fix("pattern");

    auto it = input.find("workdir");
    if (it == input.end() || it->is_null()) {
        input["workdir"] = tool_cwd_base_;
    } else {
        fix("workdir");
    }
}

std::string Agent::drain_swarm_text() {
    std::deque<std::string> items;
    {
        std::lock_guard<std::mutex> lk(swarm_inbox_mu_);
        items.swap(swarm_inbox_);
    }
    std::string block;
    for (auto& line : items) {
        block += line + "\n";
    }
    while (!block.empty() && block.back() == '\n') block.pop_back();
    return block;
}

class TurnBridge : public EventSink {
public:
    TurnBridge(
        StreamSink& sink,
        std::string& response_text,
        std::vector<ActiveToolCall>& tool_calls,
        std::map<int, int>& tool_index
    )
        : sink_(sink), text_(response_text), calls_(tool_calls), index_(tool_index) {}

    void on_event(const StreamEvent& event) override {
        if (auto* text_delta = std::get_if<TextDeltaEvent>(&event)) {
            text_ += text_delta->text;
            sink_.text_delta(text_delta->text);
        } else if (auto* tool_start = std::get_if<ToolUseStartEvent>(&event)) {
            calls_.push_back({tool_start->id, tool_start->name, "", nlohmann::json::object()});
            index_[tool_start->index] = static_cast<int>(calls_.size()) - 1;
            sink_.tool_start(tool_start->name);
        } else if (auto* tool_delta = std::get_if<ToolInputDeltaEvent>(&event)) {
            auto it = index_.find(tool_delta->index);
            if (it != index_.end()) {
                calls_[static_cast<size_t>(it->second)].input_json += tool_delta->input;
            }
        }
    }

private:
    StreamSink& sink_;
    std::string& text_;
    std::vector<ActiveToolCall>& calls_;
    std::map<int, int>& index_;
};

int Agent::run_turn(const std::string& prompt, StreamSink& sink) {
    size_t history_start = messages_.size();

    // A fresh turn starts uncancelled; the previous turn's Cancelled condition
    // must not leak into this one.
    cancel_requested_.store(false);
    if (provider_) provider_->clear_cancel();

    std::string effective_prompt = prompt;
    std::string swarm_block = drain_swarm_text();
    if (!swarm_block.empty()) {
        effective_prompt =
            "[swarm notifications received before this turn]\n" + swarm_block +
            "\n\nIf a file you previously read or edited was reported changed, re-read it and "
            "reconcile your plan before acting. Handle these, then: " +
            prompt;
    }

    Message user_msg;
    user_msg.role = Role::User;
    user_msg.content.push_back(TextBlock{effective_prompt});
    messages_.push_back(user_msg);

    const int max_steps = 10;

    try {
        std::string system_prompt = build_system_prompt(sink);

        for (int step = 0; step < max_steps; ++step) {
            std::string response_text;
            std::vector<ActiveToolCall> tool_calls;
            std::map<int, int> tool_index;  // api delta index -> tool_calls position

            TurnBridge bridge(sink, response_text, tool_calls, tool_index);
            provider_->complete(messages_, registry_.definitions(), system_prompt, bridge);

            if (cancel_requested_) {
                // Aborted mid-stream: stop now; nothing from this step is kept.
                break;
            }

            Message assistant_msg;
            assistant_msg.role = Role::Assistant;
            if (!response_text.empty()) {
                assistant_msg.content.push_back(TextBlock{response_text});
            }

            bool had_tools = false;
            for (auto& tc : tool_calls) {
                try {
                    if (!tc.input_json.empty()) {
                        tc.input = nlohmann::json::parse(tc.input_json);
                    }
                } catch (...) {
                    tc.input = nlohmann::json::object();
                }
                assistant_msg.content.push_back(ToolUseBlock{tc.id, tc.name, tc.input});
                had_tools = true;
            }
            messages_.push_back(assistant_msg);

            TokenUsage usage = provider_->last_usage();
            sink.summary(provider_->model(), usage.prompt_tokens, usage.completion_tokens);

            if (!had_tools) {
                return 0;
            }

            for (const auto& tc : tool_calls) {
                if (cancel_requested_) break;
                sink.tool_call(tc.name, tc.input);

                std::string result;
                bool is_error = false;
                nlohmann::json input;
                try {
                    Tool* tool = registry_.get(tc.name);
                    if (!tool) {
                        result = "Error: Unknown tool: " + tc.name;
                        is_error = true;
                    } else {
                        input = tc.input;
                        rewrite_tool_paths(input);
                        result = tool->execute(input);
                    }
                } catch (const std::exception& e) {
                    result = std::string("Error: ") + e.what();
                    is_error = true;
                }
                if (!is_error && swarm_ && swarm_->connected() && input.contains("file_path")) {
                    std::string fp = input["file_path"].get<std::string>();
                    if (tc.name == "read") {
                        swarm_->register_read(fp);
                    } else if (tc.name == "write" || tc.name == "edit") {
                        swarm_->report_write(fp);
                    }
                }
                sink.tool_result(make_tool_preview(tc.name, result, is_error), is_error);

                Message tool_msg;
                tool_msg.role = Role::User;
                tool_msg.content.push_back(ToolResultBlock{tc.id, result});
                messages_.push_back(tool_msg);
            }
        }

        if (cancel_requested_) {
            // Roll the partial history back so a cancelled turn leaves no
            // orphaned assistant tool-calls without matching results.
            if (messages_.size() > history_start) {
                messages_.resize(history_start);
            }
            return kCancelled;
        }

        sink.warning("Reached maximum tool steps (" + std::to_string(max_steps) + ")");
        return 1;
    } catch (const std::exception& e) {
        if (!cancel_requested_) {
            sink.warning(std::string("Error: ") + e.what());
        }
        if (messages_.size() > history_start) {
            messages_.resize(history_start);
        }
        return cancel_requested_ ? kCancelled : 1;
    }
}

int Agent::run_tui() {
    Tui tui(*this);
    return tui.run();
}