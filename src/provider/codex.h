#ifndef CODEX_H
#define CODEX_H

#include "provider.h"
#include "../auth/codexauth.h"

class Codex : public Provider {
private:
    CodexCredentials credentials_;

    nlohmann::json convert_messages(const std::vector<Message>& messages, const std::string& system);
    nlohmann::json convert_tools(const std::vector<ToolDefinition>& tools);
    EventStream parse_sse_stream(const std::string& response_text);

public:
    explicit Codex(const CodexCredentials& credentials);
    ChatResponse chat(const std::string& prompt) override;
    EventStream complete(
        const std::vector<Message>& messages,
        const std::vector<ToolDefinition>& tools,
        const std::string& system
    ) override;
    const char* name() const override;
};

#endif
