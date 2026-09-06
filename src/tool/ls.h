#ifndef TOOL_LS_H
#define TOOL_LS_H

#include "tool.h"
#include <filesystem>

class LsTool : public Tool {
public:
    const char* name() const override { return "ls"; }
    const char* description() const override {
        return R"(List the immediate entries of a directory, one per line, directories suffixed with "/".

WHEN TO USE
- exploring an unknown directory structure before reading files
- confirming what files exist in a folder

WHEN NOT TO USE
- finding files by name pattern: use glob (recursive)
- searching file contents: use grep
- reading a file: use read

DO NOT USE FOR
- recursive listings: this lists only the direct children; use glob or bash for deeper trees

USAGE
- path: optional, defaults to "." (workspace root)
- entries are unsorted (filesystem order); directories end with "/"

EXAMPLES
- {"path": "src"}
- {"path": "vendor"})";
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