#include "agent.h"
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include "tui/tui.h"

struct ActiveToolCall {
    std::string id;
    std::string name;
    std::string input_json;
    nlohmann::json input;
};

namespace {
class StdoutSink : public StreamSink {
public:
    void header(const std::string& provider, const std::string& model) override {
        std::cout << "\n  \033[2m[" << provider << " · " << model << "]\033[0m\n";
    }
    void text_delta(const std::string& text) override {
        std::cout << text << std::flush;
    }
    void tool_start(const std::string& name) override {
        std::cout << "\n\n  \033[36m→ \033[1m" << name << "\033[0m\n";
    }
    void summary(const std::string& model, int prompt_tokens, int completion_tokens) override {
        std::cout << "  \033[2m" << std::string(52, '-') << "\033[0m\n"
                  << "  \033[2mmodel: \033[0m" << model
                  << "  \033[2m| tokens in: \033[0m" << prompt_tokens
                  << "  \033[2m| out: \033[0m" << completion_tokens << "\n";
    }
    void tool_call(const std::string& name, const nlohmann::json& args) override {
        std::cout << "  \033[33m▸ \033[1m" << name << "\033[0m " << args.dump() << "\n";
    }
    void tool_result(const std::string& result, bool is_error) override {
        if (is_error) {
            std::cerr << "  \033[31m" << result << "\033[0m\n";
            return;
        }
        std::string display = result;
        if (display.size() > 220) {
            display = display.substr(0, 220) + "...\n";
        } else if (!display.empty() && display.back() != '\n') {
            display += "\n";
        }
        std::cout << "  \033[32m↩ \033[0m" << display << "\n";
    }
    void warning(const std::string& text) override {
        std::cerr << "  \033[31m" << text << "\033[0m\n";
    }
};
}  // namespace

Agent::Agent(std::unique_ptr<Provider> provider)
    : provider_(std::move(provider)), system_prompt_("You are a helpful assistant.") {
    register_builtin_tools(registry_);
}

void Agent::set_system_prompt(const std::string& prompt) {
    system_prompt_ = prompt;
}

void Agent::add_message(const Message& message) {
    messages_.push_back(message);
}

int Agent::run_once(const std::string& prompt) {
    StdoutSink sink;
    return run_turn(prompt, sink);
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

    Message user_msg;
    user_msg.role = Role::User;
    user_msg.content.push_back(TextBlock{prompt});
    messages_.push_back(user_msg);

    const int max_steps = 10;

    try {
        for (int step = 0; step < max_steps; ++step) {
            sink.header(provider_->name(), provider_->model());

            std::string response_text;
            std::vector<ActiveToolCall> tool_calls;
            std::map<int, int> tool_index;  // api delta index -> tool_calls position

            TurnBridge bridge(sink, response_text, tool_calls, tool_index);
            provider_->complete(messages_, registry_.definitions(), system_prompt_, bridge);

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
                sink.tool_call(tc.name, tc.input);

                std::string result;
                bool is_error = false;
                try {
                    Tool* tool = registry_.get(tc.name);
                    if (!tool) {
                        result = "Error: Unknown tool: " + tc.name;
                        is_error = true;
                    } else {
                        result = tool->execute(tc.input);
                    }
                } catch (const std::exception& e) {
                    result = std::string("Error: ") + e.what();
                    is_error = true;
                }
                sink.tool_result(result, is_error);

                Message tool_msg;
                tool_msg.role = Role::User;
                tool_msg.content.push_back(ToolResultBlock{tc.id, result});
                messages_.push_back(tool_msg);
            }
        }

        sink.warning("Reached maximum tool steps (" + std::to_string(max_steps) + ")");
        return 1;
    } catch (const std::exception& e) {
        sink.warning(std::string("Error: ") + e.what());
        if (messages_.size() > history_start) {
            messages_.resize(history_start);
        }
        return 1;
    }
}

int Agent::run_tui() {
    Tui tui(*this);
    return tui.run();
}