#ifndef TUI_MARKDOWN_H
#define TUI_MARKDOWN_H

#include <ftxui/screen/color.hpp>
#include <string>
#include <utility>
#include <vector>

// Minimal Markdown renderer for the TUI transcript. Produces lines of styled
// spans (no width awareness here — wrapping happens in tui.cpp) from model
// output. Supports ATX headings, fenced code blocks, blockquotes, unordered
// and ordered lists, horizontal rules, and inline **bold**, *italic*, `code`,
// ~~strikethrough~~, and [label](url) links.

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
    return a.code ? static_cast<ftxui::Color>(ftxui::Color::GrayDark)
                  : static_cast<ftxui::Color>(ftxui::Color::Default);
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

    for (const std::string& line : raw) {
        size_t indent = 0;
        std::string body = detail::trim_left_ws(line, &indent);
        body = detail::trim_right_ws(body);

        if (!in_fence) {
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
        } else {
            if (body.size() >= fence_len &&
                body[0] == fence_char && body.substr(0, fence_len).find_first_not_of(fence_char) == std::string::npos) {
                in_fence = false;
                continue;
            }
            out.emplace_back(Line{"  " + line,
                                  ftxui::Color::GrayDark, false, false});
            continue;
        }

        if (body.empty()) continue;

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
                continue;
            }
        }

        // Horizontal rule: >=3 of the same dash/star/underscore.
        {
            bool hr = body.size() >= 3;
            for (char ch : body)
                if (ch != '-' && ch != '*' && ch != '_') { hr = false; break; }
            if (hr) {
                out.emplace_back(Line{"───", ftxui::Color::GrayDark, false, false});
                continue;
            }
        }

        // Blockquote.
        if (body[0] == '>') {
            size_t after = 1;
            if (after < body.size() && (body[after] == ' ' || body[after] == '\t')) ++after;
            std::string content = body.substr(after);
            std::vector<Span> spans;
            spans.push_back(Span{"│ ", ftxui::Color::GrayLight, false, false});
            for (auto& sp : detail::inline_spans(content)) {
                sp.color = ftxui::Color::GrayLight;
                spans.push_back(std::move(sp));
            }
            out.emplace_back(Line{std::move(spans)});
            continue;
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
                continue;
            }
        }

        // Paragraph / plain.
        auto spans = detail::inline_spans(body);
        if (!spans.empty()) out.emplace_back(Line{std::move(spans)});
    }

    return out;
}

}  // namespace markdown

#endif