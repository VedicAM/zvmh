#include "claudeauth.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <chrono>
#include <cstdlib>

ClaudeCredentials load_claude_credentials() {
    const char* home = getenv("HOME");
    if (!home) {
        throw std::runtime_error("Could not find HOME environment variable");
    }

    std::string path = std::string(home) + "/.claude/.credentials.json";
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not read credentials from " + path);
    }

    nlohmann::json json;
    file >> json;

    if (!json.contains("claudeAiOauth") || json["claudeAiOauth"].is_null()) {
        throw std::runtime_error("No claudeAiOauth found in credentials");
    }

    auto& oauth = json["claudeAiOauth"];

    ClaudeCredentials creds;
    creds.access_token = oauth["accessToken"].get<std::string>();
    creds.refresh_token = oauth["refreshToken"].get<std::string>();
    creds.expires_at = oauth["expiresAt"].get<int64_t>();
    creds.subscription_type = oauth.value("subscriptionType", "");

    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    if (creds.expires_at < now_ms) {
        throw std::runtime_error("Claude OAuth token expired. Run 'claude' and '/login' to refresh.");
    }

    return creds;
}
