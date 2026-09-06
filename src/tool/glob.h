#ifndef TOOL_GLOB_H
#define TOOL_GLOB_H

#include "tool.h"
#include <glob.h>
#include <string>

class GlobTool : public Tool {
public:
    const char* name() const override { return "glob"; }
    const char* description() const override {
        return R"(Find files and directories matching a glob pattern and return their paths, one per line.

WHEN TO USE
- the prompt names a file pattern to find ("all .cpp files", "every *_test.py")
- enumerating files before reading them

WHEN NOT TO USE
- searching file contents: use grep
- listing a single directory's entries: use ls

DO NOT USE FOR
- regex or content search: this matches names only

USAGE
- pattern: glob such as src/*.h or **/CMakeLists.txt; each * matches one path component (** is not deeply recursive)
- path: optional base directory prepended to the pattern (default: workspace root)
- returns an error string when nothing matches

EXAMPLES
- {"pattern": "src/*/*.h"}
- {"pattern": "**/CMakeLists.txt", "path": "vendor"}
- {"pattern": "build/zvmh"})";
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