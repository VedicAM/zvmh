#ifndef TOOL_BASH_H
#define TOOL_BASH_H

#include "tool.h"
#include <cstdio>
#include <array>
#include <memory>

class BashTool : public Tool {
public:
    const char* name() const override { return "bash"; }
    const char* description() const override {
        return "Execute a bash command and return its output";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"command", {
                    {"type", "string"},
                    {"description", "The bash command to execute"}
                }}
            }},
            {"required", nlohmann::json::array({"command"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string command = input["command"].get<std::string>();

        std::array<char, 128> buffer;
        std::string result;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen((command + " 2>&1").c_str(), "r"), pclose);

        if (!pipe) {
            return "Error: Could not open pipe";
        }

        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }

        return result;
    }
};

#endif