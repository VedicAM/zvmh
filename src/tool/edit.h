#ifndef TOOL_EDIT_H
#define TOOL_EDIT_H

#include "tool.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <algorithm>

class EditTool : public Tool {
public:
    const char* name() const override { return "edit"; }
    const char* description() const override {
        return R"(Replace exact text in one file and return a diff preview with the replacement count. Line endings in old_string/new_string are normalized to the file's own ending, and a UTF-8 BOM is preserved.

WHEN TO USE
- replacing a specific exact string that occurs once in a file
- fixing a line, renaming a symbol, or tweaking a config value
- replacing every occurrence of a string in one call via replace_all

WHEN NOT TO USE
- creating a new file or replacing an entire file: use write
- multi-file changes: use apply_patch
- deleting a file: use apply_patch or bash rm

DO NOT USE FOR
- fuzzy or approximate matching: old_string must match byte-for-byte, so use read first if you have not seen the current text
- append-only changes: use bash >> 
- text that repeats without unique context: the tool fails on multiple exact matches unless replace_all is set

USAGE
- file_path: absolute or repo-relative path
- old_string: exact text to replace; must not be empty and must differ from new_string
- new_string: replacement text; whitespace, tabs, and newlines count
- replace_all: true replaces every exact occurrence instead of the first (default false)
- fails when old_string is not found, when it matches more than once and replace_all is false, or if the file changed between read and write
- output is one line per success plus a 6-line diff preview of old and new text

EXAMPLES
- {"file_path": "src/agent.h", "old_string": "return 0;", "new_string": "return 1;"}
- {"file_path": "README.md", "old_string": "TODO", "new_string": "DONE", "replace_all": true})";
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
                    {"description", "Exact text to replace; must match including whitespace and indentation"}
                }},
                {"new_string", {
                    {"type", "string"},
                    {"description", "Replacement text, which must differ from old_string"}
                }},
                {"replace_all", {
                    {"type", "boolean"},
                    {"description", "Replace all exact occurrences of old_string instead of the first (default false)"}
                }}
            }},
            {"required", nlohmann::json::array({"file_path", "old_string", "new_string"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        const std::string file_path = input["file_path"].get<std::string>();
        const std::string old_string = input["old_string"].get<std::string>();
        const std::string new_string = input["new_string"].get<std::string>();
        const bool replace_all = input.value("replace_all", false);

        if (old_string == new_string) {
            return "Error: No changes to apply: old_string and new_string are identical.";
        }
        if (old_string.empty()) {
            return "Error: old_string must not be empty. Use write to create or overwrite a file.";
        }

        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            return "Error: Could not open file: " + file_path;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string raw = buffer.str();

        const bool bom = raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
                         static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF;
        const std::string text = bom ? raw.substr(3) : raw;

        const std::string ending = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        const std::string old_match = convert_to_line_ending(old_string, ending);
        const std::string new_match = convert_to_line_ending(new_string, ending);

        const size_t replacements = count_occurrences(text, old_match);
        if (replacements == 0) {
            return "Error: Could not find old_string in the file. It must match exactly, including whitespace and indentation.";
        }
        if (replacements > 1 && !replace_all) {
            return "Error: Found multiple exact matches for old_string. Provide more surrounding context or set replace_all to true.";
        }

        const std::string replaced = replace_all ? replace_all_str(text, old_match, new_match)
                                                 : replace_first(text, old_match, new_match);

        std::ifstream now(file_path, std::ios::binary);
        std::stringstream now_buf;
        now_buf << now.rdbuf();
        if (now_buf.str() != raw) {
            return "Error: File changed after permission approval. Read it again before editing.";
        }

        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return "Error: Could not write file: " + file_path;
        }
        if (bom) out << "\xEF\xBB\xBF";
        out << replaced;
        if (!out) return "Error: Could not write file: " + file_path;

        std::string result = "Edited file successfully: " + file_path +
                             "\nReplacements: " + std::to_string(replacements) + "\n```diff\n";
        for (const std::string& line : preview_lines(old_string, '-')) result += line + "\n";
        for (const std::string& line : preview_lines(new_string, '+')) result += line + "\n";
        result += "```";
        return result;
    }

private:
    static std::string normalize_line_endings(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
            out.push_back(s[i]);
        }
        return out;
    }

    static std::string convert_to_line_ending(const std::string& s, const std::string& ending) {
        std::string norm = normalize_line_endings(s);
        if (ending == "\n") return norm;
        std::string out;
        out.reserve(norm.size());
        for (const char c : norm) {
            if (c == '\n') {
                out += "\r\n";
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    static size_t count_occurrences(const std::string& content, const std::string& search) {
        size_t count = 0;
        size_t offset = 0;
        while ((offset = content.find(search, offset)) != std::string::npos) {
            ++count;
            offset += search.size();
        }
        return count;
    }

    static std::string replace_first(std::string s, const std::string& from, const std::string& to) {
        const size_t pos = s.find(from);
        if (pos != std::string::npos) s.replace(pos, from.size(), to);
        return s;
    }

    static std::string replace_all_str(std::string s, const std::string& from, const std::string& to) {
        if (from.empty()) return s;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return s;
    }

    static std::vector<std::string> preview_lines(const std::string& value, char prefix) {
        const std::string norm = normalize_line_endings(value);
        std::vector<std::string> lines;
        size_t start = 0;
        for (size_t i = 0; i <= norm.size(); ++i) {
            if (i == norm.size() || norm[i] == '\n') {
                lines.push_back(norm.substr(start, i - start));
                start = i + 1;
            }
        }
        std::vector<std::string> shown;
        for (size_t i = 0; i < lines.size() && i < 6; ++i) {
            std::string line = lines[i];
            if (line.size() > 240) line = line.substr(0, 240) + "...";
            shown.push_back(std::string(1, prefix) + line);
        }
        if (lines.size() > shown.size()) shown.push_back(std::string(1, prefix) + "...");
        return shown;
    }
};

#endif