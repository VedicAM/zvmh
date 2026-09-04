#ifndef OPENROUTER_H
#define OPENROUTER_H

#include "provider.h"

class OpenRouter : public Provider {
public:
    explicit OpenRouter(const std::string& api_key);
    ChatResponse chat(const std::string& prompt) override;
    void complete(
        const std::vector<Message>& messages,
        const std::vector<ToolDefinition>& tools,
        const std::string& system,
        EventSink& sink
    ) override;
    const char* name() const override;

private:
    void emit_sse_data(
        const std::string& data,
        EventSink& sink,
        std::string& current_tool_id,
        std::string& current_tool_name
    );
};

#endif
