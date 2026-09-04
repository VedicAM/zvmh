#ifndef TOOL_GREP_H
#define TOOL_GREP_H

#include "tool.h"
#include <fstream>
#include <sstream>
#include <filesystem>

class GrepTool : public Tool {
public:
    const char* name() const override { return "grep"; }
    const char* description() const override {
        return "Search for a regex pattern in files";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"pattern", {
                    {"type", "string"},
                    {"description", "Regex pattern to search for"}
                }},
                {"path", {
                    {"type", "string"},
                    {"description", "File or directory to search in (default: current directory)"}
                }},
                {"include", {
                    {"type", "string"},
                    {"description", "File pattern to include (e.g. *.cpp, *.h)"}
                }}
            }},
            {"required", nlohmann::json::array({"pattern"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string pattern = input["pattern"].get<std::string>();
        std::string path = input.value("path", ".");

        std::string result;
        std::string include_pattern = input.value("include", "");

        if (std::filesystem::is_regular_file(path)) {
            result += search_file(path, pattern);
        } else if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    if (include_pattern.empty() || matches_pattern(filename, include_pattern)) {
                        result += search_file(entry.path().string(), pattern);
                    }
                }
            }
        } else {
            return "Error: Path not found: " + path;
        }

        if (result.empty()) {
            return "No matches found";
        }

        return result;
    }

private:
    std::string search_file(const std::string& file_path, const std::string& pattern) {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return "";
        }

        std::string result;
        std::string line;
        int line_num = 1;

        while (std::getline(file, line)) {
            if (line.find(pattern) != std::string::npos) {
                result += file_path + ":" + std::to_string(line_num) + ": " + line + "\n";
            }
            line_num++;
        }

        return result;
    }

    bool matches_pattern(const std::string& filename, const std::string& pattern) {
        return filename.find(pattern) != std::string::npos;
    }
};

#endif