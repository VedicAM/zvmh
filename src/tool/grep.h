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
        return R"(Search for a literal substring in file contents and return matching lines as file:line: text.

WHEN TO USE
- locating where a symbol, word, or string appears in the codebase before reading it
- checking whether a term is referenced anywhere

WHEN NOT TO USE
- finding files by name: use glob
- reading an entire file: use read

DO NOT USE FOR
- regex: matching is a plain substring search, not a regex engine

USAGE
- pattern: literal substring; regex metacharacters have no special meaning
- path: optional file or directory (default: workspace root); include: optional literal filename filter
- case-sensitive; capped at 50 total matches (notice appended)
- directories are searched recursively

EXAMPLES
- {"pattern": "run_turn"}
- {"pattern": "TODO", "path": "src"}
- {"pattern": "class Tool", "path": "src", "include": ".h"})";
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
        int total_results = 0;

        if (std::filesystem::is_regular_file(path)) {
            result += search_file(path, pattern, total_results);
        } else if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (total_results >= 50) break;
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    if (include_pattern.empty() || matches_pattern(filename, include_pattern)) {
                        result += search_file(entry.path().string(), pattern, total_results);
                    }
                }
            }
        } else {
            return "Error: Path not found: " + path;
        }

        if (result.empty()) {
            return "No matches found";
        }

        if (total_results >= 50) {
            result += "\n(Results limited to 50 matches)";
        }

        return result;
    }

private:
    std::string search_file(const std::string& file_path, const std::string& pattern, int& total_results) {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return "";
        }

        std::string result;
        std::string line;
        int line_num = 1;

        while (std::getline(file, line)) {
            if (total_results >= 50) break;
            if (line.find(pattern) != std::string::npos) {
                result += file_path + ":" + std::to_string(line_num) + ": " + line + "\n";
                total_results++;
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