#include "tui.h"
#include "prompt_editor.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
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

std::string trim_string(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Display width (columns) of a UTF-8 string, treating tab as one column and
// counting non-ASCII code points as two columns.
struct Line {
    std::string text;
    Color color = Color::Default;
    bool bold = false;
    bool dim = false;
};

Element line_element(const Line& line) {
    Element element = paragraph(line.text);
    if (line.bold) element = element | bold;
    if (line.dim) element = element | dim;
    if (line.color != Color::Default) element = element | color(line.color);
    return element;
}

}  // namespace

struct Tui::Impl : public ComponentBase, public StreamSink {
    Agent& agent;
    ScreenInteractive screen = ScreenInteractive::Fullscreen();
    std::atomic<bool> busy_{false};
    std::atomic<int> ctx_window_{0};
    std::atomic<uint32_t> anim_phase_{0};

    std::mutex mutex_;
    std::vector<Line> lines_;
    size_t live_index_ = kNone;
    std::string live_text_;
    std::string last_summary_;
    std::string input_;

    bool follow_bottom_ = true;
    size_t view_line_ = 0;
    Box transcript_box_;

    static constexpr size_t kNone = static_cast<size_t>(-1);

    Component layout_;
    Component input_component_;
    int input_cursor_ = 0;

    explicit Impl(Agent& agent_ref) : agent(agent_ref) {}

    void append(const Line& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
        screen.PostEvent(Event::Custom);
    }

    void set_live_line() {
        if (live_index_ == kNone) {
            lines_.push_back(Line{live_text_});
            live_index_ = lines_.size() - 1;
        } else {
            lines_[live_index_] = Line{live_text_};
        }
        screen.PostEvent(Event::Custom);
    }

    void flush_live_text() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!live_text_.empty()) {
            set_live_line();
        }
        live_text_.clear();
        live_index_ = kNone;
    }

    // --- StreamSink (called from the worker thread) ---

    void header(const std::string& provider, const std::string& model) override {
        flush_live_text();
        append(Line{provider + " · " + model, Color::GrayDark, false, true});
    }

    void text_delta(const std::string& text) override {
        std::lock_guard<std::mutex> lock(mutex_);
        live_text_ += text;
        set_live_line();
    }

    void tool_start(const std::string& name) override {
        flush_live_text();
        append(Line{"→ " + name, Color::Cyan, true, false});
    }

    void summary(const std::string& model, int prompt_tokens, int completion_tokens) override {
        flush_live_text();
        std::string summary = "model: " + model + "  | in: " + std::to_string(prompt_tokens)
                            + "  | out: " + std::to_string(completion_tokens);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_summary_ = summary;
        }
        append(Line{"", Color::GrayDark, false, true});
        append(Line{summary, Color::GrayDark, false, true});
    }

    void tool_call(const std::string& name, const nlohmann::json& args) override {
        append(Line{"▸ " + name + " " + args.dump(), Color::Yellow, true, false});
    }

    void tool_result(const std::string& result, bool is_error) override {
        append(Line{"↩ " + result, is_error ? Color::RedLight : Color::Green, false, false});
    }

    void warning(const std::string& text) override {
        flush_live_text();
        append(Line{"✖ " + text, Color::RedLight, false, false});
    }

    // --- Slash commands (main thread) ---

    void handle_command(const std::string& cmd) {
        if (cmd.rfind("/model", 0) == 0) {
            std::string arg = trim_string(cmd.substr(6));
            if (arg.empty()) {
                append(Line{"current model: " + agent.model(), Color::Cyan, true, false});
                append(Line{"set a new model with: /model <name>", Color::GrayDark, true, false});
            } else {
                agent.set_model(arg);
                ctx_window_.store(0);
                refresh_context();
                append(Line{"model set to: " + agent.model(), Color::Cyan, true, false});
            }
        } else if (cmd == "/tools" || cmd.rfind("/tools ", 0) == 0) {
            std::string arg = trim_string(cmd.substr(6));
            append(Line{"registered tools:", Color::Cyan, true, false});
            auto defs = agent.tools();
            if (arg.empty()) {
                for (const auto& def : defs) {
                    append(Line{"  - " + def.name + "  " + def.description, Color::Default, true, false});
                }
            } else {
                Tool* tool = agent.tool(arg);
                if (tool) {
                    append(Line{"  - " + std::string(tool->name()) + "  " + tool->description(),
                                Color::Default, true, false});
                    append(Line{"      schema: " + tool->parameters_schema().dump(), Color::GrayDark, false, true});
                } else {
                    append(Line{"  unknown tool: '" + arg + "'", Color::RedLight, false, false});
                }
            }
        } else if (cmd == "/clear") {
            agent.clear_messages();
            follow_bottom_ = true;
            view_line_ = 0;
            append(Line{"conversation cleared.", Color::GrayDark, false, true});
        } else if (cmd == "/help") {
            append(Line{"/model             show current model", Color::Default, false, false});
            append(Line{"/model <name>      switch model (e.g. /model openai/gpt-3.5-turbo)", Color::Default, false, false});
            append(Line{"/tools             list tools", Color::Default, false, false});
            append(Line{"/tools <name>      show tool schema", Color::Default, false, false});
            append(Line{"/clear             clear conversation history", Color::Default, false, false});
            append(Line{"/exit, /quit       leave the TUI", Color::Default, false, false});
            append(Line{"/help              show this help", Color::Default, false, false});
        } else if (cmd == "/exit" || cmd == "/quit") {
            screen.Exit();
        } else {
            append(Line{"  unknown command: '" + cmd + "'   (try /help)", Color::RedLight, false, false});
        }
    }

    // --- Turn execution (worker thread) ---

    void refresh_context() {
        std::thread([this] {
            int ctx = agent.context_window();
            ctx_window_.store(ctx);
            screen.PostEvent(Event::Custom);
        }).detach();
    }

    void run_turn_async(const std::string& prompt) {
        busy_.store(true);
        anim_phase_.store(0);
        screen.PostEvent(Event::Custom);
        std::thread([this] { animate_while_busy(); }).detach();
        std::thread([this, prompt] {
            int rc = agent.run_turn(prompt, *this);
            flush_live_text();
            append(Line{"", Color::GrayDark, false, true});
            append(Line{rc == 0 ? "done." : "turn failed.", Color::GrayDark, false, true});
            busy_.store(false);
            screen.PostEvent(Event::Custom);
        }).detach();
    }

    void animate_while_busy() {
        while (busy_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!busy_.load()) break;
            anim_phase_.fetch_add(1);
            screen.PostEvent(Event::Custom);
        }
    }

    void submit() {
        if (busy_.load()) {
            append(Line{"still running a turn...", Color::GrayDark, false, true});
            return;
        }
        std::string prompt = trim_string(input_);
        if (prompt.empty()) return;
        input_.clear();
        input_cursor_ = 0;
        if (prompt[0] == '/') {
            handle_command(prompt);
            return;
        }
        append(Line{prompt, Color::BlueLight, true, false});
        run_turn_async(prompt);
    }

    // --- Rendering ---

    Element transcript_element() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Element> elements;
        elements.reserve(lines_.size());
        for (const auto& line : lines_) {
            elements.push_back(line_element(line));
        }
        if (!elements.empty()) {
            elements.back() = elements.back() | focus;
        }
        return vbox(std::move(elements)) | vscroll_indicator | yframe | flex;
    }

    Element context_element() {
        const int ctx = ctx_window_.load();
        const TokenUsage usage = agent.usage();
        const int used = usage.prompt_tokens;

        if (ctx <= 0) {
            return text("ctx: --") | dim;
        }

        int filled = used * 12 / ctx;
        if (filled < 0) filled = 0;
        if (filled > 12) filled = 12;

        std::string bar;
        for (int i = 0; i < filled; ++i) bar += "\u2588";
        for (int i = filled; i < 12; ++i) bar += "\u2591";

        const double frac = static_cast<double>(used) / ctx;
        const int pct = static_cast<int>(frac * 100 + 0.5);
        Color bar_color = used < ctx / 2 ? Color::Green
                          : used < ctx * 4 / 5 ? Color::Yellow
                                               : Color::RedLight;

        return hbox({
                   text("ctx ") | dim,
                   text(bar) | color(bar_color),
                   text(" " + std::to_string(pct) + "% " +
                        std::to_string(used) + "/" + std::to_string(ctx)) | dim,
               });
    }

    Element loading_element() {
        // Indeterminate loader: a soft shade gradient slides across the bar.
        const int cells = 14;
        const int pos = static_cast<int>(anim_phase_.load()) % (cells + 4) - 2;

        std::string bar;
        for (int i = 0; i < cells; ++i) {
            int d = i - pos;
            if (d < 0) d = -d;
            bar += d == 0 ? "\u2588"   // █ peak
                   : d == 1 ? "\u2593" // ▓
                   : d == 2 ? "\u2592" // ▒
                   : "\u2591";         // ░
        }
        bar += "  typing";

        return hbox({
                   text(" ") | dim,
                   text(bar) | color(Color::Yellow),
                   text(" ") | dim,
               });
    }

    Element status_element() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_.load()) {
            return hbox({
                loading_element(),
                filler(),
                context_element() | size(WIDTH, LESS_THAN, 30),
                filler(),
                text(last_summary_)
                    | size(WIDTH, LESS_THAN, 60) | dim,
            });
        }
        return hbox({
            text("○ idle") | color(Color::Green),
            filler(),
            context_element() | size(WIDTH, LESS_THAN, 30),
            filler(),
            text(last_summary_)
                | size(WIDTH, LESS_THAN, 60) | dim,
        });
    }

    void setup() {
        input_component_ = Make<PromptEditor>(input_, input_cursor_, [this] { submit(); });
        input_component_->TakeFocus();

        auto prompt_glyph = Renderer(input_component_, [this] {
            std::string glyph = busy_.load() ? "…" : "❯";
            return hbox({
                       text(glyph + " ") | color(busy_.load() ? Color::Yellow : Color::GreenLight) | bold,
                       input_component_->Render() | flex,
                   });
        });

        auto transcript = Renderer([this] { return transcript_element(); });

        auto container = Container::Vertical({transcript, prompt_glyph});

        layout_ = Renderer(container, [this, transcript, prompt_glyph] {
            return vbox({
                       hbox({
                           text(" ZVMH ") | bold | color(Color::CyanLight),
                           text("· coding agent ") | dim,
                           filler(),
                           text(agent.provider_name() + " · " + agent.model()) | dim,
                       }),
                       separatorLight(),
                       transcript->Render() | flex,
                       separatorLight(),
                       status_element(),
                       prompt_glyph->Render(),
                   }) |
                   borderRounded;
        });

        refresh_context();
    }

    // --- ComponentBase ---

    int page_height() const {
        int h = transcript_box_.y_max - transcript_box_.y_min + 1;
        return h > 1 ? h : 15;
    }

    bool scroll_event(Event event) {
        int step = 0;
        if (event == Event::PageUp || event == Event::PageDown) {
            step = (event == Event::PageUp ? -1 : 1) * page_height();
        } else if (event.is_mouse()) {
            auto& m = event.mouse();
            if (m.button == Mouse::WheelUp) {
                step = -2;
            } else if (m.button == Mouse::WheelDown) {
                step = 2;
            }
        }
        if (step == 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (lines_.size() <= 1) {
            return false;
        }

        const size_t last = lines_.size() - 1;
        if (step < 0) {
            if (follow_bottom_) {
                view_line_ = last;
                follow_bottom_ = false;
            }
            const size_t back = static_cast<size_t>(-step);
            view_line_ = view_line_ > back ? view_line_ - back : 0;
        } else {
            if (follow_bottom_) {
                return false;
            }
            view_line_ += static_cast<size_t>(step);
            if (view_line_ >= last) {
                follow_bottom_ = true;
            }
        }
        return true;
    }

    bool OnEvent(Event event) override {
        if (event == Event::Custom) {
            return true;
        }
        return layout_->OnEvent(event);
    }

    Element OnRender() override {
        return layout_->Render();
    }
};

Tui::Tui(Agent& agent) : impl_(std::make_shared<Impl>(agent)) {
    impl_->setup();
}

Tui::~Tui() = default;

int Tui::run() {
    impl_->screen.Loop(impl_);
    return 0;
}