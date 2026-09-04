#ifndef TOOL_WRITE_H
#define TOOL_WRITE_H

#include "tool.h"
#include <fstream>

class WriteTool : public Tool {
public:
    const char* name() const override { return "write"; }
    const char* description() const override {
        return "Write content to a file, overwriting if it exists";
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