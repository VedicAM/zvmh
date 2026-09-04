#include "codexauth.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <cstdlib>

CodexCredentials load_codex_credentials() {
    const char* home = getenv("HOME");
    if (!home) {
        throw std::runtime_error("Could not find HOME environment variable");
    }

    std::string path = std::string(home) + "/.codex/auth.json";
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not read credentials from " + path);
    }

    nlohmann::json json;
    file >> json;

    CodexCredentials creds;

    if (json.contains("tokens") && !json["tokens"].is_null()) {
        auto& tokens = json["tokens"];
        creds.access_token = tokens["access_token"].get<std::string>();
        creds.refresh_token = tokens["refresh_token"].get<std::string>();
        creds.id_token = tokens.value("id_token", "");
    } else if (json.contains("OPENAI_API_KEY") && !json["OPENAI_API_KEY"].is_null()) {
        creds.access_token = json["OPENAI_API_KEY"].get<std::string>();
        creds.refresh_token = "";
        creds.id_token = "";
    } else {
        throw std::runtime_error("No tokens or API key found in Codex auth file");
    }

    return creds;
}
