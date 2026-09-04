#ifndef TOOL_REGISTRY_H
#define TOOL_REGISTRY_H

#include "tool.h"
#include "bash.h"
#include "read.h"
#include "write.h"
#include "edit.h"
#include "glob.h"
#include "grep.h"
#include "ls.h"

inline void register_builtin_tools(Registry& registry) {
    registry.register_tool<BashTool>();
    registry.register_tool<ReadTool>();
    registry.register_tool<WriteTool>();
    registry.register_tool<EditTool>();
    registry.register_tool<GlobTool>();
    registry.register_tool<GrepTool>();
    registry.register_tool<LsTool>();
}

#endif