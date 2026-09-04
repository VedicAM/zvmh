#ifndef TOOL_GLOB_H
#define TOOL_GLOB_H

#include "tool.h"
#include <glob.h>
#include <string>

class GlobTool : public Tool {
public:
    const char* name() const override { return "glob"; }
    const char* description() const override {
        return "Find files matching a glob pattern";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"pattern", {
                    {"type", "string"},
                    {"description", "Glob pattern to match files (e.g. **/*.cpp)"}
                }},
                {"path", {
                    {"type", "string"},
                    {"description", "Directory to search in (default: current directory)"}
                }}
            }},
            {"required", nlohmann::json::array({"pattern"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string pattern = input["pattern"].get<std::string>();

        std::string full_pattern;
        if (input.contains("path") && !input["path"].is_null()) {
            full_pattern = input["path"].get<std::string>() + "/" + pattern;
        } else {
            full_pattern = pattern;
        }

        glob_t glob_result;
        int ret = glob(full_pattern.c_str(), GLOB_TILDE, nullptr, &glob_result);
        if (ret != 0) {
            return "Error: No files matched pattern (glob returned " + std::to_string(ret) + ")";
        }

        std::string result;
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            result += glob_result.gl_pathv[i];
            result += "\n";
        }

        globfree(&glob_result);
        return result;
    }
};

#endif