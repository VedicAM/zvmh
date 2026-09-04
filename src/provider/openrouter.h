#ifndef OPENROUTER_H
#define OPENROUTER_H

#include "provider.h"

class OpenRouter : public Provider {
public:
    explicit OpenRouter(const std::string& api_key);
    ChatResponse chat(const std::string& prompt) override;
    EventStream complete(
        const std::vector<Message>& messages,
        const std::vector<ToolDefinition>& tools,
        const std::string& system
    ) override;
    const char* name() const override;
};

#endif
