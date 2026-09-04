#ifndef CLAUDE_H
#define CLAUDE_H

#include "provider.h"
#include "../auth/claudeauth.h"

class Claude : public Provider {
private:
    ClaudeCredentials credentials_;

    nlohmann::json convert_messages(const std::vector<Message>& messages);
    nlohmann::json convert_tools(const std::vector<ToolDefinition>& tools);

public:
    explicit Claude(const ClaudeCredentials& credentials);
    ChatResponse chat(const std::string& prompt) override;
    EventStream complete(
        const std::vector<Message>& messages,
        const std::vector<ToolDefinition>& tools,
        const std::string& system
    ) override;
    const char* name() const override;
};

#endif
