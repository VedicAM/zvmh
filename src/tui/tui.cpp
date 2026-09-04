#include "tui.h"
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
#include <ftxui/screen/color.hpp>

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

    std::mutex mutex_;
    std::vector<Line> lines_;
    size_t live_index_ = kNone;
    std::string live_text_;
    std::string last_summary_;
    std::string input_;

    static constexpr size_t kNone = static_cast<size_t>(-1);

    Component layout_;
    Component input_component_;

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
            append(Line{"conversation cleared.", Color::GrayDark, false, true});
        } else if (cmd == "/help") {
            append(Line{"/model             show current model", Color::Default, false, false});
            append(Line{"/model <name>      switch model (e.g. /model openai/gpt-3.5)", Color::Default, false, false});
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

    void run_turn_async(const std::string& prompt) {
        busy_.store(true);
        screen.PostEvent(Event::Custom);
        std::thread([this, prompt] {
            int rc = agent.run_turn(prompt, *this);
            flush_live_text();
            append(Line{"", Color::GrayDark, false, true});
            append(Line{rc == 0 ? "done." : "turn failed.", Color::GrayDark, false, true});
            busy_.store(false);
            screen.PostEvent(Event::Custom);
        }).detach();
    }

    void submit() {
        if (busy_.load()) {
            append(Line{"still running a turn...", Color::GrayDark, false, true});
            return;
        }
        std::string prompt = trim_string(input_);
        if (prompt.empty()) return;
        input_.clear();
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

    Element status_element() {
        std::lock_guard<std::mutex> lock(mutex_);
        return hbox({
            text(busy_.load() ? "● running" : "○ idle")
                | color(busy_.load() ? Color::Yellow : Color::Green),
            filler(),
            text(last_summary_) | dim,
        });
    }

    void setup() {
        InputOption input_option = InputOption::Default();
        input_option.multiline = false;
        input_option.placeholder = "Type a prompt, press Enter. /help for commands.";

        input_component_ = Input(&input_, input_option);
        input_component_ |= CatchEvent([this](Event event) {
            if (event == Event::Return) {
                submit();
                return true;
            }
            return false;
        });

        auto prompt_glyph = Renderer(input_component_, [this] {
            std::string glyph = busy_.load() ? "…" : "❯";
            return hbox({
                       text(glyph) | color(busy_.load() ? Color::Yellow : Color::GreenLight) | bold,
                       text(" "),
                       input_component_->Render() | flex,
                   }) |
                   size(HEIGHT, EQUAL, 1);
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
    }

    // --- ComponentBase ---

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