#ifndef PROVIDER_H
#define PROVIDER_H

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../message/message.h"

struct ChatResponse {
    std::string message;
    std::string model;
    int prompt_tokens;
    int completion_tokens;
};

struct TokenUsage {
    std::atomic<int> prompt_tokens{0};
    std::atomic<int> completion_tokens{0};

    TokenUsage() = default;
    TokenUsage(const TokenUsage& o) : prompt_tokens(o.prompt_tokens.load()), completion_tokens(o.completion_tokens.load()) {}
    TokenUsage& operator=(const TokenUsage& o) {
        prompt_tokens.store(o.prompt_tokens.load());
        completion_tokens.store(o.completion_tokens.load());
        return *this;
    }
};

class Provider {
protected:
    std::string api_key_;
    std::string model_;
    TokenUsage last_usage_;
    std::mutex context_mutex_;
    std::unordered_map<std::string, int> context_cache_;

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

    // How many tokens fit in `model`'s context window; -1 if unknown.
    // Subclasses may override to query their model catalog; results are cached
    // per model so only the first lookup for a given model hits the network.
    virtual int query_context_window(const std::string& model) { return -1; }

    int context_window(const std::string& model) {
        std::lock_guard<std::mutex> lock(context_mutex_);
        auto it = context_cache_.find(model);
        if (it != context_cache_.end()) {
            return it->second;
        }
        int ctx = query_context_window(model);
        context_cache_[model] = ctx;
        return ctx;
    }
};

#endif