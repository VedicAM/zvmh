#ifndef TUI_MARKDOWN_H
#define TUI_MARKDOWN_H

#include <ftxui/screen/color.hpp>
#include <string>
#include <utility>
#include <vector>

// Minimal Markdown renderer for the TUI transcript. Produces lines of styled
// spans (no width awareness here — wrapping happens in tui.cpp) from model
// output. Supports ATX headings, fenced code blocks, blockquotes, unordered
// and ordered lists, horizontal rules, GFM pipe tables (with :align: markers),
// and inline **bold**, *italic*, `code`, ~~strikethrough~~, and [label](url)
// links.

namespace markdown {

struct Span {
    std::string text;
    ftxui::Color color = ftxui::Color::Default;
    bool bold = false;
    bool dim = false;
    bool italic = false;
};

struct Line {
    std::vector<Span> spans;

    Line() = default;
    explicit Line(std::string t) { spans.emplace_back(Span{std::move(t)}); }
    Line(std::string t, ftxui::Color color, bool bold, bool dim) {
        spans.emplace_back(Span{std::move(t), color, bold, dim, false});
    }
    explicit Line(std::vector<Span> s) : spans(std::move(s)) {}
};

namespace detail {

inline std::string trim_left_ws(const std::string& s, size_t* out_indent = nullptr) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (out_indent) *out_indent = i;
    return s.substr(i);
}

inline std::string trim_right_ws(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
    return s.substr(0, end);
}

struct Attr {
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
};

inline bool attr_equal(const Attr& a, const Attr& b) {
    return a.bold == b.bold && a.italic == b.italic &&
           a.strike == b.strike && a.code == b.code;
}

inline ftxui::Color attr_color(const Attr& a) {
    if (a.code)
        return static_cast<ftxui::Color>(ftxui::Color::Cyan);
    if (a.bold)
        return static_cast<ftxui::Color>(ftxui::Color::White);
    return static_cast<ftxui::Color>(ftxui::Color::Default);
}

// Parse inline markup (bold/italic/code/strike/links) into styled spans,
// merging adjacent segments that share styling.
inline std::vector<Span> inline_spans(const std::string& s) {
    std::vector<Span> out;
    std::string cur;
    Attr attr;

    auto flush = [&] {
        if (cur.empty()) return;
        if (!out.empty()) {
            Span& last = out.back();
            if (last.color == attr_color(attr) && last.bold == attr.bold &&
                last.dim == (attr.strike && !attr.code) &&
                last.italic == attr.italic) {
                last.text += cur;
                cur.clear();
                return;
            }
        }
        out.emplace_back(Span{cur, attr_color(attr), attr.bold,
                              attr.strike && !attr.code, attr.italic});
        cur.clear();
    };

    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            cur += s[i + 1];
            i += 2;
            continue;
        }
        if (s[i] == '`') {
            flush();
            attr.code = !attr.code;
            ++i;
            continue;
        }
        if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            flush();
            attr.bold = !attr.bold;
            i += 2;
            continue;
        }
        if (s[i] == '*' ) {
            flush();
            attr.italic = !attr.italic;
            ++i;
            continue;
        }
        if (s[i] == '~' && i + 1 < s.size() && s[i + 1] == '~') {
            flush();
            attr.strike = !attr.strike;
            i += 2;
            continue;
        }
        if (s[i] == '[') {
            size_t close = s.find(']', i);
            if (close != std::string::npos && close + 1 < s.size() && s[close + 1] == '(') {
                size_t end = s.find(')', close + 1);
                if (end != std::string::npos) {
                    flush();
                    std::string label = s.substr(i + 1, close - i - 1);
                    Attr link_attr = attr;
                    out.emplace_back(Span{std::move(label), ftxui::Color::Cyan,
                                          true, false, false});
                    i = end + 1;
                    continue;
                }
            }
        }
        cur += s[i];
        ++i;
    }
    flush();
    return out;
}

enum class Align { Left, Center, Right };

struct TableCell {
    std::vector<Span> spans;  // styled content
    std::string text;         // plain text (markup stripped)
    int width = 0;            // approximate display width of `text`
};

struct TableRow {
    std::vector<TableCell> cells;
};

// Approximate terminal display width of a UTF-8 string. Mirrors the
// wrapping heuristic in tui.cpp (1 column for ASCII, 2 otherwise).
inline int string_width(const std::string& s) {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            w += 1;
            i += 1;
        } else {
            w += 2;
            ++i;
            while (i < s.size() &&
                   (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
                ++i;
        }
    }
    return w;
}

// Split a pipe row into cell strings. Escaped pipes (`\|`) survive and a
// leading/trailing pipe yields empty edge cells that are dropped. Returns
// true when at least one unescaped pipe split the row.
inline bool pipe_cells(const std::string& body, std::vector<std::string>& cells) {
    cells.clear();

    std::string cur;
    bool has_pipe = false;

    size_t i = 0;
    while (i < body.size()) {
        const char c = body[i];
        if (c == '\\' && i + 1 < body.size() &&
            (body[i + 1] == '|' || body[i + 1] == '\\')) {
            cur += body[i + 1];
            i += 2;
            continue;
        }
        if (c == '|') {
            has_pipe = true;
            cells.push_back(cur);
            cur.clear();
            ++i;
            continue;
        }
        cur += c;
        ++i;
    }
    cells.push_back(cur);

    if (!cells.empty() && !body.empty() && body.front() == '|')
        cells.erase(cells.begin());
    if (!cells.empty() && !body.empty() && body.back() == '|')
        cells.pop_back();

    return has_pipe;
}

// True when a delimiter cell is `:?` dashes `:?` (e.g. `---`, `:---:`, `---:`).
inline bool is_delimiter_cell(const std::string& raw) {
    std::string c = trim_left_ws(raw);
    c = trim_right_ws(c);
    if (c.empty()) return false;

    size_t i = 0;
    if (c[i] == ':') ++i;
    size_t dashes = i;
    while (i < c.size() && c[i] == '-') ++i;
    if (i == dashes) return false;  // at least one dash is required
    if (i < c.size() && c[i] == ':') ++i;
    return i == c.size();
}

// True for a delimiter row: at least two all-dash cells.
inline bool is_delimiter_row(const std::vector<std::string>& cells) {
    if (cells.size() < 2) return false;
    for (const std::string& raw : cells)
        if (!is_delimiter_cell(raw)) return false;
    return true;
}

// Column alignment implied by a delimiter cell.
inline Align delimiter_align(const std::string& raw) {
    std::string c = trim_left_ws(raw);
    c = trim_right_ws(c);
    const bool left = !c.empty() && c.front() == ':';
    const bool right = c.size() > 1 && c.back() == ':';
    if (left && right) return Align::Center;
    if (right) return Align::Right;
    return Align::Left;
}

inline TableCell make_cell(const std::string& raw) {
    std::string t = trim_left_ws(raw);
    t = trim_right_ws(t);

    TableCell cell;
    cell.spans = inline_spans(t);
    for (const Span& sp : cell.spans) cell.text += sp.text;
    cell.width = string_width(cell.text);
    return cell;
}

inline TableRow make_table_row(const std::vector<std::string>& cells) {
    TableRow row;
    row.cells.reserve(cells.size());
    for (const std::string& raw : cells)
        row.cells.push_back(make_cell(raw));
    return row;
}

// Byte length of the UTF-8 glyph starting at s[i] (0 when i >= s.size()).
inline size_t table_glyph_len(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Clip styled spans to `budget` display columns, appending an ellipsis when
// anything is dropped so a too-wide cell still ends cleanly.
inline void truncate_spans(std::vector<Span>& spans, int budget) {
    std::vector<Span> out;
    int used = 0;
    bool overflow = false;

    for (const Span& sp : spans) {
        const int w = string_width(sp.text);

        if (used + w <= budget) {
            out.push_back(sp);
            used += w;
            continue;
        }

        const int room = budget - used;
        std::string t;
        int tw = 0;
        size_t i = 0;

        while (i < sp.text.size()) {
            const size_t len = table_glyph_len(sp.text, i);
            if (len == 0) break;
            const int gw = static_cast<unsigned char>(sp.text[i]) < 0x80 ? 1 : 2;
            if (tw + gw > room) break;
            t += sp.text.substr(i, len);
            i += len;
            tw += gw;
        }

        out.emplace_back(Span{t, sp.color, sp.bold, sp.dim, sp.italic});
        overflow = true;
        break;
    }

    if (overflow)
        out.back().text += "\u2026";  // …

    spans = std::move(out);
}

// Alignment per column, derived from the delimiter row. Missing entries
// (delimiter shorter than the header) default to left.
inline std::vector<Align> table_alignment(const std::vector<std::string>& delim,
                                          size_t cols) {
    std::vector<Align> out(cols, Align::Left);
    for (size_t i = 0; i < delim.size() && i < cols; ++i)
        out[i] = delimiter_align(delim[i]);
    return out;
}

// A horizontal table rule built from the given corner/join glyphs.
inline std::string repeat_utf8(const std::string& glyph, size_t n) {
    std::string s;
    s.reserve(glyph.size() * n);
    for (size_t i = 0; i < n; ++i) s += glyph;
    return s;
}

inline Line border_line(const std::vector<int>& widths,
                        const std::string& left,
                        const std::string& cross,
                        const std::string& right) {
    std::string s = left;
    for (size_t c = 0; c < widths.size(); ++c) {
        s += repeat_utf8("\u2500", static_cast<size_t>(widths[c]) + 2);
        if (c + 1 < widths.size()) s += cross;
    }
    s += right;
    return Line{s, ftxui::Color::Default, false, true};
}

// One table row with cells padded to their column widths and aligned.
inline Line table_row_line(const TableRow& row,
                           const std::vector<int>& widths,
                           const std::vector<Align>& align,
                           bool header) {
    const Span pipe{"\u2502", ftxui::Color::Default, false, true, false};
    const Span space{" ", ftxui::Color::Default, false, false, false};

    std::vector<Span> spans;
    spans.reserve(widths.size() * 4 + 1);

    for (size_t c = 0; c < widths.size(); ++c) {
        spans.push_back(pipe);
        spans.push_back(space);

        const bool have = c < row.cells.size();
        int content_w = have ? row.cells[c].width : 0;

        // Cells wider than the column budget (e.g. the global cap) are
        // truncated to the budget so the border cannot blow out.
        std::vector<Span> cell_spans;
        if (have && content_w > widths[c]) {
            cell_spans = row.cells[c].spans;
            truncate_spans(cell_spans, widths[c]);
            content_w = widths[c];
        }

        int pad = widths[c] - content_w;
        int pad_left = 0;
        int pad_right = pad;
        if (c < align.size()) {
            if (align[c] == Align::Right) {
                pad_left = pad;
                pad_right = 0;
            } else if (align[c] == Align::Center) {
                pad_left = pad / 2;
                pad_right = pad - pad_left;
            }
        }

        if (pad_left > 0) {
            spans.push_back(Span{std::string(static_cast<size_t>(pad_left), ' '),
                                 ftxui::Color::Default, false, false, false});
        }
        if (have) {
            const std::vector<Span>& content =
                cell_spans.empty() ? row.cells[c].spans : cell_spans;
            for (Span sp : content) {
                if (header) sp.bold = true;
                spans.push_back(std::move(sp));
            }
        }
        if (pad_right > 0) {
            spans.push_back(Span{std::string(static_cast<size_t>(pad_right), ' '),
                                 ftxui::Color::Default, false, false, false});
        }
        spans.push_back(space);
    }
    spans.push_back(pipe);

    return Line{std::move(spans)};
}

// Append a full box-drawn table (top, header, separator, body, bottom) to
// `out`. Column widths are derived from the widest cell per column and capped
// so a pathological row cannot blow out the transcript width.
inline void emit_table(const std::vector<TableRow>& rows,
                       const std::vector<Align>& align,
                       std::vector<Line>& out) {
    const size_t ncols = align.size();
    if (rows.empty() || ncols == 0) return;

    const int kMaxColWidth = 40;

    std::vector<int> widths(ncols, 0);
    for (const TableRow& row : rows) {
        const size_t nc = row.cells.size();
        for (size_t c = 0; c < ncols; ++c) {
            if (c >= nc) continue;
            widths[c] = std::max(widths[c], row.cells[c].width);
        }
    }
    for (size_t c = 0; c < ncols; ++c)
        widths[c] = std::min(kMaxColWidth, widths[c]);

    out.push_back(border_line(widths, "\u250c", "\u252c", "\u2510"));
    out.push_back(table_row_line(rows[0], widths, align, true));
    out.push_back(border_line(widths, "\u251c", "\u253c", "\u2524"));
    for (size_t r = 1; r < rows.size(); ++r)
        out.push_back(table_row_line(rows[r], widths, align, false));
    out.push_back(border_line(widths, "\u2514", "\u2534", "\u2518"));
}

}  // namespace detail

// Render a block of markdown text into styled lines. Blank lines are dropped.
inline std::vector<Line> render(const std::string& text) {
    std::vector<Line> out;

    std::string t;
    t.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') continue;
            t += '\n';
            continue;
        }
        t += text[i];
    }

    std::vector<std::string> raw;
    for (size_t pos = 0; pos <= t.size();) {
        size_t nl = t.find('\n', pos);
        if (nl == std::string::npos) {
            raw.push_back(t.substr(pos));
            break;
        }
        raw.push_back(t.substr(pos, nl - pos));
        pos = nl + 1;
    }

    bool in_fence = false;
    char fence_char = '`';
    size_t fence_len = 3;

    // Table assembly state. A pipe row whose next line is a delimiter row
    // (:--- etc.) becomes the header; rows follow until a non-row line.
    bool in_table = false;
    std::vector<std::string> t_header;
    std::vector<detail::TableRow> t_rows;
    std::vector<detail::Align> t_align;

    // A pipe line that might be a table header is deferred one iteration:
    // if the next line is a delimiter row it becomes the header, otherwise
    // it is rendered as an ordinary block line.
    std::string t_candidate;
    size_t t_candidate_indent = 0;

    // Render a single non-fence, non-table line: heading, rule, quote, list,
    // or paragraph. The existing per-line checks live here so a deferred
    // table-header candidate can be flushed through the same path.
    auto render_block_line = [&](const std::string& body, size_t indent) {
        if (body.empty()) return;

        // ATX heading: 1-6 leading '#' followed by space.
        if (body[0] == '#') {
            size_t h = 0;
            while (h < body.size() && body[h] == '#') ++h;
            if (h <= 6 && (h == body.size() || body[h] == ' ' || body[h] == '\t')) {
                std::string rest = body.substr(h + (h < body.size() ? 1 : 0));
                rest = detail::trim_right_ws(rest);
                ftxui::Color c = h <= 2 ? static_cast<ftxui::Color>(ftxui::Color::CyanLight)
                                    : static_cast<ftxui::Color>(ftxui::Color::Default);
                auto spans = detail::inline_spans(rest);
                std::vector<Span> all;
                all.reserve(spans.size());
                for (auto& sp : spans) {
                    sp.color = c;
                    sp.bold = true;
                    all.push_back(std::move(sp));
                }
                if (all.empty()) all.emplace_back(Span{"", c, true, false, false});
                out.emplace_back(Line{std::move(all)});
                return;
            }
        }

        // Horizontal rule: >=3 of the same dash/star/underscore.
        {
            bool hr = body.size() >= 3;
            for (char ch : body)
                if (ch != '-' && ch != '*' && ch != '_') { hr = false; break; }
            if (hr) {
                out.emplace_back(Line{"───", ftxui::Color::Blue, false, false});
                return;
            }
        }

        // Blockquote.
        if (body[0] == '>') {
            size_t after = 1;
            if (after < body.size() && (body[after] == ' ' || body[after] == '\t')) ++after;
            std::string content = body.substr(after);
            std::vector<Span> spans;
            spans.push_back(Span{"│ ", ftxui::Color::Blue, false, false});
            for (auto& sp : detail::inline_spans(content)) {
                sp.color = ftxui::Color::Blue;
                spans.push_back(std::move(sp));
            }
            out.emplace_back(Line{std::move(spans)});
            return;
        }

        // Lists.
        {
            std::vector<Span> prefix;
            size_t li = 0;
            while (li < body.size() && (body[li] == ' ' || body[li] == '\t')) ++li;
            size_t marker_end = li;
            if (body[li] == '-' || body[li] == '+' || body[li] == '*') {
                marker_end = li + 1;
            } else {
                size_t digits = li;
                while (digits < body.size() && body[digits] >= '0' && body[digits] <= '9') ++digits;
                if (digits > li && digits < body.size() && body[digits] == '.') marker_end = digits + 1;
            }
            if (marker_end > li && marker_end < body.size() &&
                (body[marker_end] == ' ' || body[marker_end] == '\t')) {
                std::string content = body.substr(marker_end);
                std::string lead(indent, ' ');
                bool ordered = body[li] >= '0' && body[li] <= '9';
                std::string marker = ordered ? body.substr(li, marker_end - li) : "•";
                prefix.push_back(Span{lead + "  ", ftxui::Color::Default, false, false});
                prefix.push_back(Span{marker + " ", ftxui::Color::Default, true, false});
                for (auto& sp : detail::inline_spans(content))
                    prefix.push_back(std::move(sp));
                out.emplace_back(Line{std::move(prefix)});
                return;
            }
        }

        // Paragraph / plain.
        auto spans = detail::inline_spans(body);
        if (!spans.empty()) out.emplace_back(Line{std::move(spans)});
    };

    for (const std::string& line : raw) {
        size_t indent = 0;
        std::string body = detail::trim_left_ws(line, &indent);
        body = detail::trim_right_ws(body);

        if (!in_fence) {
            std::vector<std::string> pipe;
            const bool has_pipe = detail::pipe_cells(body, pipe);

            if (in_table) {
                // Extend the table with rows that look like it. Short rows
                // (fewer cells than the header) are padded with empties —
                // GFM-style — which also keeps a partially-streamed row from
                // tearing the table apart mid-flight. Extra cells end it.
                const bool valid =
                    has_pipe &&
                    !detail::is_delimiter_row(pipe) &&
                    !pipe.empty() &&
                    pipe.size() <= t_align.size();

                if (valid) {
                    t_rows.push_back(detail::make_table_row(pipe));
                    continue;
                }

                detail::emit_table(t_rows, t_align, out);
                in_table = false;

                // Swallow a stray delimiter row instead of printing it.
                if (has_pipe && detail::is_delimiter_row(pipe)) continue;
            }

            if (!t_candidate.empty()) {
                if (detail::is_delimiter_row(pipe)) {
                    t_rows.clear();
                    t_rows.push_back(detail::make_table_row(t_header));
                    t_align = detail::table_alignment(pipe, t_header.size());
                    in_table = true;
                    t_header.clear();
                    t_candidate.clear();
                    continue;
                }
                render_block_line(t_candidate, t_candidate_indent);
                t_candidate.clear();
                t_header.clear();
            }

            if (body.size() >= 3 &&
                (body[0] == '`' || body[0] == '~') &&
                body[0] == body[1] && body[0] == body[2]) {
                size_t j = 0;
                while (j < body.size() && body[j] == body[0]) ++j;
                if (j >= 3) {
                    in_fence = true;
                    fence_char = body[0];
                    fence_len = j;
                    continue;
                }
            }

            // A plain pipe line promises a table if the next line is a
            // delimiter row. Skip lines that open other block constructs.
            bool block_start =
                body.empty() ||
                body[0] == '#' || body[0] == '>' ||
                body[0] == '-' || body[0] == '*' || body[0] == '+';
            size_t digits = 0;
            while (digits < body.size() && body[digits] >= '0' && body[digits] <= '9') ++digits;
            if (digits > 0 && digits < body.size() && body[digits] == '.') block_start = true;

            if (has_pipe && !block_start && pipe.size() >= 2) {
                t_header = pipe;
                t_candidate = body;
                t_candidate_indent = indent;
                continue;
            }
        } else {
            if (body.size() >= fence_len &&
                body[0] == fence_char && body.substr(0, fence_len).find_first_not_of(fence_char) == std::string::npos) {
                in_fence = false;
                continue;
            }
            out.emplace_back(Line{"  " + line,
                                  ftxui::Color::Blue, false, false});
            continue;
        }

        render_block_line(body, indent);
    }

    if (in_table) detail::emit_table(t_rows, t_align, out);
    if (!t_candidate.empty())
        render_block_line(t_candidate, t_candidate_indent);

    return out;
}

}  // namespace markdown

#endif