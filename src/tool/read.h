#ifndef TOOL_READ_H
#define TOOL_READ_H

#include "tool.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <set>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include "../utf8.h"

class ReadTool : public Tool {
public:
    static constexpr size_t kMaxReadLines = 2000;
    static constexpr size_t kMaxReadBytes = 50 * 1024;
    static constexpr size_t kMaxLineLength = 2000;

    const char* name() const override { return "read"; }
    const char* description() const override {
        return R"(Read a file (returning its contents, paged on demand) or list a directory (sorted, directories suffixed with "/"). Binary and malformed files are rejected.

WHEN TO USE
- inspecting a file's exact text to quote it or plan an edit
- reading a slice of a large file with offset/limit
- checking the entries of a directory (paged)

WHEN NOT TO USE
- searching contents across files: use grep
- finding files by name pattern: use glob
- a simple unsorted directory listing: use ls

DO NOT USE FOR
- binary, archive, image, or PDF files: they are rejected as binary; use bash (file, strings, unzip) instead
- files with invalid UTF-8: they are rejected; use bash tools instead
- very large files in one go: output is capped, so read in pages via offset/limit

USAGE
- file_path: absolute or repo-relative path to a file or directory
- offset: 1-based line/entry to start at (default 1); past the end of a file it is an error
- limit: maximum lines/entries to return, capped at 2000 (default 2000)
- output text is capped at 50KB; larger files are paged and a truncation marker gives the next offset
- individual lines over 2000 chars are truncated with a marker
- binary detection: known binary/archive extensions, PDF or image magic bytes, NUL bytes, and a high non-printable byte ratio are all rejected

EXAMPLES
- {"file_path": "src/message/message.h"}
- {"file_path": "src/tool/edit.h", "offset": 200, "limit": 100}
- {"file_path": "src/tool"})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"file_path", {
                    {"type", "string"},
                    {"description", "Path to the file or directory to read"}
                }},
                {"offset", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"description", "1-based line or entry to start at (default 1)"}
                }},
                {"limit", {
                    {"type", "integer"},
                    {"minimum", 1},
                    {"maximum", kMaxReadLines},
                    {"description", "Maximum lines or entries to return (default 2000, capped at 2000)"}
                }}
            }},
            {"required", nlohmann::json::array({"file_path"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        const std::string file_path = input["file_path"].get<std::string>();
        const long long raw_offset = input.value("offset", (long long)1);
        const long long raw_limit = input.value("limit", (long long)kMaxReadLines);
        const size_t offset = raw_offset < 1 ? 1 : static_cast<size_t>(raw_offset);
        size_t limit = raw_limit < 1 ? kMaxReadLines : static_cast<size_t>(raw_limit);
        if (limit > kMaxReadLines) limit = kMaxReadLines;
        const bool explicit_page = input.contains("offset") || input.contains("limit");

        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::status(file_path, ec);
        if (ec || !std::filesystem::exists(status)) {
            return "Error: Path is not a file or directory: " + file_path;
        }
        if (std::filesystem::is_directory(status)) {
            return list_directory(file_path, offset, limit);
        }
        if (std::filesystem::is_regular_file(status)) {
            return read_file(file_path, offset, limit, explicit_page);
        }
        return "Error: Path is not a file or directory: " + file_path;
    }

private:
    static std::string join(const std::vector<std::string>& v, const std::string& sep) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += sep;
            s += v[i];
        }
        return s;
    }

    static std::string lowercase(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(static_cast<unsigned char>(c)); });
        return s;
    }

    static const std::set<std::string>& binary_extensions() {
        static const std::set<std::string> ext = {
            ".zip", ".tar", ".gz", ".exe", ".dll", ".so", ".class", ".jar", ".war", ".7z",
            ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".odt", ".ods", ".odp",
            ".bin", ".dat", ".obj", ".o", ".a", ".lib", ".wasm", ".pyc", ".pyo"
        };
        return ext;
    }

    static std::string extension_of(const std::string& path) {
        const size_t dot = path.find_last_of('.');
        const size_t slash = path.find_last_of('/');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
        return lowercase(path.substr(dot));
    }

    static bool binary_extension(const std::string& ext) {
        return binary_extensions().count(ext) > 0;
    }

    static bool starts_with(const std::string& s, const std::vector<unsigned char>& prefix) {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (static_cast<unsigned char>(s[i]) != prefix[i]) return false;
        }
        return true;
    }

    static bool binary_magic(const std::string& bytes) {
        if (starts_with(bytes, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) return true;   // PNG
        if (starts_with(bytes, {0xFF, 0xD8, 0xFF})) return true;                                 // JPEG
        if (starts_with(bytes, {0x47, 0x49, 0x46, 0x38})) return true;                           // GIF
        if (starts_with(bytes, {0x52, 0x49, 0x46, 0x46}) && starts_with(bytes.substr(8), {0x57, 0x45, 0x42, 0x50})) return true;  // WEBP
        if (starts_with(bytes, {0x25, 0x50, 0x44, 0x46})) return true;                           // PDF
        return false;
    }

    static bool binary_detect(const std::string& bytes) {
        if (bytes.empty()) return false;
        size_t non_printable = 0;
        for (const unsigned char b : bytes) {
            if (b == 0) return true;
            if (b < 9 || (b > 13 && b < 32)) ++non_printable;
        }
        return static_cast<double>(non_printable) / bytes.size() > 0.3;
    }

    // Truncates to at most max_bytes without splitting a multi-byte UTF-8 sequence.
    static std::string truncate_to(const std::string& s, size_t max_bytes) {
        if (s.size() <= max_bytes) return s;
        size_t cut = max_bytes;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        return s.substr(0, cut);
    }

    static std::string read_file(const std::string& path, size_t offset, size_t limit, bool explicit_page) {
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        std::ifstream in(path, std::ios::binary);
        if (!in) return "Error: Cannot open file: " + path;

        const std::string ext = extension_of(path);
        if (binary_extension(ext)) return "Error: Cannot read binary file: " + path;

        std::string first;
        {
            std::string buf(64 * 1024, '\0');
            in.read(&buf[0], static_cast<std::streamsize>(buf.size()));
            first = buf.substr(0, static_cast<size_t>(in.gcount()));
        }
        if (binary_magic(first)) return "Error: Cannot read binary file: " + path;

        const bool paged = size > kMaxReadBytes || explicit_page;
        if (!paged) {
            if (binary_detect(first)) return "Error: Cannot read binary file: " + path;
            std::string rest;
            if (size > first.size()) {
                rest.resize(static_cast<size_t>(size) - first.size());
                in.read(&rest[0], static_cast<std::streamsize>(rest.size()));
                rest.resize(static_cast<size_t>(in.gcount()));
            }
            const std::string full = first + rest;
            if (!utf8_valid(full)) return "Error: File is not valid UTF-8: " + path;
            return full;
        }

        const std::string kLineSuffix = "... (line truncated to " + std::to_string(kMaxLineLength) + " chars)";
        std::vector<std::string> lines;
        std::string pending;
        bool long_line = false;
        size_t line = 1;
        size_t bytes = 0;
        size_t next = 0;

        auto append = [&](const std::string& input, bool force_suffix) -> bool {
            if (line < offset) { ++line; return true; }
            if (lines.size() >= limit || bytes >= kMaxReadBytes) { next = line; return false; }
            const std::string text = (input.size() > kMaxLineLength || force_suffix)
                                         ? truncate_to(input, kMaxLineLength) + kLineSuffix
                                         : input;
            const size_t cost = text.size() + (lines.empty() ? 0 : 1);
            if (bytes + cost > kMaxReadBytes) { next = line; return false; }
            lines.push_back(text);
            bytes += cost;
            ++line;
            return true;
        };

        // One-segment lines: a segment either ends in '\n' (exactly one complete
        // line) or is a newline-free tail that continues into the next chunk.
        const auto consume = [&](const std::string& segment, std::string& error) -> bool {
            if (!segment.empty() && segment.back() == '\n') {
                std::string current = pending + segment.substr(0, segment.size() - 1);
                pending.clear();
                const bool was_long = long_line;
                long_line = false;
                if (!current.empty() && current.back() == '\r') current.pop_back();
                if (!utf8_valid(current)) {
                    error = "Error: File is not valid UTF-8: " + path;
                    return false;
                }
                return append(current, was_long);
            }
            if (long_line) return true;
            pending += segment;
            if (pending.size() > kMaxLineLength) {
                pending = truncate_to(pending, kMaxLineLength);
                if (!utf8_valid(pending)) {
                    error = "Error: File is not valid UTF-8: " + path;
                    return false;
                }
                long_line = true;
            }
            return true;
        };

        const auto consume_chunk = [&](const std::string& chunk, std::string& error) -> int {
            size_t start = 0;
            while (start < chunk.size()) {
                if (lines.size() >= limit || bytes >= kMaxReadBytes) { next = line; return 1; }
                const size_t nl = chunk.find('\n', start);
                const size_t end = nl == std::string::npos ? chunk.size() : nl + 1;
                const std::string segment = chunk.substr(start, end - start);
                if (binary_detect(segment)) {
                    error = "Error: Cannot read binary file: " + path;
                    return -1;
                }
                if (!consume(segment, error)) return error.empty() ? 1 : -1;
                start = end;
            }
            return 0;
        };

        std::string err;
        int c = consume_chunk(first, err);
        if (c < 0) return err;
        bool done = c == 1;
        while (!done) {
            std::string buf(64 * 1024, '\0');
            in.read(&buf[0], static_cast<std::streamsize>(buf.size()));
            buf.resize(static_cast<size_t>(in.gcount()));
            if (buf.empty()) break;
            c = consume_chunk(buf, err);
            if (c < 0) return err;
            done = c == 1;
        }
        if (!done && !pending.empty()) {
            std::string last = pending;
            if (!last.empty() && last.back() == '\r') last.pop_back();
            if (!utf8_valid(last)) return "Error: File is not valid UTF-8: " + path;
            append(last, long_line);
        }
        if (lines.empty() && offset != 1) return "Error: Offset " + std::to_string(offset) + " is out of range";
        std::string out = join(lines, "\n");
        if (next != 0) out += "\n... (truncated; use offset " + std::to_string(next) + " to continue)";
        return out;
    }

    static std::string list_directory(const std::string& path, size_t offset, size_t limit) {
        std::vector<std::pair<std::string, std::string>> entries;
        std::error_code ec;
        std::filesystem::directory_iterator it(path, ec), end;
        if (ec) return "Error: Cannot read directory: " + path;
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            const std::filesystem::directory_entry& entry = *it;
            const std::filesystem::file_status st = entry.status(ec);
            if (ec) continue;
            std::string suffix;
            if (std::filesystem::is_directory(st)) suffix = "/";
            else if (std::filesystem::is_regular_file(st)) suffix = "";
            else continue;
            entries.emplace_back(entry.path().filename().string(), suffix);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            const bool ad = a.second == "/";
            const bool bd = b.second == "/";
            if (ad != bd) return ad;
            return a.first < b.first;
        });

        std::vector<std::string> out;
        bool truncated = false;
        const size_t start = offset - 1;
        if (start < entries.size()) {
            for (size_t i = start; i < entries.size() && i < start + limit; ++i) {
                out.push_back(entries[i].first + entries[i].second);
            }
            truncated = start + out.size() < entries.size();
        }
        std::string result = join(out, "\n");
        if (truncated) result += "\n... (truncated; use offset " + std::to_string(offset + out.size()) + " to continue)";
        return result;
    }
};

#endif