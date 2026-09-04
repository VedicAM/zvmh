#ifndef CLAUDE_AUTH_H
#define CLAUDE_AUTH_H

#include <string>

struct ClaudeCredentials {
    std::string access_token;
    std::string refresh_token;
    int64_t expires_at;
    std::string subscription_type;
};

ClaudeCredentials load_claude_credentials();

#endif
