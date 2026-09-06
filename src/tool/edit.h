#ifndef TOOL_EDIT_H
#define TOOL_EDIT_H

#include "tool.h"
#include <fstream>
#include <sstream>

class EditTool : public Tool {
public:
    const char* name() const override { return "edit"; }
    const char* description() const override {
        return R"(Replace the first occurrence of old_string with new_string inside a file.

WHEN TO USE
- modifying a specific part of an existing file without rewriting the rest
- fixing a line, renaming a symbol, or tweaking a config value

WHEN NOT TO USE
- replacing a whole file: use write
- creating a new file: use write

DO NOT USE FOR
- bulk or repeated replacements: only the first occurrence is replaced; apply edit once per target or use bash sed
- blind edits without knowing the exact current text: read first, then match byte-for-byte

USAGE
- file_path: file to modify
- old_string: must match the file byte-for-byte; include surrounding context to disambiguate repeats
- new_string: replacement; empty deletes old_string
- whitespace, tabs, and newlines count

EXAMPLES
- {"file_path": "src/agent.h", "old_string": "std::string system_prompt_;", "new_string": "std::string system_prompt_ = \"You are a helpful assistant.\";"}
- {"file_path": "CMakeLists.txt", "old_string": "src/agent.cpp", "new_string": "src/agent.cpp\n    src/main.cpp"})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Path to the file to edit"}
                }},
                {"old_string", {
                    {"type", "string"},
                    {"description", "The string to search for"}
                }},
                {"new_string", {
                    {"type", "string"},
                    {"description", "The replacement string"}
                }}
            }},
            {"required", nlohmann::json::array({"file_path", "old_string", "new_string"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string file_path = input["file_path"].get<std::string>();
        std::string old_string = input["old_string"].get<std::string>();
        std::string new_string = input["new_string"].get<std::string>();

        std::ifstream file(file_path);
        if (!file.is_open()) {
            return "Error: Could not open file: " + file_path;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        size_t pos = content.find(old_string);
        if (pos == std::string::npos) {
            return "Error: Old string not found in file";
        }

        content.replace(pos, old_string.length(), new_string);

        std::ofstream out(file_path, std::ios::trunc);
        out << content;
        return "File edited: " + file_path;
    }
};

#endif