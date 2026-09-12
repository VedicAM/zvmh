# AGENTS.md

C++17 CLI coding agent (zvmh) whose mission is to get the best possible performance out of **free models**, routed through OpenRouter (a single provider). Built around an SSE-streaming tool-use loop and a builtin tool set. No test suite, no linter, no CI — **the build is the only verification.**

## Build & run

```sh
cmake -S . -B build && cmake --build build      # deps are git submodules (vendor/json, vendor/cpr) + vendor/ftxui
export OPENROUTER_API_KEY=<key>                 # required for the auto-spawned session daemon
./build/zvmh -m "<prompt>" -C <dir>             # one-shot CLI (streams to stdout)
./build/zvmh                                    # no -m -> FTXUI fullscreen TUI
./build/zvmh --connect 127.0.0.1:5500 -m "..."  # join a shared swarms server instead
./build/zvmh --server start|stop|status         # manage the shared daemon on 5500
./build/zvmh --server start|stop|status --session   # manage the session daemon on 5501
```

- Every `src/**` file must be listed explicitly in `CMakeLists.txt` (no `file(GLOB)`). Adding a `.cpp`/`.h` without updating it breaks the build.
- OpenRouter is the only provider. All authentication goes through the `OPENROUTER_API_KEY` env var — no credential files.
- FTXUI (v7, `ScreenInteractive`/`App`) is vendored at `vendor/ftxui` via `add_subdirectory`, linked as `ftxui::ftxui`.

## Conventions that will bite you

- **All new source files must be lowercase on disk** (e.g. `src/provider/openrouter.h`, not `OpenRouter.h`) to avoid clang's `-Wnonportable-include-path` on macOS. This applies to headers and `.cpp` files alike — do not create any CamelCase files.
- Note `CMakeLists.txt` references provider files with MixedCase (`provider/Provider.h`, `provider/OpenRouter.cpp`) — that only works because macOS is case-insensitive. List new files in `CMakeLists.txt` exactly as they appear on disk (lowercase).
- Include guards are `ALL_CAPS` from the header name (`PROVIDER_H`, `MESSAGE_H`, `TOOL_H`, `TOOL_REGISTRY_H`).

## Architecture

- `src/provider/provider.h`: `Provider` ABC — pure virtual `complete(messages, tools, system, EventSink&)`, `name()`, plus `model_`/`set_model()`/`last_usage()`. The only implementation is `OpenRouter` (`src/provider/openrouter.cpp`).
- `src/provider/openrouter.cpp:5` defaults the model to `minimax/minimax-m3:free`; change it there and via the TUI `/model` command when evaluating free models.
- **The agent runtime lives on the server, not the client.** A client never constructs a `Provider`. Each connected client owns one hosted `Agent` slot on the `SwarmsServer` (`src/server/server.cpp`, `AgentSlot` in `server.h`): provider + registry + history + system context, turns streamed back over the wire. `SwarmsServer(host, port, poll_ms, api_key, solo)` — no key → swarm-only server (prompts rejected with a warning), `solo=true` → self-terminates when no client is connected and no turn is running (see `maybe_shutdown_if_idle`).
- `src/remote/remote_agent.{h,cpp}`: the thin-client `Agent` facade (`remote::RemoteAgent`). `run_turn` sends a `prompt` frame and waits on a `turn_cv_` until `turn_done`; the `ServerClient` reader thread maps frames into the query caches (`state`/`ctx`/`agents_list`) and the active `StreamSink` (`stream`). Query methods (`model()`, `tools()`, `context_window()`, `usage()`) read the cache; `set_model`/`clear_messages` send frames. `swarm()` returns a `RemotePeer` adapter so the TUI `/agents`/`/msg` still work.
- **Agent is a virtual class now** (`src/agent.h`): `run_once`/`run_turn`/`run_tui`/`attach_swarm`/`set_swarm_realtime`/query methods all `virtual`, plus a null-safe `provider_` and a virtual dtor. The base keeps the real in-process runtime (used by the server) and the remote face derives from it. `agent.attach_swarm(std::unique_ptr<swarm::SwarmPeer>, with_tools)` — `with_tools` registers `MsgTool`/`PeersTool`.
- `src/message/message.h` is the shared wire-neutral IR: `ContentBlock` = `TextBlock | ToolUseBlock | ToolResultBlock`; `Message` = role + content blocks; `StreamEvent` = `TextDelta | ToolUseStart | ToolInputDelta | ToolUseEnd | MessageEnd`.
- **Tool round-trip (the critical invariant):** the agent stores an assistant tool call as `Role::Assistant` + a `ToolUseBlock`, then each result as a **separate** `Role::User` message containing a `ToolResultBlock`. OpenRouter's converter must translate that to the OpenAI wire format, and every `ToolUseBlock.id` MUST get a response — the harness has been emitting `role: "tool"` with `tool_call_id` since a bug where `ToolResultBlock`s were silently dropped produced `400 ... tool_call_ids did not have response messages`.
- **Ordering is mandatory:** push the assistant message (with all `tool_calls`), then insert results for EVERY call, and only then call `complete()` again. Never make a second model request before responses are in history. Parallel tool calls require one response per id.
- **Parallel call indexing:** `ToolUseStartEvent`/`ToolInputDeltaEvent` carry an `index`. OpenRouter forwards the API delta's `index` (`tc.value("index", 0)` in `openrouter.cpp:166`). The agent maps streaming index → `tool_calls` slot, so don't drop it.
- The agent's tool executor (`src/agent.cpp` `run_turn`) snapshots `history_start`, wraps each `tool->execute` in try/catch so a failed tool still yields a `ToolResultBlock` ("Error: ..."), and on unexpected exception rolls history back to `history_start`. Unknown tool names produce an `Error: Unknown tool` result, not a crash. Max 10 tool steps per turn.
- `src/tool/`: header-only `Tool` ABC + `Registry`. Register every tool in `src/tool/registry.h` `register_builtin_tools`; currently bash, edit, glob, grep, ls, read, write.
- `StreamSink` (`src/agent.h`) is the output abstraction for turns: `run_turn(prompt, sink)` is the single shared tool-loop. The one-shot CLI uses a `StdoutSink` (`src/agent.cpp`); the TUI uses a sink that appends styled lines; the server uses `WireSink` (`src/server/wire_sink.h`) that turns every sink callback into a `stream` wire frame. Never bypass the sink for turn output.
- **Server-hosted tool paths:** each hosted agent gets `set_tool_cwd_base(repo)` (from the client's hello `repo`). `Agent::rewrite_tool_paths` rewrites relative `file_path`/`path`/`pattern`/`workdir` inputs against that base before `tool->execute`, because threads cannot `chdir` per agent. `bash` defaults its `workdir` to the repo. Process cwd (and the system-context task, which reads cwd) remains the daemon's own.
- `src/server/swarm_peer.h`: `SwarmPeer` is the interface the agent's tool loop and realtime path rely on. Two impls: `ServerClient` (real TCP, also the thin client's transport) and `SwarmsServer::PeerView` (fully in-process, `inject()` simulates frames so `msg`/`conflict` land in the hosted agent's `swarm_inbox_`). Both implement `register_read`/`report_write`/`send_message`/`request_peers`/`peers`/`knows_peer`.
- Wire protocol (`src/server/protocol.h`, NDJSON-over-TCP): client→ `hello`, `prompt`, `clear`, `model_set` (plus legacy `read`/`write`/`msg`/`agents`/`ping`/`bye`); server→ `hello_ack`, `state` (provider/model/tools/ctx), `stream` (one per `StreamSink` callback, event field mirrors the method name), `ctx`, `turn_done`, `conflict`, `msg`, `agents_list`, `pong`, `bye`, `shutdown`. Session port `5501` (`kSessionPort`), shared port `5500` (`kDefaultPort`).
- `--server --session` (`src/main.cpp`): operates the session daemon (`~/.zvmh/session.pid`, `~/.zvmh/session.log`, port 5501, solo). With no `--connect`, `ensure_session_backend()` probes 5501 and forks a solo daemon that inherits cwd + `OPENROUTER_API_KEY`, then the client joins as `remote::RemoteAgent`. `--connect` attaches to a shared server instead and never spawns.

## TUI

- `src/tui/tui.{h,cpp}` (`Tui::Impl` in `tui.cpp`) is an FTXUI v7 `ComponentBase` + `StreamSink` subclass. Layout: header (brand · provider · model), transcript frame (`vbox | focus` on last line auto-scrolls), status bar, and an `Input` box.
- **Threading model:** the FTXUI loop runs on the main thread. Each turn runs in a detached worker thread that calls `agent.run_turn(prompt, *impl)`. With a local `Agent` the sink is fed from that worker thread; with a `remote::RemoteAgent` it is fed from the `ServerClient` reader thread — either way the sink appends `Line`s under `mutex_` and wakes the UI with `screen.PostEvent(Event::Custom)` (thread-safe); `OnEvent` returns true for `Event::Custom` to force a redraw. `busy_` is `std::atomic<bool>` and gates new turns.
- Slash commands (parsed with `rfind(cmd, 0) == 0`): `/model <name>` (persists via `set_model()` for next request), `/tools [name]`, `/clear`, `/help`, `/exit`, `/quit`. Enter is intercepted on the input via `CatchEvent` to submit.
- Gotcha: `StreamSink::text_delta` is named that because the name `text` would shadow FTXUI's `text()` DOM function inside the `Impl` scope. **Live render:** each `text_delta` mutates the in-place `lines_[live_index_]` line and posts a Custom event — so tokens redraw as they arrive; `live_index_` (a `size_t`, `kNone = -1`) points at the current live line, and `flush_live_text()` finalizes it (reset to `kNone`) at structural events/end of turn.
- The TUI needs a real terminal; it reads stdin from `/dev/tty` (FTXUI `HandlePipedInput` default) and uses the alternate screen buffer, so don't test it with plain pipes — use `pty.fork()` from Python so the TUI gets a controlling tty, or `HandlePipedInput()`.

## Streaming architecture

- **Tokens stream live end-to-end (no full-body buffer).** `Provider::complete(messages, tools, system, EventSink&)` invokes an `EventSink::on_event(StreamEvent)` (`src/message/message.h`) as chunks arrive; the old `EventStream` return is gone.
- `OpenRouter::complete` (`src/provider/openrouter.cpp`) uses a `cpr::Session` + `SetWriteCallback` running a per-WriteCallback SSE line splitter feeding `emit_sse_data()`. `emit_sse_data` is the streaming analog of the old parse loop (shared `current_tool_id`/`current_tool_name` state for `ToolUseEnd`). `--data:` chunks are forwarded immediately; the body is never accumulated (only a capped ~16 KB `error_tail` kept for error messages).
- Non-200 paths throw after `session.Post()` using the buffered `error_tail`; cpr transport errors via `response.error`. The response object itself is dropped after `Post` returns — it never holds the SSE body.
- `Agent::run_turn` (`src/agent.cpp`) wires an internal `TurnBridge : EventSink` into `provider_->complete` that accumulates `response_text` / `tool_calls` / `tool_index` (via `input_json +=`) AND forwards to the `StreamSink` in the same call, keeping the tool-loop invariant (assistant msg → tool results → re-request), with `history_start` rollback on exception.
- Mock SSE server for testing streaming/memory: `/var/folders/5n/4z6n3mgd3hqgbkb5tky611mw0000gn/T/opencode/mock_llm.py` (+ `mock_llm_slow.py` dribbling a word every 0.4 s; CLI/TUI streaming verified against it — words appear at ≈0.02/0.42/0.83 s).