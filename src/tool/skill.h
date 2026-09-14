#ifndef TOOL_SKILL_H
#define TOOL_SKILL_H

#include "tool.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Skills: directories named after the skill, each holding a SKILL.md. Global
// skills live in <HOME>/.zvmh/skills, project skills in <cwd>/.zvmh/skills;
// project skills override global ones on a name conflict (mirroring opencode's
// scope precedence). The SkillTool loads a skill by name and injects its
// instructions plus a sampled file list into the conversation.

namespace skills {

struct Info {
    std::string name;       // skill name, equals the directory name
    std::string location;   // absolute path to the skill file (SKILL.md)
    std::string directory;  // absolute path of the skill's directory
    std::string content;    // raw contents of the skill file
};

inline constexpr size_t kFileLimit = 10;

inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Discovers every available skill. Later roots override earlier ones, so
// project <cwd>/.zvmh/skills wins over global <HOME>/.zvmh/skills. The result
// is sorted by name and only skills with loadable (non-empty) content appear.
inline std::vector<Info> list() {
    const char* home = getenv("HOME");
    std::vector<std::filesystem::path> roots;
    if (home && *home) roots.push_back(std::filesystem::path(home) / ".zvmh" / "skills");
    roots.push_back(std::filesystem::current_path() / ".zvmh" / "skills");

    std::map<std::string, Info> by_name;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory()) continue;
            const std::filesystem::path file = entry.path() / "SKILL.md";
            std::error_code fec;
            if (!std::filesystem::is_regular_file(file, fec)) continue;
            Info info;
            info.name = entry.path().filename().string();
            info.directory = std::filesystem::absolute(entry.path()).string();
            info.location = std::filesystem::absolute(file).string();
            info.content = read_text(file);
            if (info.content.empty()) continue;
            by_name[info.name] = std::move(info);
        }
    }

    std::vector<Info> infos;
    infos.reserve(by_name.size());
    for (auto& [name, info] : by_name) infos.push_back(std::move(info));
    std::sort(infos.begin(), infos.end(),
              [](const Info& a, const Info& b) { return a.name < b.name; });
    return infos;
}

inline std::optional<Info> find(const std::string& name) {
    for (const Info& info : list()) {
        if (info.name == name) return info;
    }
    return std::nullopt;
}

// Files inside a skill directory, excluding any file named SKILL.md, sorted and
// capped at kFileLimit. directory must be absolute so the paths match the
// reference's absolute: true glob.
inline std::vector<std::string> sample_files(const std::string& directory) {
    std::vector<std::string> files;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        directory, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        std::error_code iter_ec;
        if (it->is_regular_file(iter_ec) && it->path().filename() != "SKILL.md") {
            files.push_back(it->path().string());
        }
        it.increment(ec);
        if (ec) break;
    }
    std::sort(files.begin(), files.end());
    if (files.size() > kFileLimit) files.resize(kFileLimit);
    return files;
}

inline std::string to_model_output(const Info& skill, const std::vector<std::string>& files) {
    std::string out;
    out += "<skill_content name=\"" + skill.name + "\">\n";
    out += "# Skill: " + skill.name + "\n\n";
    out += trim(skill.content) + "\n\n";
    out += "Base directory for this skill: " + skill.directory + "\n";
    out += "Relative paths in this skill (e.g., scripts/, reference/) are relative to this base directory.\n";
    out += "Note: file list is sampled.\n\n";
    out += "<skill_files>\n";
    for (const std::string& file : files) {
        out += "<file>" + file + "</file>\n";
    }
    out += "</skill_files>\n";
    out += "</skill_content>";
    return out;
}

}  // namespace skills

class SkillTool : public Tool {
public:
    const char* name() const override { return "skill"; }

    const char* description() const override {
        return R"(Load a specialized skill when the task at hand matches one of the available skills in the system context.

WHEN TO USE
- the system context lists an available skill whose purpose matches the current task
- the task references a skill by name or asks for that skill's workflow and instructions

WHEN NOT TO USE
- no available skill covers the task: answer directly or use the ordinary tool set
- individual skill files need targeted reading first: use read on the specific path
- a skill script must actually run: use bash on the skill's directory

DO NOT USE FOR
- loading files outside a skill directory: use read/glob instead
- running a skill's scripts: the tool only injects text, it never executes anything

USAGE
- name: required, must match exactly one of the available skills listed in the system context
- a skill is a directory named <name> containing SKILL.md, discovered in <HOME>/.zvmh/skills and <cwd>/.zvmh/skills
- project-local <cwd>/.zvmh/skills overrides the global <HOME>/.zvmh/skills on a name conflict
- skills whose SKILL.md is empty are omitted from the available list and cannot be loaded
- returns the skill content plus up to 10 file paths sampled from the skill directory (SKILL.md excluded)

EXAMPLES
- {"name": "my-custom-skill"}
- {"name": "review-workflow"})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"name", {
                    {"type", "string"},
                    {"description", "The name of the skill from the available skills list"}
                }}
            }},
            {"required", nlohmann::json::array({"name"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string name = input.value("name", "");
        std::optional<skills::Info> skill = skills::find(name);
        if (!skill) return "Error: Unable to load skill " + name;

        const bool is_skill_md = std::filesystem::path(skill->location).filename() == "SKILL.md";
        const std::vector<std::string> files =
            is_skill_md ? skills::sample_files(skill->directory) : std::vector<std::string>{};
        return skills::to_model_output(*skill, files);
    }
};

#endif