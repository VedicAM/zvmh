#ifndef PROMPT_EDITOR_H
#define PROMPT_EDITOR_H

#include <functional>
#include <string>
#include <vector>
#include <iostream>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/terminal.hpp>

namespace ftxui {

namespace prompt_editor_detail {

// Byte length of the UTF-8 glyph starting at s[i].
size_t pe_glyph_len(const std::string& s, size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Approximate terminal display width of the glyph at s[i] (1 or 2 columns).
int pe_glyph_width(const std::string& s, size_t i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    return c < 0x80 ? 1 : 2;
}

// Display width of a UTF-8 string.
int pe_string_width(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        w += pe_glyph_width(s, i);
        i += pe_glyph_len(s, i);
    }
    return w;
}

// Byte index of the start of the glyph before `pos`.
size_t pe_glyph_back(const std::string& s, size_t pos) {
    if (pos == 0) return 0;
    size_t prev = pos - 1;
    while (prev > 0 && (static_cast<unsigned char>(s[prev]) & 0xC0) == 0x80) {
        --prev;
    }
    return prev;
}

}  // namespace prompt_editor_detail

// A small multiline text editor backed by a plain std::string, rendered as
// hard-wrapped rows. FTXUI's Input component clips lines that exceed the box
// width instead of wrapping them, so we draw our own editor here.
class PromptEditor : public ComponentBase {
public:
    PromptEditor(std::string& content, int& cursor, std::function<void()> on_submit)
        : content_(content), cursor_(cursor), on_submit_(std::move(on_submit)) {
        const auto size = Terminal::Size();
        if (size.dimx > 0) avail_ = size.dimx - 6;
    }

    // Number of visual (wrapped) rows the current content occupies.
    int wrapped_rows() const { return static_cast<int>(wrap().size()); }

private:
    std::string& content_;
    int& cursor_;
    std::function<void()> on_submit_;
    Box box_;
    int avail_ = 40;

    struct Row {
        size_t start;  // byte offset where this row begins
        size_t end;    // byte offset one past the row's last glyph
        int width;     // display width
    };

    // Hard-wrap content_ at glyph boundaries. Rows are contiguous byte spans of
    // content_, so byte -> (row, col) mapping is exact.
    std::vector<Row> wrap() const {
        std::vector<Row> rows;
        if (content_.empty()) {
            rows.push_back(Row{0, 0, 0});
            return rows;
        }
        const int limit = avail_ > 0 ? avail_ : 1;
        size_t start = 0;
        int row_width = 0;
        size_t i = 0;
        while (i < content_.size()) {
            const unsigned char c = static_cast<unsigned char>(content_[i]);
            if (c == '\n') {
                rows.push_back(Row{start, i, row_width});
                ++i;
                start = i;
                row_width = 0;
                continue;
            }
            const size_t glen = prompt_editor_detail::pe_glyph_len(content_, i);
            const int gw = prompt_editor_detail::pe_glyph_width(content_, i);
            if (row_width > 0 && row_width + gw > limit) {
                rows.push_back(Row{start, i, row_width});
                start = i;
                row_width = 0;
            }
            row_width += gw;
            i += glen;
        }
        rows.push_back(Row{start, i, row_width});
        return rows;
    }

    bool cursor_position(const std::vector<Row>& rows, int* row, int* col) const {
        const int cur = cursor_;
        for (size_t r = 0; r < rows.size(); ++r) {
            if (cur >= static_cast<int>(rows[r].start) && cur <= static_cast<int>(rows[r].end)) {
                *row = static_cast<int>(r);
                *col = prompt_editor_detail::pe_string_width(
                    content_.substr(rows[r].start, cur - rows[r].start));
                return true;
            }
        }
        const Row& last = rows.back();
        *row = static_cast<int>(rows.size() - 1);
        *col = prompt_editor_detail::pe_string_width(
            content_.substr(last.start, last.end - last.start));
        return false;
    }

    int cursor_row(const std::vector<Row>& rows) const {
        int row = 0;
        int col = 0;
        cursor_position(rows, &row, &col);
        return row;
    }

    void clamp_cursor() {
        if (cursor_ < 0) cursor_ = 0;
        if (cursor_ > static_cast<int>(content_.size())) cursor_ = static_cast<int>(content_.size());
    }

    bool handle_backspace() {
        if (cursor_ <= 0) return true;
        const size_t prev = prompt_editor_detail::pe_glyph_back(content_, static_cast<size_t>(cursor_));
        content_.erase(prev, static_cast<size_t>(cursor_) - prev);
        cursor_ = static_cast<int>(prev);
        return true;
    }

    bool handle_delete() {
        if (cursor_ >= static_cast<int>(content_.size())) return false;
        const size_t glen = prompt_editor_detail::pe_glyph_len(content_, static_cast<size_t>(cursor_));
        content_.erase(static_cast<size_t>(cursor_), glen);
        return true;
    }

    bool handle_arrow_horizontal(int delta) {
        clamp_cursor();
        if (delta < 0) {
            if (cursor_ > 0) {
                cursor_ = static_cast<int>(prompt_editor_detail::pe_glyph_back(content_,
                                                                               static_cast<size_t>(cursor_)));
            }
        } else if (cursor_ < static_cast<int>(content_.size())) {
            cursor_ += static_cast<int>(prompt_editor_detail::pe_glyph_len(content_,
                                                                           static_cast<size_t>(cursor_)));
        }
        return true;
    }

    bool handle_vertical(int dir) {
        clamp_cursor();
        const std::vector<Row> rows = wrap();
        int row = 0;
        int col = 0;
        cursor_position(rows, &row, &col);
        const int target = row + dir;
        if (target < 0 || target >= static_cast<int>(rows.size())) {
            return false;
        }
        const Row& to = rows[static_cast<size_t>(target)];
        size_t pos = to.start;
        int w = 0;
        while (pos < to.end) {
            const size_t glen = prompt_editor_detail::pe_glyph_len(content_, pos);
            const int gw = prompt_editor_detail::pe_glyph_width(content_, pos);
            if (w + gw > col) break;
            w += gw;
            pos += glen;
        }
        cursor_ = static_cast<int>(pos);
        return true;
    }

    void handle_home_end(bool home) {
        const std::vector<Row> rows = wrap();
        const int row = cursor_row(rows);
        cursor_ = home ? static_cast<int>(rows[static_cast<size_t>(row)].start)
                       : static_cast<int>(rows[static_cast<size_t>(row)].end);
    }

    bool handle_mouse(Event event) {
        const auto& m = event.mouse();
        if (m.button != Mouse::Left || m.motion != Mouse::Pressed) {
            return false;
        }
        if (m.x < box_.x_min || m.x > box_.x_max || m.y < box_.y_min || m.y > box_.y_max) {
            return false;
        }
        TakeFocus();
        const std::vector<Row> rows = wrap();
        const int target_row = m.y - box_.y_min;
        if (target_row < 0) return true;
        if (target_row >= static_cast<int>(rows.size())) return true;
        const Row& row = rows[static_cast<size_t>(target_row)];
        const int target_col = m.x - box_.x_min;
        size_t pos = row.start;
        int w = 0;
        while (pos < row.end) {
            const size_t glen = prompt_editor_detail::pe_glyph_len(content_, pos);
            const int gw = prompt_editor_detail::pe_glyph_width(content_, pos);
            if (w + gw > target_col) break;
            w += gw;
            pos += glen;
        }
        cursor_ = static_cast<int>(pos);
        return true;
    }

    // Whether all the ancestors are active and this is focusable.
    bool Focusable() const override { return true; }

    bool OnEvent(Event event) override {
        // Enter = submit
        if (event == Event::Return) {
            on_submit_();
            return true;
        }

        // Shift+Enter — modifyOtherKeys variant
            if (event.input() == "\x1b\r") {
                content_.insert(static_cast<size_t>(cursor_), "\n");
                ++cursor_;
                return true;
            }




        // Ctrl+J = newline
        if (event.is_character() && event.character() == "\n") {
            content_.insert(static_cast<size_t>(cursor_), "\n");
            ++cursor_;
            return true;
        }

        if (event.is_character()) {
            const std::string& c = event.character();
            content_.insert(static_cast<size_t>(cursor_), c);
            cursor_ += static_cast<int>(c.size());
            return true;
        }

        if (event == Event::Backspace) return handle_backspace();
        if (event == Event::Delete) return handle_delete();
        if (event == Event::ArrowLeft) return handle_arrow_horizontal(-1);
        if (event == Event::ArrowRight) return handle_arrow_horizontal(+1);
        if (event == Event::ArrowUp) return handle_vertical(-1);
        if (event == Event::ArrowDown) return handle_vertical(+1);

        if (event == Event::Home) {
            handle_home_end(true);
            return true;
        }

        if (event == Event::End) {
            handle_home_end(false);
            return true;
        }

        if (event.is_mouse()) return handle_mouse(event);

        return false;
    }

    Element OnRender() override {
        if (box_.x_min >= 0 && box_.x_max >= box_.x_min) {
            avail_ = box_.x_max - box_.x_min + 1;
        }
        const std::vector<Row> rows = wrap();
        const bool empty = content_.empty();
        int cursor_row = 0;
        int cursor_col = 0;
        cursor_position(rows, &cursor_row, &cursor_col);

        Elements cells;
        cells.reserve(rows.size());
        for (size_t r = 0; r < rows.size(); ++r) {
            const std::string line = content_.substr(rows[r].start, rows[r].end - rows[r].start);
            if (empty && r == 0) {
                cells.push_back(hbox({
                    text("\u2588") | bold | color(Color::White),
                    // text("Type a prompt, press Enter to send. Shift+Enter for a new line. /help for commands.") | dim,
                }));
            } else if (static_cast<int>(r) == cursor_row) {
                const size_t local = static_cast<size_t>(cursor_) - rows[r].start;
                cells.push_back(hbox({
                    text(line.substr(0, local)),
                    text("\u2588") | bold | color(Color::White),
                    text(line.substr(local)),
                }));
            } else {
                cells.push_back(text(line));
            }
        }
        return vbox(std::move(cells)) | reflect(box_);
    }
};

}  // namespace ftxui

#endif