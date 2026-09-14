#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#include "tool.h"
#include "apply_patch.h"
#include "bash.h"
#include "read.h"
#include "write.h"
#include "edit.h"
#include "glob.h"
#include "grep.h"
#include "ls.h"
#include "skill.h"
#include "websearch.h"
#include "webfetch.h"

inline void register_builtin_tools(Registry& registry) {
    registry.register_tool<ApplyPatchTool>();
    registry.register_tool<BashTool>();
    registry.register_tool<ReadTool>();
    registry.register_tool<WriteTool>();
    registry.register_tool<EditTool>();
    registry.register_tool<GlobTool>();
    registry.register_tool<GrepTool>();
    registry.register_tool<LsTool>();
    registry.register_tool<SkillTool>();
    registry.register_tool<WebSearchTool>();
    registry.register_tool<WebFetchTool>();
}

#endif