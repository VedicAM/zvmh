#ifndef AGENT_H
#define AGENT_H

#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "provider/provider.h"
#include "message/message.h"
#include "tool/registry.h"

class StreamSink {
public:
    virtual ~StreamSink() = default;
    virtual void header(const std::string& provider, const std::string& model) {}
    virtual void text_delta(const std::string& text) {}
    virtual void tool_start(const std::string& name) {}
    virtual void summary(const std::string& model, int prompt_tokens, int completion_tokens) {}
    virtual void tool_call(const std::string& name, const nlohmann::json& args) {}
    virtual void tool_result(const std::string& result, bool is_error) {}
    virtual void warning(const std::string& text) {}
};

class Agent {
private:
    std::unique_ptr<Provider> provider_;
    std::vector<Message> messages_;
    std::string system_prompt_;
    Registry registry_;

public:
    explicit Agent(std::unique_ptr<Provider> provider);

    int run_once(const std::string& prompt);
    int run_turn(const std::string& prompt, StreamSink& sink);
    int run_tui();

    std::string provider_name() const { return provider_->name(); }
    std::string model() const { return provider_->model(); }
    void set_model(const std::string& model) { provider_->set_model(model); }
    std::vector<ToolDefinition> tools() const { return registry_.definitions(); }
    Tool* tool(const std::string& name) const { return registry_.get(name); }
    void clear_messages() { messages_.clear(); }

    void add_message(const Message& message);
    void set_system_prompt(const std::string& prompt);
};

#endif