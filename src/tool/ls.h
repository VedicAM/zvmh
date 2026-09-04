#ifndef TOOL_LS_H
#define TOOL_LS_H

#include "tool.h"
#include <filesystem>

class LsTool : public Tool {
public:
    const char* name() const override { return "ls"; }
    const char* description() const override {
        return "List files and directories in a path";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "Path to list (default: current directory)"}
                }}
            }}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string path = input.value("path", ".");

        if (!std::filesystem::exists(path)) {
            return "Error: Path not found: " + path;
        }

        std::string result;
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_directory()) {
                result += entry.path().filename().string() + "/\n";
            } else {
                result += entry.path().filename().string() + "\n";
            }
        }

        return result;
    }
};

#endif