#include "agent.h"
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>

struct ActiveToolCall {
    std::string id;
    std::string name;
    std::string input_json;
    nlohmann::json input;
};

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

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
    return run_turn(prompt);
}

int Agent::run_turn(const std::string& prompt) {
    size_t history_start = messages_.size();

    Message user_msg;
    user_msg.role = Role::User;
    user_msg.content.push_back(TextBlock{prompt});
    messages_.push_back(user_msg);

    const int max_steps = 10;

    try {
        for (int step = 0; step < max_steps; ++step) {
            std::cout << "\n  \033[2m[" << provider_->name() << " · " << provider_->model() << "]\033[0m\n";

            EventStream events = provider_->complete(messages_, registry_.definitions(), system_prompt_);

            std::string response_text;
            std::vector<ActiveToolCall> tool_calls;
            std::map<int, int> tool_index;  // api delta index -> tool_calls position

            for (const auto& event : events) {
                if (auto* text_delta = std::get_if<TextDeltaEvent>(&event)) {
                    response_text += text_delta->text;
                    std::cout << text_delta->text << std::flush;
                } else if (auto* tool_start = std::get_if<ToolUseStartEvent>(&event)) {
                    tool_calls.push_back({tool_start->id, tool_start->name, "", nlohmann::json::object()});
                    tool_index[tool_start->index] = static_cast<int>(tool_calls.size()) - 1;
                    std::cout << "\n\n  \033[36m→ \033[1m" << tool_start->name << "\033[0m\n";
                } else if (auto* tool_delta = std::get_if<ToolInputDeltaEvent>(&event)) {
                    auto it = tool_index.find(tool_delta->index);
                    if (it != tool_index.end()) {
                        tool_calls[it->second].input_json += tool_delta->input;
                    }
                }
            }

            std::cout << "\n";

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
            std::cout << "  \033[2m" << std::string(52, '-') << "\033[0m\n";
            std::cout << "  \033[2mmodel: \033[0m" << provider_->model()
                      << "  \033[2m| tokens in: \033[0m" << usage.prompt_tokens
                      << "  \033[2m| out: \033[0m" << usage.completion_tokens << "\n";

            if (!had_tools) {
                return 0;
            }

            for (const auto& tc : tool_calls) {
                std::cout << "  \033[33m▸ \033[1m" << tc.name << "\033[0m "
                          << tc.input.dump() << "\n";

                std::string result;
                try {
                    Tool* tool = registry_.get(tc.name);
                    if (!tool) {
                        result = "Error: Unknown tool: " + tc.name;
                    } else {
                        result = tool->execute(tc.input);
                    }
                } catch (const std::exception& e) {
                    result = std::string("Error: ") + e.what();
                    std::cerr << "  \033[31m" << result << "\033[0m\n";
                }

                std::string display = result;
                if (display.size() > 220) {
                    display = display.substr(0, 220) + "...\n";
                } else if (!display.empty() && display.back() != '\n') {
                    display += "\n";
                }
                std::cout << "  \033[32m↩ \033[0m" << display << "\n";

                Message tool_msg;
                tool_msg.role = Role::User;
                tool_msg.content.push_back(ToolResultBlock{tc.id, result});
                messages_.push_back(tool_msg);
            }
        }

        std::cerr << "  \033[31mError:\033[0m Reached maximum tool steps (" << max_steps << ")\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "  \033[31mError:\033[0m " << e.what() << "\n";
        if (messages_.size() > history_start) {
            messages_.resize(history_start);
        }
        return 1;
    }
}

void Agent::handle_command(const std::string& input) {
    std::string cmd = trim(input);

    if (cmd.rfind("/model", 0) == 0) {
        std::string arg = trim(cmd.substr(6));
        if (arg.empty()) {
            std::cout << "  current model: \033[1m" << provider_->model() << "\033[0m\n"
                      << "  set a new model with: /model <name>\n";
        } else {
            provider_->set_model(arg);
            std::cout << "  model set to: \033[1m" << provider_->model() << "\033[0m\n";
        }
        return;
    }

    if (cmd == "/tools" || cmd.rfind("/tools ", 0) == 0) {
        std::string arg = trim(cmd.substr(6));
        std::cout << "  registered tools:\n";
        auto defs = registry_.definitions();
        if (arg.empty()) {
            for (const auto& def : defs) {
                std::cout << "    - \033[1m" << def.name << "\033[0m  " << def.description << "\n";
            }
        } else {
            Tool* tool = registry_.get(arg);
            if (tool) {
                std::cout << "    - \033[1m" << tool->name() << "\033[0m  " << tool->description() << "\n"
                          << "      schema: " << tool->parameters_schema().dump() << "\n";
            } else {
                std::cout << "    unknown tool: '" << arg << "'\n";
            }
        }
        return;
    }

    if (cmd == "/clear") {
        messages_.clear();
        std::cout << "  conversation cleared.\n";
        return;
    }

    if (cmd == "/help") {
        std::cout << "  /model            show current model\n"
                  << "  /model <name>     switch model (e.g. /model openai/gpt-3.5)\n"
                  << "  /tools            list tools\n"
                  << "  /tools <name>     show tool schema\n"
                  << "  /clear            clear conversation history\n"
                  << "  /exit, /quit      leave the REPL\n";
        return;
    }

    if (cmd == "/exit" || cmd == "/quit") {
        return;
    }

    std::cout << "  unknown command: '" << cmd << "'   (try /help)\n";
}

int Agent::repl() {
    std::cout << "\n"
              << "  \033[1mZVMH\033[0m · coding agent\n"
              << "  " << std::string(38, '-') << "\n"
              << "  provider : \033[1m" << provider_->name() << "\033[0m\n"
              << "  model    : \033[1m" << provider_->model() << "\033[0m\n"
              << "  tools    : " << registry_.definitions().size() << " registered\n"
              << "  " << std::string(38, '-') << "\n"
              << "  Commands: '/help' for commands, 'exit' or 'quit' to leave\n\n";

    std::string input;
    while (true) {
        std::cout << "\033[1;32m> \033[0m" << std::flush;
        std::getline(std::cin, input);
        if (!std::cin) {
            std::cout << "\n";
            break;
        }
        if (input.empty()) {
            continue;
        }
        if (input == "exit" || input == "quit" || input == "/exit" || input == "/quit") {
            break;
        }
        if (input[0] == '/') {
            handle_command(input);
            continue;
        }
        run_turn(input);
    }

    std::cout << "\nGoodbye.\n";
    return 0;
}