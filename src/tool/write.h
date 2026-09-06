#ifndef TOOL_WRITE_H
#define TOOL_WRITE_H

#include "tool.h"
#include <fstream>

class WriteTool : public Tool {
public:
    const char* name() const override { return "write"; }
    const char* description() const override {
        return R"(Create or overwrite a file with the given content (truncate) and return the written path.

WHEN TO USE
- creating a new file
- replacing an entire existing file with new content in one call
- materializing a template or generated output to disk

WHEN NOT TO USE
- changing part of an existing file: use edit

DO NOT USE FOR
- append-only changes (this truncates!): use bash >> for appends
- files too large to restate in full: use bash heredoc

USAGE
- file_path: absolute or repo-relative destination
- content: exact bytes to write; must contain the whole file
- parent directories must already exist (use bash mkdir first if needed)

EXAMPLES
- {"file_path": "src/tool/foo.h", "content": "#ifndef TOOL_FOO_H\n#define TOOL_FOO_H\n\n#endif"}
- {"file_path": "README.md", "content": "# zvmh\n\nC++17 coding agent.\n"})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Path to the file to write"}
                }},
                {"content", {
                    {"type", "string"},
                    {"description", "Content to write to the file"}
                }}
            }},
            {"required", nlohmann::json::array({"file_path", "content"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string file_path = input["file_path"].get<std::string>();
        std::string content = input["content"].get<std::string>();

        std::ofstream file(file_path, std::ios::trunc);
        if (!file.is_open()) {
            return "Error: Could not open file for writing: " + file_path;
        }

        file << content;
        return "File written: " + file_path;
    }
};

#endif