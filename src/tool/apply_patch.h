#ifndef TOOL_APPLY_PATCH_H
#define TOOL_APPLY_PATCH_H

#include "tool.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <optional>
#include <set>

class ApplyPatchTool : public Tool {
public:
    const char* name() const override { return "apply_patch"; }
    const char* description() const override {
        return R"(Apply one patch containing add, update, and delete file operations. All targets are resolved and validated before any file is written; operations then apply sequentially, so a failure mid-patch leaves earlier operations applied and the error names them.

WHEN TO USE
- creating, modifying, and deleting several files in a single call
- bulk edits where many separate edit/write calls would be wasteful

WHEN NOT TO USE
- changing one exact spot in an existing file: use edit
- creating or fully overwriting one file: use write
- deleting one file: use bash rm

DO NOT USE FOR
- files whose exact current text you have not read: update hunks must match the file byte-for-byte, so use read first
- binary files: content is treated as text and line endings are normalized to LF

USAGE
- patch_text: sections separated by directive lines; "*** Begin Patch" and "*** End Patch" markers are optional
- "*** Add File: path" followed by one "+" line per file line (a blank line makes an empty line)
- "*** Update File: path" followed by unified-diff hunks: "@@" starts a hunk, " " is context, "-" is removed, "+" is added
- "*** Delete File: path" takes no content
- add requires the file to be absent; update and delete require it to exist
- every update hunk must match exactly once: no match or multiple matches fail the patch
- ops run in patch order; the result lists each applied op as "A", "M", or "D" plus its path
- parent directories must already exist

EXAMPLES
- {"patch_text": "*** Add File: notes.txt\n+hello\n+world\n"}
- {"patch_text": "*** Update File: src/agent.h\n@@\n-    return 0;\n+    return 1;\n"}
- {"patch_text": "*** Delete File: scratch.txt\n"})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"patch_text", {
                    {"type", "string"},
                    {"description", "The full patch text describing add, update, and delete operations"}
                }}
            }},
            {"required", nlohmann::json::array({"patch_text"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        const std::string patch_text = input.value("patch_text", std::string());
        if (patch_text.empty()) return "Error: patch_text is required";

        std::vector<Hunk> hunks;
        if (!parse_patch(patch_text, hunks)) {
            return "Error: apply_patch verification failed: " + parse_error_;
        }
        if (hunks.empty()) return "Error: patch rejected: empty patch";

        std::map<std::string, std::string> content;
        std::set<std::string> deleted;
        std::vector<Prepared> prepared;

        for (const Hunk& hunk : hunks) {
            std::optional<std::string> cur = current_content(hunk.path, content, deleted);
            if (hunk.type == Hunk::Type::Add) {
                if (cur) return "Error: Unable to apply patch at " + hunk.path + ": file already exists";
                Prepared p;
                p.type = hunk.type;
                p.path = hunk.path;
                p.content = add_content(hunk.lines);
                content[hunk.path] = p.content;
                prepared.push_back(std::move(p));
            } else if (hunk.type == Hunk::Type::Delete) {
                if (!cur) return "Error: Unable to apply patch at " + hunk.path + ": file not found";
                content.erase(hunk.path);
                deleted.insert(hunk.path);
                Prepared p;
                p.type = hunk.type;
                p.path = hunk.path;
                prepared.push_back(std::move(p));
            } else {
                if (!cur) return "Error: Unable to apply patch at " + hunk.path + ": file not found";
                std::string next;
                if (!apply_updates(*cur, hunk.lines, hunk.path, next)) {
                    return "Error: Unable to apply patch at " + hunk.path + ": " + update_error_;
                }
                Prepared p;
                p.type = hunk.type;
                p.path = hunk.path;
                p.content = next;
                content[hunk.path] = next;
                prepared.push_back(std::move(p));
            }
        }

        std::vector<std::string> applied;
        for (const Prepared& p : prepared) {
            bool ok = p.type == Hunk::Type::Delete ? std::filesystem::remove(p.path) : write_file(p.path, p.content);
            if (!ok) {
                std::string msg = "Error: Patch partially applied before failing at " + p.path;
                if (!applied.empty()) {
                    msg += ". Applied: " + join(applied, ", ");
                }
                return msg;
            }
            applied.push_back(std::string(p.type == Hunk::Type::Add ? "A " : p.type == Hunk::Type::Update ? "M " : "D ") + p.path);
        }

        return "Applied patch sequentially:\n" + join(applied, "\n");
    }

private:
    struct Hunk {
        enum class Type { Add, Update, Delete };
        Type type;
        std::string path;
        std::vector<std::string> lines;
    };

    struct Prepared {
        Hunk::Type type;
        std::string path;
        std::string content;
    };

    std::string parse_error_;
    std::string update_error_;

    static std::vector<std::string> split_lines(const std::string& text) {
        std::vector<std::string> out;
        std::string cur;
        for (const char c : text) {
            if (c == '\n') {
                if (!cur.empty() && cur.back() == '\r') cur.pop_back();
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            if (cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
        }
        return out;
    }

    static std::string join(const std::vector<std::string>& v, const std::string& sep) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += sep;
            s += v[i];
        }
        return s;
    }

    static std::string trim(const std::string& s) {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        const size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static bool starts_with(const std::string& s, const std::string& prefix) {
        return s.rfind(prefix, 0) == 0;
    }

    static bool parse_section_header(const std::string& line, std::string& type, std::string& path) {
        const std::string add = "*** Add File: ";
        const std::string update = "*** Update File: ";
        const std::string del = "*** Delete File: ";
        if (starts_with(line, add)) { type = "add"; path = line.substr(add.size()); return true; }
        if (starts_with(line, update)) { type = "update"; path = line.substr(update.size()); return true; }
        if (starts_with(line, del)) { type = "delete"; path = line.substr(del.size()); return true; }
        return false;
    }

    bool parse_patch(const std::string& patch_text, std::vector<Hunk>& hunks) {
        const std::vector<std::string> lines = split_lines(patch_text);
        std::string current_type;
        std::string current_path;
        std::vector<std::string> current_lines;

        const auto flush = [&]() -> bool {
            if (current_type.empty()) return true;
            if (current_type == "add") {
                for (const std::string& ln : current_lines) {
                    if (!ln.empty() && ln[0] != '+') {
                        parse_error_ = "add section for " + current_path + " contains a line without '+' prefix: " + ln;
                        return false;
                    }
                }
            }
            hunks.push_back({current_type == "add" ? Hunk::Type::Add : current_type == "update" ? Hunk::Type::Update : Hunk::Type::Delete, current_path, current_lines});
            current_type.clear();
            current_path.clear();
            current_lines.clear();
            return true;
        };

        for (const std::string& line : lines) {
            const std::string trimmed = trim(line);
            if (trimmed.empty()) {
                if (!current_type.empty()) current_lines.push_back(line);
                continue;
            }
            if (starts_with(trimmed, "*** Begin Patch") || starts_with(trimmed, "*** End Patch")) {
                if (!flush()) return false;
                continue;
            }
            if (starts_with(trimmed, "*** ")) {
                std::string type, path;
                if (!parse_section_header(trimmed, type, path)) {
                    parse_error_ = "unknown directive: " + trimmed;
                    return false;
                }
                if (!flush()) return false;
                if (path.empty()) {
                    parse_error_ = type + " section with an empty path";
                    return false;
                }
                current_type = type;
                current_path = path;
                continue;
            }
            if (current_type.empty()) {
                parse_error_ = "line outside of a patch section: " + trimmed;
                return false;
            }
            current_lines.push_back(line);
        }
        return flush();
    }

    static std::optional<std::string> read_file(const std::string& path) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::nullopt;
        std::stringstream buf;
        buf << f.rdbuf();
        return buf.str();
    }

    static bool write_file(const std::string& path, const std::string& content) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << content;
        return static_cast<bool>(f);
    }

    static std::optional<std::string> current_content(const std::string& path,
                                                      const std::map<std::string, std::string>& content,
                                                      const std::set<std::string>& deleted) {
        const auto it = content.find(path);
        if (it != content.end()) return it->second;
        if (deleted.count(path)) return std::nullopt;
        return read_file(path);
    }

    static std::string add_content(const std::vector<std::string>& lines) {
        std::vector<std::string> out;
        out.reserve(lines.size());
        for (const std::string& line : lines) {
            out.push_back(line.empty() ? "" : line.substr(1));
        }
        std::string c = join(out, "\n");
        if (!c.empty() && c.back() != '\n') c += "\n";
        return c;
    }

    static std::vector<size_t> occurrences(const std::vector<std::string>& lines,
                                           const std::vector<std::string>& block) {
        std::vector<size_t> occ;
        if (block.size() > lines.size()) return occ;
        for (size_t i = 0; i + block.size() <= lines.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < block.size(); ++j) {
                if (lines[i + j] != block[j]) { match = false; break; }
            }
            if (match) occ.push_back(i);
        }
        return occ;
    }

    bool apply_updates(const std::string& content, const std::vector<std::string>& raw_lines,
                       const std::string& path, std::string& out) {
        std::vector<std::vector<std::pair<char, std::string>>> groups;
        std::vector<std::pair<char, std::string>> group;
        for (const std::string& raw : raw_lines) {
            if (raw == "@@") {
                if (!group.empty()) { groups.push_back(std::move(group)); group.clear(); }
                continue;
            }
            if (raw.empty()) { group.emplace_back(' ', ""); continue; }
            const char mark = raw[0];
            if (mark == ' ' || mark == '-' || mark == '+') { group.emplace_back(mark, raw.substr(1)); continue; }
            update_error_ = "malformed update line in " + path + ": " + raw;
            return false;
        }
        if (!group.empty()) groups.push_back(std::move(group));
        if (groups.empty()) { update_error_ = "empty update section in " + path; return false; }

        const bool had_nl = !content.empty() && content.back() == '\n';
        std::vector<std::string> cur = split_lines(content);
        for (const auto& g : groups) {
            std::vector<std::string> old_l, new_l;
            for (const auto& [mark, text] : g) {
                if (mark == ' ' || mark == '-') old_l.push_back(text);
                if (mark == ' ' || mark == '+') new_l.push_back(text);
            }
            if (old_l.empty()) { update_error_ = "update hunk in " + path + " has nothing to match"; return false; }
            const std::vector<size_t> occ = occurrences(cur, old_l);
            if (occ.empty()) { update_error_ = "update hunk in " + path + " does not match the file"; return false; }
            if (occ.size() > 1) { update_error_ = "update hunk in " + path + " matches multiple locations"; return false; }
            std::vector<std::string> next;
            next.insert(next.end(), cur.begin(), cur.begin() + static_cast<long>(occ[0]));
            next.insert(next.end(), new_l.begin(), new_l.end());
            next.insert(next.end(), cur.begin() + static_cast<long>(occ[0]) + static_cast<long>(old_l.size()), cur.end());
            cur = std::move(next);
        }
        out = join(cur, "\n");
        if (had_nl) out += "\n";
        return true;
    }
};

#endif