#ifndef TOOL_H
#define TOOL_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <utility>
#include <nlohmann/json.hpp>
#include "../message/message.h"

class Tool {
public:
    virtual ~Tool() = default;
    virtual const char* name() const = 0;
    virtual const char* description() const = 0;
    virtual nlohmann::json parameters_schema() const = 0;
    virtual std::string execute(const nlohmann::json& input) = 0;

    ToolDefinition to_definition() const {
        return ToolDefinition{
            name(),
            description(),
            parameters_schema()
        };
    }
};

class Registry {
private:
    std::map<std::string, std::unique_ptr<Tool>> tools_;

public:
    Registry() = default;

    template<typename T, typename... Args>
    void register_tool(Args&&... args) {
        auto tool = std::make_unique<T>(std::forward<Args>(args)...);
        tools_[tool->name()] = std::move(tool);
    }

    std::vector<ToolDefinition> definitions() const {
        std::vector<ToolDefinition> defs;
        for (const auto& [name, tool] : tools_) {
            defs.push_back(tool->to_definition());
        }
        return defs;
    }

    Tool* get(const std::string& name) const {
        auto it = tools_.find(name);
        return it != tools_.end() ? it->second.get() : nullptr;
    }

    bool has(const std::string& name) const {
        return tools_.find(name) != tools_.end();
    }
};

#endif