# AGENTS.md

C++17 CLI coding agent (zvmh) with pluggable LLM providers (Claude, Codex, OpenRouter), an SSE-streaming tool-use loop, and a builtin tool set. No test suite, no linter, no CI — **the build is the only verification.**

## Build & run

```sh
cmake -S . -B build && cmake --build build      # deps are git submodules (vendor/json, vendor/cpr)
./build/zvmh -p <claude|codex|openrouter|auto> -m "<prompt>" -C <dir>
./build/zvmh                                    # no -m -> REPL
```

- Every `src/**` file must be listed explicitly in `CMakeLists.txt` (no `file(GLOB)`). Adding a `.cpp`/`.h` without updating it breaks the build.
- `-p auto` falls back Claude → Codex → OpenRouter. OpenRouter requires `OPENROUTER_API_KEY` env var.

## Conventions that will bite you

- **Provider headers are lowercase on disk** (`src/provider/openrouter.h`, `claude.h`, `codex.h`, `provider.h`) to avoid clang's `-Wnonportable-include-path` on macOS. Keep them lowercase. Note `CMakeLists.txt` lists them with MixedCase (`provider/Provider.h`) — that only works because macOS is case-insensitive.
- Auth headers use CamelCase (`src/auth/ClaudeAuth.h`, `CodexAuth.h`); includes must match exactly.
- Include guards are `ALL_CAPS` from the header name (`PROVIDER_H`, `MESSAGE_H`, `TOOL_H`, `TOOL_REGISTRY_H`).

## Architecture

- `src/provider/provider.h`: `Provider` ABC — pure virtual `complete(messages, tools, system)` (returns `EventStream`), `name()`, plus `model_`/`set_model()`/`last_usage()`.
- `src/message/message.h` is the shared wire-neutral IR: `ContentBlock` = `TextBlock | ToolUseBlock | ToolResultBlock`; `Message` = role + content blocks; `StreamEvent` = `TextDelta | ToolUseStart | ToolInputDelta | ToolUseEnd | MessageEnd`. `EventStream` is a fully-buffered `std::vector` (all chunks arrive before `complete()` returns).
- **Tool round-trip (the critical invariant):** the agent stores an assistant tool call as `Role::Assistant` + a `ToolUseBlock`, then each result as a **separate** `Role::User` message containing a `ToolResultBlock`. Every provider's message converter must translate that to its native wire format and every `ToolUseBlock.id` MUST get a response message, or OpenAI-family APIs return `400 ... tool_call_ids did not have response messages`:
  - OpenRouter / Codex: emit `{"role": "tool", "tool_call_id": id, "content": ...}`. (OpenRouter once silently dropped `ToolResultBlock`s — the source of that exact 400.)
  - Claude: emit a `type: "tool_result"` content block with `tool_use_id` inside the user message.
- **Ordering is mandatory:** push the assistant message (with all `tool_calls`), then insert results for EVERY call, and only then call `complete()` again. Never make a second model request before responses are in history. Parallel tool calls require one response per id.
- **Parallel call indexing:** `ToolUseStartEvent`/`ToolInputDeltaEvent` carry an `index`. OpenAI-family providers must forward the API delta's `index` (`tc.value("index", 0)`); Claude emits `0`. The agent maps streaming index → `tool_calls` slot, so don't drop it.
- The agent's tool executor (`src/agent.cpp` `run_turn`) snapshots `history_start`, wraps each `tool->execute` in try/catch so a failed tool still yields a `ToolResultBlock` ("Error: ..."), and on unexpected exception rolls history back to `history_start`. Unknown tool names produce an `Error: Unknown tool` result, not a crash. Max 10 tool steps per turn.
- `src/tool/`: header-only `Tool` ABC + `Registry`. Register every tool in `src/tool/registry.h` `register_builtin_tools`; currently bash, edit, glob, grep, ls, read, write.

## Auth & API details

- Claude: `~/.claude/.credentials.json` → `claudeAiOauth.accessToken` via `src/auth/ClaudeAuth.cpp`; throws if `expiresAt` (ms) is past — hints to run `claude` and `/login`. Uses `x-api-key`, `anthropic-version: 2023-06-01`, `anthropic-beta: claude-code-20250219,oauth-2025-04-20,interleaved-thinking-2025-05-14` headers.
- Codex: `~/.codex/auth.json` → `tokens.access_token` (or `OPENAI_API_KEY` key), Bearer header. Codex requests set `"stream_options": {"include_usage": true}` so usage arrives in the stream.
- Token usage: providers record `last_usage_`; Claude reads it from `message_start`/`message_delta` SSE events.

## REPL

Slash commands (parsed with `rfind(cmd, 0) == 0`): `/model <name>` (persists via `set_model()` for next request), `/tools [name]`, `/clear`, `/help`, `/exit`, `/quit`. The REPL prints `[provider · model]`, tool calls, and a token-usage summary per turn.