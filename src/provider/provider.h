#ifndef PROVIDER_H
#define PROVIDER_H

#include <string>
#include <vector>
#include "../message/message.h"

struct ChatResponse {
    std::string message;
    std::string model;
    int prompt_tokens;
    int completion_tokens;
};

struct TokenUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
};

class Provider {
protected:
    std::string api_key_;
    std::string model_;
    TokenUsage last_usage_;

public:
    explicit Provider(const std::string& api_key, std::string model = "unknown")
        : api_key_(api_key), model_(std::move(model)) {}
    virtual ~Provider() = default;

    virtual ChatResponse chat(const std::string& prompt) = 0;
    virtual void complete(
        const std::vector<Message>& messages,
        const std::vector<ToolDefinition>& tools,
        const std::string& system,
        EventSink& sink
    ) = 0;
    virtual const char* name() const = 0;

    std::string model() const { return model_; }
    void set_model(const std::string& model) { model_ = model; }
    TokenUsage last_usage() const { return last_usage_; }
};

#endif