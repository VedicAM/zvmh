#include "tui.h"
#include "prompt_editor.h"
#include "markdown.h"

#include <limits>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/terminal.hpp>

#include <nlohmann/json.hpp>
#include "../agent.h"

using namespace ftxui;

namespace {

using Line = markdown::Line;
using Span = markdown::Span;

std::string trim_string(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";

    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// A row counts as blank when it carries no visible glyphs.
bool line_is_blank(const Line& line) {
    for (const Span& span : line.spans) {
        if (span.text.find_first_not_of(" \t") != std::string::npos)
            return false;
    }

    return true;
}

// Drop blank rows at the edges of a block so that padding can be applied
// uniformly, regardless of what the producer emitted.
//
// Markdown rendering in particular tends to leave leading/trailing gaps
// that differ from block to block.
void trim_blank_edges(std::vector<Line>& block) {
    size_t begin = 0;
    size_t end = block.size();

    while (begin < end && line_is_blank(block[begin]))
        ++begin;

    while (end > begin && line_is_blank(block[end - 1]))
        --end;

    if (begin != 0 || end != block.size()) {
        block = std::vector<Line>(
            block.begin() + static_cast<std::ptrdiff_t>(begin),
            block.begin() + static_cast<std::ptrdiff_t>(end));
    }
}

Element span_element(const Span& span) {
    Element element = text(span.text);

    if (span.bold)
        element = element | bold;

    if (span.dim)
        element = element | dim;

    if (span.italic)
        element = element | italic;

    if (span.color != Color::Default)
        element = element | color(span.color);

    return element;
}

// Hard-wrap a multi-span line at `limit` display columns.
//
// Each returned Line represents exactly one physical terminal row.
std::vector<Line> wrap_line(const Line& line, int limit) {
    using prompt_editor_detail::pe_glyph_len;
    using prompt_editor_detail::pe_glyph_width;

    if (limit <= 0)
        limit = 100;

    // Keep one cell of breathing room so the final glyph isn't clipped
    // by the enclosing FTXUI layout/border.
    const int safe_limit = std::max(1, limit - 1);

    std::vector<Line> rows;
    Line row;
    int width = 0;

    auto append_glyph = [&](const std::string& glyph, const Span& style) {
        if (!row.spans.empty()) {
            Span& last = row.spans.back();

            if (last.color == style.color &&
                last.bold == style.bold &&
                last.dim == style.dim &&
                last.italic == style.italic) {

                last.text += glyph;
                return;
            }
        }

        Span new_span = style;
        new_span.text = glyph;
        row.spans.push_back(std::move(new_span));
    };

    auto push_row = [&]() {
        if (!row.spans.empty()) {
            rows.push_back(std::move(row));
            row = Line{};
            width = 0;
        }
    };

    auto add_word =
        [&](const std::vector<std::pair<std::string, Span>>& glyphs) {

            int word_width = 0;

            for (const auto& [glyph, style] : glyphs)
                word_width += pe_glyph_width(glyph, 0);

            const bool has_content = width > 0;

            // Word fits on the current line.
            if (has_content &&
                width + 1 + word_width <= safe_limit) {

                Span space = glyphs.front().second;
                append_glyph(" ", space);
                ++width;

                for (const auto& [glyph, style] : glyphs) {
                    const int glyph_width =
                        pe_glyph_width(glyph, 0);

                    append_glyph(glyph, style);
                    width += glyph_width;
                }

                return;
            }

            // Start the word on a new line.
            if (has_content)
                push_row();

            // Hard-wrap words that are wider than the available width.
            if (word_width > safe_limit) {
                for (const auto& [glyph, style] : glyphs) {
                    const int glyph_width =
                        pe_glyph_width(glyph, 0);

                    if (width > 0 &&
                        width + glyph_width > safe_limit) {
                        push_row();
                    }

                    append_glyph(glyph, style);
                    width += glyph_width;
                }

                return;
            }

            // Word fits on an empty line.
            for (const auto& [glyph, style] : glyphs) {
                const int glyph_width =
                    pe_glyph_width(glyph, 0);

                append_glyph(glyph, style);
                width += glyph_width;
            }
        };

    std::vector<std::pair<std::string, Span>> word;

    auto flush_word = [&]() {
        if (!word.empty()) {
            add_word(word);
            word.clear();
        }
    };

    for (const Span& span : line.spans) {
        size_t i = 0;

        while (i < span.text.size()) {
            const size_t glen = pe_glyph_len(span.text, i);
            const std::string glyph =
                span.text.substr(i, glen);

            if (glyph == " " || glyph == "\t") {
                flush_word();
            } else {
                word.emplace_back(glyph, span);
            }

            i += glen;
        }
    }

    flush_word();
    push_row();

    if (rows.empty())
        rows.push_back(Line{""});

    return rows;
}

// Render a single already-wrapped physical row.
Element physical_line_element(const Line& line) {
    if (line.spans.empty())
        return text("");

    Elements cells;
    cells.reserve(line.spans.size());

    for (const auto& span : line.spans)
        cells.push_back(span_element(span));

    return hbox(std::move(cells));
}

// Render a logical line.
//
// This is retained for the case where the line only contains one span,
// because paragraph() gives better handling of ordinary text.
Element line_element(const Line& line, int width_limit) {
    if (line.spans.empty())
        return text("");

    if (line.spans.size() == 1) {
        const Span& span = line.spans[0];

        Element element = paragraph(span.text);

        if (span.bold)
            element = element | bold;

        if (span.dim)
            element = element | dim;

        if (span.italic)
            element = element | italic;

        if (span.color != Color::Default)
            element = element | color(span.color);

        return element;
    }

    auto rows = wrap_line(line, width_limit);

    Elements cells;
    cells.reserve(rows.size());

    for (const auto& row : rows)
        cells.push_back(physical_line_element(row));

    return vbox(std::move(cells));
}

}  // namespace

struct Tui::Impl : public ComponentBase, public StreamSink {
    Agent& agent;

    ScreenInteractive screen = ScreenInteractive::Fullscreen();

    std::atomic<bool> busy_{false};
    std::atomic<int> ctx_window_{0};
    std::atomic<uint32_t> anim_phase_{0};

    std::mutex mutex_;

    // Logical markdown lines.
    std::vector<Line> lines_;

    // Streaming/live response state.
    size_t live_index_ = kNone;
    size_t live_count_ = 0;
    std::string live_text_;

    std::string last_summary_;
    std::string input_;

    // Prompt history for up/down recall from the input area.
    std::vector<std::string> prompt_history_;
    size_t history_pos_ = kNone;
    std::string history_pending_;

    // Command autocomplete state.
    std::vector<std::string> completion_choices_;
    std::string completion_key_;
    size_t completion_index_ = 0;


    bool follow_bottom_ = true;
    size_t view_row_ = 0;

    // Cached physical rows generated during rendering.
    //
    // These are rebuilt when the transcript width changes or the content
    // changes.
    std::vector<Line> physical_lines_;

    int physical_width_ = -1;

    Box transcript_box_;

    static constexpr size_t kNone = static_cast<size_t>(-1);

    Component layout_;
    Component input_component_;

    int input_cursor_ = 0;

    Box prompt_box_;

    explicit Impl(Agent& agent_ref)
        : agent(agent_ref) {}

    int transcript_width() const {
        const int width =
            transcript_box_.x_max > transcript_box_.x_min
                ? transcript_box_.x_max - transcript_box_.x_min + 1
                : 100;

        return std::max(1, width);
    }

    int page_height() const {
        const int h =
            transcript_box_.y_max - transcript_box_.y_min + 1;

        return h > 1 ? h : 15;
    }

    // Rebuild the physical representation of the transcript.
    //
    // Every entry in physical_lines_ corresponds to one terminal row.
    void rebuild_physical_lines_locked(int width) {
        physical_lines_.clear();

        if (width <= 0)
            width = 100;

        for (const auto& line : lines_) {
            auto wrapped = wrap_line(line, width);

            for (auto& row : wrapped)
                physical_lines_.push_back(std::move(row));
        }

        physical_width_ = width;
    }

    void ensure_physical_lines_locked(int width) {
        if (physical_width_ != width)
            rebuild_physical_lines_locked(width);
    }

    size_t max_view_row_locked() const {
        const size_t height =
            static_cast<size_t>(std::max(1, page_height()));

        if (physical_lines_.size() <= height)
            return 0;

        return physical_lines_.size() - height;
    }

    void snap_to_bottom_locked() {
        view_row_ = max_view_row_locked();
        follow_bottom_ = true;
    }


    // Caller must hold mutex_.
    void ensure_leading_gap_locked() {
        // Note the empty case comes first: an empty transcript still
        // gets a gap, so the very first message is padded on top the
        // same as every message after it.
        if (lines_.empty() || !line_is_blank(lines_.back()))
            lines_.push_back(Line{""});
    }

    // Caller must hold mutex_.
    void mark_dirty_locked() {
        // Content changed, so the physical layout must be rebuilt.
        physical_width_ = -1;

        if (follow_bottom_) {
            // We cannot know the exact physical height until the next
            // layout pass. Keep the viewport at the bottom logically;
            // transcript_element() will clamp it correctly.
            view_row_ = std::numeric_limits<size_t>::max();
        }
    }

    void append_block(std::vector<Line> block) {
        trim_blank_edges(block);

        if (block.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(mutex_);

            ensure_leading_gap_locked();

            for (auto& line : block)
                lines_.push_back(std::move(line));

            lines_.push_back(Line{""});

            mark_dirty_locked();
        }

        screen.PostEvent(Event::Custom);
    }

    void append(const Line& line) {
        append_block(std::vector<Line>{line});
    }

    // Replace the live region with freshly rendered Markdown lines.
    //
    // The live block is padded on the same terms as any other message.
    //
    // Caller must hold mutex_.
    void set_live_lines_locked(std::vector<Line> rendered) {
        trim_blank_edges(rendered);

        if (rendered.empty())
            return;

        // Trailing gap, so the block matches every other message.
        rendered.push_back(Line{""});

        if (live_index_ == kNone) {
            ensure_leading_gap_locked();

            live_index_ = lines_.size();

            lines_.insert(
                lines_.end(),
                rendered.begin(),
                rendered.end());

            live_count_ = rendered.size();
        } else {
            const size_t end =
                live_index_ + live_count_;

            lines_.erase(
                lines_.begin() +
                    static_cast<std::ptrdiff_t>(live_index_),
                lines_.begin() +
                    static_cast<std::ptrdiff_t>(
                        std::min(end, lines_.size())));

            lines_.insert(
                lines_.begin() +
                    static_cast<std::ptrdiff_t>(live_index_),
                rendered.begin(),
                rendered.end());

            live_count_ = rendered.size();
        }

        mark_dirty_locked();
    }

    void set_live_lines(std::vector<Line> rendered) {
        std::lock_guard<std::mutex> lock(mutex_);
        set_live_lines_locked(std::move(rendered));
        screen.PostEvent(Event::Custom);
    }

    void flush_live_text() {
        std::lock_guard<std::mutex> lock(mutex_);

        live_text_.clear();
        live_index_ = kNone;
        live_count_ = 0;

        mark_dirty_locked();
    }

    // Live swarm line rendered from the server reader thread. Thread-safe:
    // appends under mutex_ and wakes the UI with a Custom event.
    void swarm_live(const std::string& swarm_text) {
        flush_live_text();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            ensure_leading_gap_locked();
            lines_.push_back(Line{
                swarm_text,
                Color::Magenta,
                false,
                false
            });
            lines_.push_back(Line{""});

            mark_dirty_locked();
        }

        screen.PostEvent(Event::Custom);
    }

    void header(
        const std::string& provider,
        const std::string& model) override {

        flush_live_text();

        append(Line{
            provider + " · " + model,
            Color::GrayLight,
            false,
            false
        });
    }

    void text_delta(const std::string& text_delta_value) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            live_text_ += text_delta_value;

            set_live_lines_locked(markdown::render(live_text_));
        }

        screen.PostEvent(Event::Custom);
    }

    void tool_start(const std::string& name) override {
        flush_live_text();

        append(Line{
            "→ " + name,
            Color::Cyan,
            true,
            false
        });
    }

    void tool_call(
        const std::string& name,
        const nlohmann::json& args) override {

        (void)args;

        append(Line{
            "▸ " + name,
            Color::Yellow,
            true,
            false
        });
    }

    void tool_result(
        const std::string& result,
        bool is_error) override {

        append(Line{
            "↩ " + result,
            is_error ? Color::RedLight : Color::Green,
            false,
            false
        });
    }

    void warning(const std::string& text) override {
        flush_live_text();

        append(Line{
            "✖ " + text,
            Color::RedLight,
            false,
            false
        });
    }

    void handle_command(const std::string& cmd) {
        if (cmd.rfind("/model", 0) == 0) {
            std::string arg = trim_string(cmd.substr(6));

            if (arg.empty()) {
                append_block({
                    Line{
                        "current model: " + agent.model(),
                        Color::Cyan,
                        true,
                        false
                    },
                    Line{
                        "set a new model with: /model <name>",
                        Color::GrayLight,
                        true,
                        false
                    },
                });
            } else {
                agent.set_model(arg);
                ctx_window_.store(0);

                refresh_context();

                append(Line{
                    "model set to: " + agent.model(),
                    Color::Cyan,
                    true,
                    false
                });
            }

        } else if (
            cmd == "/tools" ||
            cmd.rfind("/tools ", 0) == 0) {

            std::string arg = trim_string(cmd.substr(6));

            std::vector<Line> block;

            block.push_back(Line{
                "registered tools:",
                Color::Cyan,
                true,
                false
            });

            auto defs = agent.tools();

            if (arg.empty()) {
                for (const auto& def : defs) {
                    block.push_back(Line{
                        "  - " + def.name + "  " + def.description,
                        Color::Default,
                        true,
                        false
                    });
                }
            } else {
                Tool* tool = agent.tool(arg);

                if (tool) {
                    block.push_back(Line{
                        "  - " +
                            std::string(tool->name()) +
                            "  " +
                            tool->description(),
                        Color::Default,
                        true,
                        false
                    });

                    block.push_back(Line{
                        "      schema: " +
                            tool->parameters_schema().dump(),
                        Color::GrayLight,
                        false,
                        false
                    });
                } else {
                    block.push_back(Line{
                        "  unknown tool: '" + arg + "'",
                        Color::RedLight,
                        false,
                        false
                    });
                }
            }

            append_block(std::move(block));

        } else if (cmd == "/clear") {
            agent.clear_messages();

            {
                std::lock_guard<std::mutex> lock(mutex_);

                lines_.clear();

                live_text_.clear();
                live_index_ = kNone;
                live_count_ = 0;

                physical_lines_.clear();
                physical_width_ = -1;

                follow_bottom_ = true;
                view_row_ = 0;
            }

            append(Line{
                "conversation cleared.",
                Color::GrayLight,
                false,
                false
            });

        } else if (cmd == "/agents") {
            if (!agent.swarm_connected()) {
                append(Line{
                    "not connected to a swarm server (start one with `zvmh --server start`)",
                    Color::RedLight,
                    false,
                    false
                });
            } else {
                std::vector<nlohmann::json> peers = agent.swarm()->peers();

                std::vector<Line> block;
                block.push_back(Line{
                    "swarm agents:",
                    Color::Cyan,
                    true,
                    false
                });

                if (peers.empty()) {
                    block.push_back(Line{
                        "  (none connected yet)",
                        Color::GrayLight,
                        false,
                        false
                    });
                } else {
                    for (const auto& p : peers) {
                        block.push_back(Line{
                            "  " + p.value("id", "?") + "  " +
                                p.value("name", "?") + "  (" +
                                p.value("repo", "?") + ")",
                            Color::Default,
                            false,
                            false
                        });
                    }
                }

                append_block(std::move(block));
                agent.swarm()->request_peers();
            }

        } else if (cmd == "/msg" || cmd.rfind("/msg ", 0) == 0) {
            std::string rest = trim_string(cmd.substr(4));
            size_t sp = rest.find(' ');

            if (!agent.swarm_connected()) {
                append(Line{
                    "not connected to a swarm server (start one with `zvmh --server start`)",
                    Color::RedLight,
                    false,
                    false
                });
            } else if (sp == std::string::npos) {
                append(Line{
                    "usage: /msg <all|repo|<agent id>> <text>",
                    Color::Yellow,
                    false,
                    false
                });
            } else {
                std::string to = trim_string(rest.substr(0, sp));
                std::string text = trim_string(rest.substr(sp + 1));

                if (to != "all" && to != "repo" && !agent.swarm()->knows_peer(to)) {
                    append(Line{
                        "no peer with id '" + to + "' is connected (use /agents)",
                        Color::RedLight,
                        false,
                        false
                    });
                } else if (text.empty()) {
                    append(Line{
                        "usage: /msg <all|repo|<agent id>> <text>",
                        Color::Yellow,
                        false,
                        false
                    });
                } else {
                    agent.swarm()->send_message(to, text);
                    append(Line{
                        "msg sent to " + to,
                        Color::GrayLight,
                        false,
                        false
                    });
                }
            }

        } else if (cmd == "/help") {
            append_block({
                Line{
                    "/model             show current model",
                    Color::Default,
                    false,
                    false
                },
                Line{
                    "/model <name>      switch model (e.g. /model minimax/minimax-m3:free)",
                    Color::Default,
                    false,
                    false
                },
                Line{
                    "/tools             list tools",
                    Color::Default,
                    false,
                    false
                },
                Line{
                    "/tools <name>      show tool schema",
                    Color::Default,
                    false,
                    false
                },
                Line{
                    "/clear             clear conversation history",
                    Color::Default,
                    false,
                    false
                },
                Line{
                    "/agents            list swarm-connected agents",
                    Color::Default,
                    false,
                    false
                },
                Line{
                    "/msg <to> <text>   message a peer (all, repo, or an agent id)",
                    Color::Default,
                    false,
                    false
                },
                Line{
                    "/exit, /quit       leave the TUI",
                    Color::Default,
                    false,
                    false
                },
                Line{
                    "/help              show this help",
                    Color::Default,
                    false,
                    false
                },
            });

        } else if (
            cmd == "/exit" ||
            cmd == "/quit") {

            screen.Exit();

        } else {
            append(Line{
                "  unknown command: '" +
                    cmd +
                    "'   (try /help)",
                Color::RedLight,
                false,
                false
            });
        }
    }

    void refresh_context() {
        std::thread([this] {
            int ctx = agent.context_window();

            ctx_window_.store(ctx);

            screen.PostEvent(Event::Custom);
        }).detach();
    }

    // The command word currently being typed (i.e. the "/xxx" prefix).
    // Returns empty when the input is not a slash command.
    std::string command_prefix() const {
        const std::string t = trim_string(input_);
        if (t.empty() || t[0] != '/')
            return "";

        size_t sp = t.find(' ');
        return sp == std::string::npos ? t : t.substr(0, sp);
    }

    // All registered slash commands.
    static const std::vector<std::string>& command_choices() {
        static const std::vector<std::string> choices = {
            "/model", "/tools", "/clear", "/agents", "/msg", "/help", "/exit", "/quit",
        };
        return choices;
    }

    // One-line description shown next to each command in the popup.
    static const char* command_desc(const std::string& name) {
        static const std::unordered_map<std::string, const char*> desc = {
            {"/model", "show or set the current model"},
            {"/tools", "list registered tools & schemas"},
            {"/clear", "clear conversation history"},
            {"/agents", "list swarm-connected agents"},
            {"/msg",    "message an agent (all, repo, or id)"},
            {"/help",  "show this help"},
            {"/exit",  "leave the TUI"},
            {"/quit",  "leave the TUI"},
        };

        auto it = desc.find(name);
        return it == desc.end() ? "" : it->second;
    }

    // Suggestions matching the command word or its description.
    std::vector<std::string> current_suggestions() const {
        const std::string prefix = command_prefix();
        if (prefix.empty())
            return {};

        std::vector<std::string> out;

        for (const auto& choice : command_choices()) {
            // Exact command-word prefix match.
            if (choice.rfind(prefix, 0) == 0) {
                out.push_back(choice);
                continue;
            }

            // Case-insensitive substring match against the rest of the
            // command word and its description.
            const std::string body = prefix.substr(1);
            if (body.empty())
                continue;

            auto lower = [](std::string s) {
                for (char& c : s)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };

            const std::string lb = lower(body);

            std::string name = lower(choice);
            std::string desc = lower(command_desc(choice));

            if (name.find(lb) != std::string::npos ||
                desc.find(lb) != std::string::npos) {
                out.push_back(choice);
            }
        }
        return out;
    }

    // Recompute the active completion list, resetting the selected index when
    // the command word changes.
    void refresh_suggestions() {
        std::vector<std::string> fresh = current_suggestions();

        if (!fresh.empty() && command_prefix() != completion_key_) {
            completion_key_ = command_prefix();
            completion_index_ = 0;
        }

        completion_choices_ = std::move(fresh);
    }

    void accept_suggestion() {
        if (completion_choices_.empty())
            return;

        if (completion_index_ >= completion_choices_.size())
            completion_index_ = completion_choices_.size() - 1;

        input_ = completion_choices_[completion_index_];
        input_cursor_ = static_cast<int>(input_.size());

        completion_key_ = input_;
        completion_index_ = 0;
        completion_choices_ = current_suggestions();

        screen.PostEvent(Event::Custom);
    }

    bool cycle_suggestion(int dir) {
        refresh_suggestions();
        if (completion_choices_.empty())
            return false;

        const size_t n = completion_choices_.size();
        completion_index_ =
            (completion_index_ + n + static_cast<size_t>(dir)) % n;

        screen.PostEvent(Event::Custom);
        return true;
    }

    Element suggestions_element() {
        refresh_suggestions();

        if (completion_choices_.empty())
            return text("");

        Elements items;
        for (size_t i = 0; i < completion_choices_.size(); ++i) {
            const bool sel = (i == completion_index_);
            const std::string& name = completion_choices_[i];
            const char* desc = command_desc(name);

            Color text_color =
                sel ? Color::CyanLight : Color::GrayLight;

            items.push_back(
                hbox({
                    text(std::string(sel ? "▸ " : "  ") + name)
                        | color(text_color) | bold,

                    text("  " + std::string(desc))
                        | dim
                        | color(text_color),
                }));
        }

        return vbox(std::move(items))
            | clear_under
            | borderRounded
            | color(Color::GrayDark);
    }

    void run_turn_async(const std::string& prompt) {
        busy_.store(true);
        anim_phase_.store(0);

        screen.PostEvent(Event::Custom);

        std::thread([this] {
            animate_while_busy();
        }).detach();

        std::thread([this, prompt] {
            int rc = agent.run_turn(prompt, *this);
            (void)rc;

            flush_live_text();

            busy_.store(false);

            screen.PostEvent(Event::Custom);
        }).detach();
    }

    void animate_while_busy() {
        while (busy_.load()) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(50));

            if (!busy_.load())
                break;

            anim_phase_.fetch_add(1);

            screen.PostEvent(Event::Custom);
        }
    }

    void record_history(const std::string& prompt) {
        if (prompt_history_.empty() ||
            prompt_history_.back() != prompt) {

            prompt_history_.push_back(prompt);

            if (prompt_history_.size() > 200)
                prompt_history_.erase(prompt_history_.begin());
        }

        history_pos_ = kNone;
        history_pending_.clear();
    }

    void history_previous() {
        if (prompt_history_.empty())
            return;

        if (history_pos_ == kNone) {
            history_pending_ = input_;
            history_pos_ = prompt_history_.size() - 1;
        } else if (history_pos_ > 0) {
            --history_pos_;
        } else {
            return;
        }

        input_ = prompt_history_[history_pos_];
        input_cursor_ = static_cast<int>(input_.size());
        screen.PostEvent(Event::Custom);
    }

    void history_next() {
        if (history_pos_ == kNone)
            return;

        if (history_pos_ + 1 < prompt_history_.size()) {
            ++history_pos_;
            input_ = prompt_history_[history_pos_];
        } else {
            history_pos_ = kNone;
            input_ = history_pending_;
        }

        input_cursor_ = static_cast<int>(input_.size());
        screen.PostEvent(Event::Custom);
    }

    void submit() {
        if (busy_.load()) {
            append(Line{
                "still running a turn...",
                Color::GrayLight,
                false,
                false
            });

            return;
        }

        std::string prompt = trim_string(input_);

        if (prompt.empty())
            return;

        record_history(prompt);

        input_.clear();
        input_cursor_ = 0;

        if (prompt[0] == '/') {
            handle_command(prompt);
            return;
        }

        append(Line{
            prompt,
            Color::BlueLight,
            true,
            false
        });

        run_turn_async(prompt);
    }

    Element transcript_element() {
        std::lock_guard<std::mutex> lock(mutex_);

        const int width = transcript_width();

        int height = page_height();

        if (height < 1)
            height = 1;

        // Build the physical terminal representation.
        ensure_physical_lines_locked(width);

        const size_t physical_count = physical_lines_.size();

        const size_t max_first =
            physical_count > static_cast<size_t>(height)
                ? physical_count - static_cast<size_t>(height)
                : 0;

        // Follow mode means the viewport is always at the bottom.
        if (follow_bottom_ ||
            view_row_ == std::numeric_limits<size_t>::max()) {

            view_row_ = max_first;
        } else if (view_row_ > max_first) {
            view_row_ = max_first;
        }

        const size_t first_row = view_row_;

        const size_t last_row =
            std::min(
                physical_count,
                first_row + static_cast<size_t>(height));

        Elements elements;
        elements.reserve(last_row - first_row);

        for (size_t i = first_row; i < last_row; ++i)
            elements.push_back(
                physical_line_element(physical_lines_[i]));

        if (elements.empty())
            elements.push_back(text(""));

        return hbox({
            text(" ")
                | color(Color::Default),

            vbox(std::move(elements))
                | vscroll_indicator
                | yframe
                | flex
                | reflect(transcript_box_),

            text(" ")
                | color(Color::Default),
        });
    }

    Element context_element() {
        const int ctx = ctx_window_.load();

        const TokenUsage usage = agent.usage();
        const int used = usage.prompt_tokens;

        if (ctx <= 0)
            return text("ctx: --") | dim;

        int filled = used * 12 / ctx;

        if (filled < 0)
            filled = 0;

        if (filled > 12)
            filled = 12;

        std::string bar;

        for (int i = 0; i < filled; ++i)
            bar += "\u2588";

        for (int i = filled; i < 12; ++i)
            bar += "\u2591";

        const double frac =
            static_cast<double>(used) / ctx;

        const int pct =
            static_cast<int>(frac * 100 + 0.5);

        Color bar_color =
            used < ctx / 2
                ? Color::Green
                : used < ctx * 4 / 5
                    ? Color::Yellow
                    : Color::RedLight;

        return hbox({
            text("ctx ") | dim,

            text(bar) | color(bar_color),

            text(
                " " +
                std::to_string(pct) +
                "% " +
                std::to_string(used) +
                "/" +
                std::to_string(ctx)
            ) | dim,
        });
    }

    Element loading_element() {
        // Indeterminate loader:
        // a soft shade gradient slides across the bar.

        const int cells = 14;

        const int pos =
            static_cast<int>(anim_phase_.load()) %
                (cells + 4) -
            2;

        std::string bar;

        for (int i = 0; i < cells; ++i) {
            int d = i - pos;

            if (d < 0)
                d = -d;

            bar +=
                d == 0 ? "\u2588" :
                d == 1 ? "\u2593" :
                d == 2 ? "\u2592" :
                         "\u2591";
        }

        bar += "  typing";

        return hbox({
            text(" ") | dim,
            text(bar) | color(Color::Yellow),
            text(" ") | dim,
        });
    }

    Element usage_element() {
        const TokenUsage usage = agent.usage();

        const int in_tokens = usage.prompt_tokens;
        const int out_tokens = usage.completion_tokens;

        auto format_tokens = [](int tokens) -> std::string {
            if (tokens >= 1000000) {
                return std::to_string(tokens / 1000000) + "." +
                    std::to_string((tokens / 100000) % 10) + "m";
            }

            if (tokens >= 1000) {
                return std::to_string(tokens / 1000) + "." +
                    std::to_string((tokens / 100) % 10) + "k";
            }

            return std::to_string(tokens);
        };

        return hbox({
            text(format_tokens(in_tokens) + " in")
                | dim,

            text(" · ")
                | dim,

            text(format_tokens(out_tokens) + " out")
                | dim,

            text(" · ")
                | dim,

            text(agent.model())
                | dim,
        });
    }

    Element status_element() {
        std::lock_guard<std::mutex> lock(mutex_);

        Element left;

        if (busy_.load()) {
            left = loading_element();
        } else {
            left = text("○ idle")
                | color(Color::Green);
        }

        return hbox({
            text(" "),

            hbox({
                left,

                filler(),

                usage_element(),
            }) | flex,

            text(" "),
        });
    }

    void setup() {
        input_component_ =
            Make<PromptEditor>(
                input_,
                input_cursor_,
                [this] {
                    submit();
                },
                [this] {
                    history_previous();
                },
                [this] {
                    history_next();
                });

        input_component_->TakeFocus();

        auto prompt_glyph =
            Renderer(input_component_, [this] {
                std::string glyph =
                    busy_.load() ? "…" : "❯";

                return hbox({
                    text(glyph + " ")
                        | color(
                            busy_.load()
                                ? Color::Yellow
                                : Color::GreenLight)
                        | bold,

                    input_component_->Render()
                        | flex,
                }) | reflect(prompt_box_);
            });

        auto transcript =
            Renderer([this] {
                return transcript_element();
            });

        auto container =
            Container::Vertical({
                transcript,
                prompt_glyph
            });

        layout_ =
            Renderer(
                container,
                [this, transcript, prompt_glyph] {

                    Element body =
                        vbox({
                            hbox({
                                text(" "),
                                
                                hbox({
                                    text("ZVMH")
                                        | bold
                                        | color(Color::CyanLight),

                                    text(" · coding agent")
                                        | dim,

                                    filler(),

                                    text(
                                        agent.provider_name() +
                                        " · " +
                                        agent.model()
                                    ) | dim,
                                }) | flex,

                                text(" "),
                            }),

                            separatorLight(),

                            transcript->Render()
                                | flex,

                            separatorLight(),

                            status_element(),

                            prompt_glyph->Render(),
                        })
                        | borderRounded;

                    refresh_suggestions();

                    // When a slash command is being typed, float the
                    // autocomplete popup above the prompt bar, on top of
                    // everything else on screen.
                    if (completion_choices_.empty())
                        return body;

                    // Anchor the popup so its bottom edge sits above the
                    // prompt bar (status row + separator + prompt height).
                    int prompt_rows = prompt_box_.y_min >= 0
                        ? prompt_box_.y_max - prompt_box_.y_min + 1
                        : 1;

                    const int bottom_margin =
                        prompt_rows + 1 + 1;

                    return dbox({
                        body,

                        vbox({
                            filler(),

                            suggestions_element()
                                | clear_under,

                            text("")
                                | size(
                                    HEIGHT,
                                    EQUAL,
                                    bottom_margin),
                        }),
                    });
                });

        refresh_context();

        if (agent.swarm_connected()) {
            agent.set_swarm_realtime(
                [this](const std::string& line) {
                    swarm_live(line);
                });
        }
    }

    bool scroll_event(Event event) {
        int step = 0;

        if (event == Event::PageUp ||
            event == Event::PageDown) {

            const int amount =
                std::max(1, page_height());

            step =
                event == Event::PageUp
                    ? -amount
                    : amount;

        } else if (event.is_mouse()) {
            const auto& mouse = event.mouse();

            if (mouse.button == Mouse::WheelUp) {
                // Three physical terminal rows per wheel tick.
                step = -3;

            } else if (mouse.button == Mouse::WheelDown) {
                step = 3;
            }
        }

        if (step == 0)
            return false;

        std::lock_guard<std::mutex> lock(mutex_);

        const int width = transcript_width();

        ensure_physical_lines_locked(width);

        if (physical_lines_.empty())
            return false;

        const size_t max_first =
            max_view_row_locked();

        if (step < 0) {
            // If we're currently following the bottom, begin from the
            // actual bottom rather than the last logical line.
            if (follow_bottom_ ||
                view_row_ == std::numeric_limits<size_t>::max()) {

                view_row_ = max_first;
                follow_bottom_ = false;
            }

            const size_t amount =
                static_cast<size_t>(-step);

            if (view_row_ > amount)
                view_row_ -= amount;
            else
                view_row_ = 0;

            follow_bottom_ = false;
        }

        else {
            // Already at the bottom.
            if (follow_bottom_)
                return false;

            const size_t amount =
                static_cast<size_t>(step);

            if (view_row_ + amount >= max_first) {
                view_row_ = max_first;

                // Reaching the exact bottom resumes follow mode.
                follow_bottom_ = true;
            } else {
                view_row_ += amount;
            }
        }

        screen.PostEvent(Event::Custom);

        return true;
    }

    bool OnEvent(Event event) override {
        if (event == Event::Custom)
            return true;

        if (event == Event::PageUp ||
            event == Event::PageDown) {

            return scroll_event(event);
        }

        if (event.is_mouse()) {
            const auto& mouse = event.mouse();

            if (mouse.button == Mouse::WheelUp ||
                mouse.button == Mouse::WheelDown) {

                return scroll_event(event);
            }
        }

        // Command autocomplete handling while the input starts with '/'.
        if (!input_.empty() && input_[0] == '/') {
            if (event == Event::Tab) {
                // Fill in the highlighted suggestion without running it.
                refresh_suggestions();
                if (!completion_choices_.empty()) {
                    accept_suggestion();
                    return true;
                }
            } else if (event == Event::ArrowDown) {
                if (cycle_suggestion(+1))
                    return true;
            } else if (event == Event::ArrowUp) {
                if (cycle_suggestion(-1))
                    return true;
            } else if (event == Event::Return) {
                refresh_suggestions();
                if (!completion_choices_.empty()) {
                    const std::string& sel =
                        completion_choices_[completion_index_];

                    if (sel == input_) {
                        // Already filled in — run it.
                        accept_suggestion();
                        submit();
                    } else if (input_.size() < sel.size() &&
                               sel.rfind(input_, 0) == 0) {
                        // Partial command word (e.g. "/mg") — fill it in so
                        // the next Enter runs the completed command.
                        accept_suggestion();
                    } else {
                        // Full command with an argument past the suggestion
                        // (e.g. "/msg all hi") — run it as typed.
                        submit();
                    }
                    return true;
                }
            }
        }

        return layout_->OnEvent(event);
    }

    Element OnRender() override {
        return layout_->Render();
    }
};

Tui::Tui(Agent& agent)
    : impl_(std::make_shared<Impl>(agent)) {

    impl_->setup();
}

Tui::~Tui(){
    std::cout << "\x1b[<u" << std::flush;
};

int Tui::run() {
    std::cout << "\x1b[>1u" << std::flush;
    impl_->screen.Loop(impl_);
    return 0;
}