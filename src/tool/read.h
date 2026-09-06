#ifndef TOOL_READ_H
#define TOOL_READ_H

#include "tool.h"
#include <fstream>
#include <sstream>

class ReadTool : public Tool {
public:
    const char* name() const override { return "read"; }
    const char* description() const override {
        return R"(Read a file and return its entire contents.

WHEN TO USE
- inspecting a file named in the prompt to see its exact text, quote it, or plan an edit
- re-checking a file you previously modified

WHEN NOT TO USE
- searching contents across files: use grep
- finding files by name pattern: use glob
- listing directory entries: use ls

DO NOT USE FOR
- huge or binary files: the full content is returned at once; use bash (head) for enormous logs or binary data

USAGE
- file_path: absolute or repo-relative path
- no offset or limit; the whole file comes back in one result

EXAMPLES
- {"file_path": "src/message/message.h"}
- {"file_path": "CMakeLists.txt"})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Path to the file to read"}
                }}
            }},
            {"required", nlohmann::json::array({"file_path"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string file_path = input["file_path"].get<std::string>();

        std::ifstream file(file_path);
        if (!file.is_open()) {
            return "Error: Could not open file: " + file_path;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

#endif