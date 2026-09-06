# AGENTS.md — src/tool/

Builtin tools for zvmh. Header-only classes under `src/tool/`, registered into a single
`Registry`, surfaced to the model as OpenAI function definitions (`ToolDefinition`).

## Tool format (every tool)

Each tool is its own header, a `class XTool : public Tool` (`tool.h`), implementing four
members:

- `const char* name() const` — snake_case identifier used in the wire protocol (`bash`,
  `edit`, `glob`, `grep`, `ls`, `read`, `write`). Never change an emitted name.
- `const char* description() const` — see "Description format" below.
- `nlohmann::json parameters_schema() const` — JSON Schema (draft-07 subset): top-level
  `{"type": "object", "properties": {...}, "required": [...]}`. Every parameter gets a
  `type` and a `description`. `required` is a JSON array of the non-optional keys — for a
  single required param use `nlohmann::json::array({"file_path"})`.
- `std::string execute(const nlohmann::json& input)` — synchronous, returns a string that
  is the tool result verbatim. Failures MUST be returned as a `"Error: ..."` string, never
  thrown; the harness wraps execute in try/catch and reports unexpected exceptions as
  `Error:` results. Keep output bounded (grep caps at 50 matches, bash at 1MB) — a tool
  result is one message block and lands in the model context, so size matters.

## Description format (required)

The description is the model's only guide to when/how to call the tool. Use EXACTLY this
structure, in this order, as one string literal:

```
<One line: what the tool does and what it returns>

WHEN TO USE
- specific scenarios/keywords that should route here

WHEN NOT TO USE
- soft redirects — which other tool to prefer instead

DO NOT USE FOR
- hard boundaries that the tool is structurally incapable of

USAGE
- constraints the schema can't capture: defaults, caps, encoding, edge behaviors

EXAMPLES
- concrete JSON invocations for pattern matching, one per line
```

Rules:
- Keep each bullet to one line. Real facts only — the description must match the actual
  `execute()` behavior (e.g. grep is a literal substring search, not regex; -- tools that
  aren't actually capable of regex must not claim it).
- `USAGE` documents defaults, limits, and failure modes (reference constants: bash has
  `DEFAULT_TIMEOUT_MS`, `MAX_TIMEOUT_MS`, `MAX_CAPTURE_BYTES`).
- `DO NOT USE FOR` must overlap with a `WHEN NOT TO USE` redirect (say "use edit/write",
  not just "don't").
- Use raw string literals `R"(...)"` so newlines are preserved; the string must not
  contain `)"` and must not contain C++ comments.

## Wiring a new tool (all required, or the build/round-trip breaks)

1. Create the header, lowercase on disk (this repo is macOS case-insensitive — clang's
   `-Wnonportable-include-path` flags CamelCase includes).
2. Register it in `registry.h` `register_builtin_tools` via
   `registry.register_tool<XTool>();`.
3. Add the exact on-disk path to `CMakeLists.txt` (no `file(GLOB)`) — every `src/**` file
   must be listed explicitly in the `add_executable` sources.
4. Include guard: `ALL_CAPS` from the filename (`TOOL_X_H`).
5. `#include "tool.h"` for the `Tool` base + `Registry`.