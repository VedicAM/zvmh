#ifndef SYSTEM_BUILTINS_H
#define SYSTEM_BUILTINS_H

#include "registry.h"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Built-in system context sources: core/environment (working directory, workspace
// root, git status, platform), core/date (today's date), and core/agents-md
// (AGENTS.md files walked up the directory tree).

namespace sysctx {

inline std::string platform_name() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "win32";
#else
    return "unknown";
#endif
}

inline std::filesystem::path find_vcs_root(const std::filesystem::path& start) {
    std::filesystem::path p = std::filesystem::absolute(start);
    for (;;) {
        if (std::filesystem::exists(p / ".git")) return p;
        std::filesystem::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return std::filesystem::path();
}

inline std::string current_date_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a %b %d %Y", &tm);
    return buf;
}

inline std::string build_environment_block() {
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path root;
    bool git = false;
    try {
        root = find_vcs_root(cwd);
        git = !root.empty();
    } catch (...) {
    }
    if (root.empty()) root = cwd;

    std::string out;
    out += "<env>\n";
    out += "  Working directory: " + cwd.string() + "\n";
    out += "  Workspace root folder: " + root.string() + "\n";
    out += "  Is directory a git repo: " + std::string(git ? "yes" : "no") + "\n";
    out += "  Platform: " + platform_name() + "\n";
    out += "</env>";
    return out;
}

inline std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

inline std::string collect_agents_md() {
    std::filesystem::path start = std::filesystem::current_path();
    std::vector<std::string> contents;
    std::filesystem::path p = std::filesystem::absolute(start);
    std::filesystem::path prev;
    for (;;) {
        std::filesystem::path file = p / "AGENTS.md";
        if (std::filesystem::is_regular_file(file)) {
            std::string text = read_file_text(file);
            if (!text.empty()) contents.push_back(text);
        }
        std::filesystem::path parent = p.parent_path();
        if (parent == p || parent == prev) break;
        prev = p;
        p = parent;
    }
    std::string out;
    for (size_t i = 0; i < contents.size(); ++i) {
        if (i) out += "\n\n";
        out += contents[i];
    }
    return out;
}

inline std::vector<RegistrationHandle> register_system_context_builtins(SystemContextRegistry& registry) {
    std::vector<RegistrationHandle> handles;
    handles.push_back(registry.register_context("core/environment", []() -> SystemContext {
        return make(Source<std::string>{
            Key("core/environment"),
            []() -> std::variant<Unavailable, std::string> { return build_environment_block(); },
            [](const std::string& env) {
                return "Here is some useful information about the environment you are running in:\n" + env;
            },
            [](const std::string&, const std::string& env) {
                return "The environment you are running in is now:\n" + env;
            },
            {},
        });
    }));
    handles.push_back(registry.register_context("core/date", []() -> SystemContext {
        return make(Source<std::string>{
            Key("core/date"),
            []() -> std::variant<Unavailable, std::string> { return current_date_string(); },
            [](const std::string& date) { return "Today's date: " + date; },
            [](const std::string&, const std::string& date) { return "Today's date is now: " + date; },
            {},
        });
    }));
    if (!collect_agents_md().empty()) {
        handles.push_back(registry.register_context("core/agents-md", []() -> SystemContext {
            return make(Source<std::string>{
                Key("core/agents-md"),
                []() -> std::variant<Unavailable, std::string> {
                    std::string content = collect_agents_md();
                    if (content.empty()) return Unavailable{};
                    return content;
                },
                [](const std::string& content) { return content; },
                [](const std::string&, const std::string& content) { return content; },
                {},
            });
        }));
    }
    return handles;
}

}  // namespace sysctx

#endif