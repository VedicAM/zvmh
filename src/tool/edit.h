#ifndef TOOL_EDIT_H
#define TOOL_EDIT_H

#include "tool.h"
#include <fstream>
#include <sstream>

class EditTool : public Tool {
public:
    const char* name() const override { return "edit"; }
    const char* description() const override {
        return "Edit a file by searching for a string and replacing it";
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