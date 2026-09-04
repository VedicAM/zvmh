#ifndef TOOL_READ_H
#define TOOL_READ_H

#include "tool.h"
#include <fstream>
#include <sstream>

class ReadTool : public Tool {
public:
    const char* name() const override { return "read"; }
    const char* description() const override {
        return "Read the contents of a file";
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