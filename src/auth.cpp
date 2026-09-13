#include "auth.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <sys/stat.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

namespace auth {

namespace {

std::string default_dir() {
    const char* home = getenv("HOME");
    return home ? (std::filesystem::path(home) / ".zvmh").string()
                : std::string(".zvmh");
}

}  // namespace

const std::vector<ProviderInfo>& supported_providers() {
    static const std::vector<ProviderInfo> providers = {
        {"openrouter", "OpenRouter"},
    };
    return providers;
}

std::string auth_file_path() {
    return (std::filesystem::path(default_dir()) / "auth.json").string();
}

bool load_credentials(std::string& provider_id, std::string& api_key) {
    provider_id.clear();
    api_key.clear();

    const std::string path = auth_file_path();
    std::ifstream f(path);
    if (!f.is_open()) return false;

    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;

    provider_id = j.value("provider", "");
    api_key = j.value("api_key", "");
    if (provider_id.empty() || api_key.empty()) return false;

    bool known = false;
    for (const auto& p : supported_providers()) {
        if (p.id == provider_id) {
            known = true;
            break;
        }
    }
    return known;
}

bool save_credentials(const std::string& provider_id, const std::string& api_key) {
    if (provider_id.empty() || api_key.empty()) return false;

    bool known = false;
    for (const auto& p : supported_providers()) {
        if (p.id == provider_id) {
            known = true;
            break;
        }
    }
    if (!known) return false;

    nlohmann::json j;
    j["provider"] = provider_id;
    j["api_key"] = api_key;

    const std::filesystem::path path(auth_file_path());
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return false;
        out << j.dump(2) << "\n";
        out.flush();
        if (!out) {
            out.close();
            std::remove(tmp.c_str());
            return false;
        }
    }
    ::chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string active_api_key() {
    const char* key = getenv("OPENROUTER_API_KEY");
    if (key && *key) return key;

    std::string provider_id;
    std::string api_key;
    if (load_credentials(provider_id, api_key)) return api_key;
    return "";
}

std::string active_provider_id() {
    std::string provider_id;
    std::string api_key;
    if (load_credentials(provider_id, api_key)) return provider_id;
    return "";
}

}  // namespace auth