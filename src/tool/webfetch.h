#ifndef TOOL_WEBFETCH_H
#define TOOL_WEBFETCH_H

#include "tool.h"
#include "../utf8.h"
#include <cpr/cpr.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

// Fetches a single HTTP/HTTPS URL and returns it as text, markdown, or HTML
// (markdown by default). HTML is converted with a small built-in parser (no DOM
// library), mirroring opencode's WebFetchTool (extractTextFromHTML +
// convertHTMLToMarkdown on top of htmlparser2/netadata turndown).

namespace webfetch {

inline constexpr size_t kMaxResponseBytes = 5 * 1024 * 1024;
inline constexpr size_t kMaxOutputBytes = 60 * 1024;
inline constexpr int kDefaultTimeoutSeconds = 30;
inline constexpr int kMaxTimeoutSeconds = 120;

inline constexpr const char* kBrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/143.0.0.0 Safari/537.36";
inline constexpr const char* kFallbackUserAgent = "opencode";

struct Response {
    std::string body;
    std::string content_type;
    bool ok = false;
};

inline std::string lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

inline bool is_http_url(const std::string& url) {
    const std::string u = lower_copy(url);
    return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
}

inline std::string accept_header(const std::string& format) {
    if (format == "markdown") {
        return "text/markdown;q=1.0, text/x-markdown;q=0.9, text/plain;q=0.8, text/html;q=0.7, */*;q=0.1";
    }
    if (format == "text") {
        return "text/plain;q=1.0, text/markdown;q=0.9, text/html;q=0.8, */*;q=0.1";
    }
    if (format == "html") {
        return "text/html;q=1.0, application/xhtml+xml;q=0.9, text/plain;q=0.8, "
               "text/markdown;q=0.7, */*;q=0.1";
    }
    return "*/*";
}

inline std::string mime_from(const std::string& content_type) {
    const size_t semi = content_type.find(';');
    std::string m = semi == std::string::npos ? content_type : content_type.substr(0, semi);
    m = lower_copy(trim(m));
    return m;
}

inline bool is_image_attachment(const std::string& mime) {
    return mime.rfind("image/", 0) == 0 && mime != "image/svg+xml" && mime != "image/vnd.fastbidsheet";
}

inline bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool is_textual_mime(const std::string& mime) {
    return mime.empty() || mime.rfind("text/", 0) == 0 || mime == "application/json" ||
           ends_with(mime, "+json") || mime == "application/xml" || ends_with(mime, "+xml") ||
           mime == "application/javascript" || mime == "application/x-javascript";
}

inline cpr::Response do_get(const std::string& url, const std::string& format, int timeout_seconds,
                            const std::string& user_agent) {
    cpr::Header headers = {
        {"User-Agent", user_agent},
        {"Accept", accept_header(format)},
        {"Accept-Language", "en-US,en;q=0.9"},
    };
    return cpr::Get(cpr::Url{url}, headers, cpr::Timeout{timeout_seconds * 1000});
}

inline bool is_cloudflare_challenge(const cpr::Response& response) {
    const auto it = response.header.find("cf-mitigated");
    return response.status_code == 403 && it != response.header.end() && it->second == "challenge";
}

inline bool fetch(const std::string& url, const std::string& format, int timeout_seconds, Response& out) {
    cpr::Response response = do_get(url, format, timeout_seconds, kBrowserUserAgent);
    if (response.error) return false;
    if (is_cloudflare_challenge(response)) {
        response = do_get(url, format, timeout_seconds, kFallbackUserAgent);
        if (response.error) return false;
    }
    if (response.status_code < 200 || response.status_code >= 300) return false;
    if (static_cast<size_t>(response.text.size()) > kMaxResponseBytes) return false;
    out.body = std::move(response.text);
    const auto ct = response.header.find("content-type");
    out.content_type = ct != response.header.end() ? ct->second : "";
    out.ok = true;
    return true;
}

inline std::string truncate_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

// ---- minimal HTML tree ----
struct HtmlNode {
    std::string tag;                                // lowercase tag name, "" for text nodes
    std::map<std::string, std::string> attrs;       // attribute name already lowercased
    std::vector<HtmlNode> children;
    std::string text;                               // raw text for text nodes
};

inline std::string attr(const HtmlNode& n, const std::string& key) {
    const auto it = n.attrs.find(lower_copy(key));
    return it != n.attrs.end() ? it->second : "";
}

inline bool is_void_tag(const std::string& tag) {
    static const std::set<std::string> voids = {
        "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr"};
    return voids.count(tag) != 0;
}

inline HtmlNode parse_html(const std::string& html) {
    HtmlNode root;
    std::vector<HtmlNode*> stack;
    stack.push_back(&root);
    const size_t n = html.size();
    size_t i = 0;
    while (i < n) {
        if (html[i] != '<') {
            size_t end = html.find('<', i);
            if (end == std::string::npos) end = n;
            if (end > i) {
                HtmlNode text;
                text.text = html.substr(i, end - i);
                stack.back()->children.push_back(std::move(text));
            }
            i = end;
            continue;
        }
        if (html.compare(i, 4, "<!--") == 0) {
            size_t end = html.find("-->", i + 4);
            i = end == std::string::npos ? n : end + 3;
            continue;
        }
        const size_t gt = html.find('>', i + 1);
        if (gt == std::string::npos) break;
        std::string raw = html.substr(i + 1, gt - (i + 1));
        i = gt + 1;
        if (raw.empty()) continue;
        if (raw[0] == '/') {
            const std::string tag = lower_copy(trim(raw.substr(1)));
            for (size_t k = stack.size(); k-- > 1;) {
                if (stack[k]->tag == tag) {
                    stack.resize(k);
                    break;
                }
            }
            continue;
        }
        if (raw[0] == '!') continue;
        const size_t name_end = raw.find_first_of(" \t\r\n/>");
        const std::string tag = lower_copy(raw.substr(0, name_end));
        if (tag.empty()) continue;
        HtmlNode node;
        node.tag = tag;
        size_t p = name_end;
        while (p < raw.size()) {
            const size_t st = raw.find_first_not_of(" \t\r\n", p);
            if (st == std::string::npos || raw[st] == '/') break;
            const size_t eq = raw.find('=', st);
            if (eq == std::string::npos) {
                size_t de = raw.find_first_of(" \t\r\n", st);
                if (de == std::string::npos) de = raw.size();
                node.attrs[lower_copy(raw.substr(st, de - st))] = "";
                p = de;
                continue;
            }
            const std::string key = lower_copy(raw.substr(st, eq - st));
            const size_t vs = raw.find_first_not_of(" \t\r\n", eq + 1);
            std::string val;
            size_t next;
            if (vs == std::string::npos) {
                val = "";
                next = eq + 1;
            } else {
                const char q = raw[vs];
                if (q == '"' || q == '\'') {
                    size_t close = raw.find(q, vs + 1);
                    if (close == std::string::npos) close = raw.size();
                    val = raw.substr(vs + 1, close - vs - 1);
                    next = close + 1;
                } else {
                    size_t ve = raw.find_first_of(" \t\r\n", vs);
                    if (ve == std::string::npos) ve = raw.size();
                    val = raw.substr(vs, ve - vs);
                    next = ve;
                }
            }
            node.attrs[key] = val;
            p = next;
        }
        stack.back()->children.push_back(std::move(node));
        if (!is_void_tag(tag)) {
            stack.push_back(&stack.back()->children.back());
        }
    }
    return root;
}

inline std::string collapse_ws(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            space = true;
        } else if (space && !out.empty()) {
            out += ' ';
            out += c;
            space = false;
        } else {
            out += c;
            space = false;
        }
    }
    return out;
}

inline bool is_skip_tag(const std::string& tag) {
    static const std::set<std::string> skips = {
        "script", "style", "meta", "link", "noscript", "iframe", "object", "embed",
        "head", "template", "title", "svg"};
    return skips.count(tag) != 0;
}

inline bool is_block_tag(const std::string& tag) {
    static const std::set<std::string> blocks = {
        "p", "div", "section", "article", "header", "footer", "main", "aside", "nav",
        "figure", "figcaption", "form", "fieldset", "address", "details", "summary",
        "blockquote", "pre", "ul", "ol", "li", "table", "thead", "tbody", "tfoot", "tr",
        "th", "td", "hr", "dl", "dt", "dd",
        "h1", "h2", "h3", "h4", "h5", "h6"};
    return blocks.count(tag) != 0;
}

inline std::string raw_text(const HtmlNode& n) {
    std::string out;
    for (const HtmlNode& c : n.children) {
        if (c.tag.empty()) {
            out += c.text;
        } else {
            out += raw_text(c);
        }
    }
    return out;
}

inline std::string render_inline(const HtmlNode& n);
inline std::string render_block(const HtmlNode& n);

inline std::string children_inline(const HtmlNode& n) {
    std::string out;
    for (const HtmlNode& c : n.children) {
        if (c.tag.empty()) {
            out += collapse_ws(c.text);
        } else if (is_block_tag(c.tag)) {
            out += "\n" + render_block(c) + "\n";
        } else {
            out += render_inline(c);
        }
    }
    return out;
}

inline std::string render_inline(const HtmlNode& n) {
    if (n.tag.empty()) return collapse_ws(n.text);
    const std::string& t = n.tag;
    if (is_skip_tag(t)) return "";
    if (t == "br") return "  \n";
    if (t == "img") {
        return "![" + attr(n, "alt") + "](" + attr(n, "src") + ")";
    }
    if (t == "a") {
        std::string inner = trim(collapse_ws(children_inline(n)));
        return "[" + inner + "](" + attr(n, "href") + ")";
    }
    if (t == "b" || t == "strong") return "**" + children_inline(n) + "**";
    if (t == "em" || t == "i") return "*" + children_inline(n) + "*";
    if (t == "del" || t == "s" || t == "strike") return "~~" + children_inline(n) + "~~";
    if (t == "code" || t == "kbd") return "`" + children_inline(n) + "`";
    if (t == "pre") return "\n" + render_block(n) + "\n";
    return children_inline(n);
}

inline std::string indent_block(const std::string& s, const std::string& prefix) {
    std::string out;
    size_t start = 0;
    while (start < s.size()) {
        size_t nl = s.find('\n', start);
        const std::string line = nl == std::string::npos ? s.substr(start) : s.substr(start, nl - start);
        if (!line.empty()) {
            if (!out.empty()) out += "\n";
            out += prefix + line;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

inline std::string render_list(const HtmlNode& n, bool ordered) {
    std::string out;
    int index = 1;
    for (const HtmlNode& c : n.children) {
        if (c.tag != "li") continue;
        std::string body;
        std::string nested;
        for (const HtmlNode& sub : c.children) {
            if (sub.tag == "ul" || sub.tag == "ol") {
                nested += indent_block(render_list(sub, sub.tag == "ol"), "  ");
            } else if (sub.tag.empty()) {
                body += collapse_ws(sub.text);
            } else if (is_block_tag(sub.tag)) {
                body += " " + render_block(sub);
            } else {
                body += render_inline(sub);
            }
        }
        const std::string marker = ordered ? std::to_string(index++) + ". " : "- ";
        out += marker + trim(body);
        if (!nested.empty()) out += "\n" + nested;
        out += "\n";
    }
    return out;
}

inline std::string render_code_block(const HtmlNode& n) {
    std::string lang;
    for (const HtmlNode& c : n.children) {
        if (c.tag == "code") {
            const std::string cls = attr(c, "class");
            const size_t pos = cls.find("language-");
            if (pos != std::string::npos) lang = cls.substr(pos + 9);
            break;
        }
    }
    std::string code = raw_text(n);
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
    return "```" + lang + "\n" + code + "\n```";
}

inline std::string render_quote(const HtmlNode& n) {
    const std::string text = trim(collapse_ws(children_inline(n)));
    std::string out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        const std::string line =
            nl == std::string::npos ? text.substr(start) : text.substr(start, nl - start);
        if (!line.empty()) {
            if (!out.empty()) out += "\n";
            out += "> " + line;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

inline std::string render_row(const HtmlNode& n) {
    std::vector<std::string> cells;
    for (const HtmlNode& c : n.children) {
        if (c.tag == "td" || c.tag == "th") {
            cells.push_back(trim(collapse_ws(children_inline(c))));
        }
    }
    std::string out;
    for (size_t j = 0; j < cells.size(); ++j) {
        if (j) out += " | ";
        out += cells[j];
    }
    return out + "\n";
}

inline std::string render_table(const HtmlNode& n) {
    std::string out;
    for (const HtmlNode& c : n.children) {
        if (c.tag == "tr") {
            out += render_row(c);
        } else if (c.tag == "thead" || c.tag == "tbody" || c.tag == "tfoot") {
            out += render_table(c);
        }
    }
    return out;
}

inline std::string render_block(const HtmlNode& n) {
    if (n.tag.empty()) return collapse_ws(n.text);
    const std::string& t = n.tag;
    if (is_skip_tag(t)) return "";
    if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
        return std::string(t[1] - '0', '#') + " " + trim(children_inline(n));
    }
    if (t == "hr") return "---";
    if (t == "pre") return render_code_block(n);
    if (t == "blockquote") return render_quote(n);
    if (t == "ul") return render_list(n, false);
    if (t == "ol") return render_list(n, true);
    if (t == "li") return "- " + trim(children_inline(n));
    if (t == "table") return render_table(n);
    if (t == "tr") return render_row(n);
    if (t == "th" || t == "td") return trim(children_inline(n));
    return trim(children_inline(n));
}

inline std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

inline std::string convert_html_to_markdown(const std::string& html) {
    const HtmlNode root = parse_html(html);
    std::vector<std::string> parts;
    for (const HtmlNode& c : root.children) {
        std::string part =
            (!c.tag.empty() && is_block_tag(c.tag)) ? render_block(c) : render_inline(c);
        part = trim(part);
        if (!part.empty()) parts.push_back(part);
    }
    return join(parts, "\n\n");
}

inline void collect_text(const HtmlNode& n, std::string& out) {
    for (const HtmlNode& c : n.children) {
        if (c.tag.empty()) {
            out += c.text;
        } else if (!is_skip_tag(c.tag)) {
            collect_text(c, out);
        }
    }
}

// Mirrors opencode's extractTextFromHTML: all visible text concatenated with no
// inserted separators, then trimmed.
inline std::string extract_text_from_html(const std::string& html) {
    const HtmlNode root = parse_html(html);
    std::string out;
    collect_text(root, out);
    return trim(out);
}

}  // namespace webfetch

class WebFetchTool : public Tool {
public:
    const char* name() const override { return "webfetch"; }

    const char* description() const override {
        return R"(Fetch content from an HTTP or HTTPS URL and return it as text, markdown, or HTML. Markdown is the default.

WHEN TO USE
- the answer lives on a specific page and search snippets are not enough: fetch the exact URL
- current docs or reference pages with a known URL need to be brought into context

WHEN NOT TO USE
- discovery across many results: use websearch to search and summarize instead
- the content is already in context or in the repository: reuse it, or use read for local files

DO NOT USE FOR
- anything that is not an http:// or https:// URL: local resources go through read/glob/ls
- downloading binary assets: non-textual content types are rejected

USAGE
- url: required, must use http:// or https://
- format: "text" (plain extracted text), "markdown" (default), or "html" (raw markup)
- timeout: optional integer seconds, default 30, maximum 120
- Accept and Accept-Language headers follow the chosen format; a Cloudflare 403 "challenge" retries once with a plain user agent
- response must be 2xx and under 5MB; non-textual content types are rejected with "Error: Unable to fetch <url>"; image/svg+xml is allowed
- text/html is converted per format (built-in parser: headings, paragraphs, lists, links, emphasis, code blocks, quotes, tables); non-HTML content is returned as-is
- results are UTF-8 sanitized; output over 60KB is replaced with a preview plus a truncation marker

EXAMPLES
- {"url": "https://example.com"}
- {"url": "https://example.com/docs", "format": "text", "timeout": 60})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"url", {
                    {"type", "string"},
                    {"description", "The HTTP or HTTPS URL to fetch content from"}
                }},
                {"format", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"text", "markdown", "html"})},
                    {"description", "The format to return the content in. Defaults to markdown."}
                }},
                {"timeout", {
                    {"type", "integer"},
                    {"description", "Optional timeout in seconds (maximum: 120)"}
                }}
            }},
            {"required", nlohmann::json::array({"url"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string url = input.value("url", "");
        std::string format = input.value("format", "markdown");
        if (format != "text" && format != "html" && format != "markdown") format = "markdown";
        int timeout = input.value("timeout", webfetch::kDefaultTimeoutSeconds);
        if (timeout < 1) timeout = 1;
        if (timeout > webfetch::kMaxTimeoutSeconds) timeout = webfetch::kMaxTimeoutSeconds;

        if (!webfetch::is_http_url(url)) return "Error: Unable to fetch " + url;

        webfetch::Response response;
        if (!webfetch::fetch(url, format, timeout, response)) return "Error: Unable to fetch " + url;

        const std::string mime = webfetch::mime_from(response.content_type);
        if (webfetch::is_image_attachment(mime) || !webfetch::is_textual_mime(mime)) {
            return "Error: Unable to fetch " + url;
        }

        const std::string text = utf8_sanitize(response.body);
        std::string output;
        if (response.content_type.find("text/html") == std::string::npos) {
            output = text;
        } else if (format == "markdown") {
            output = webfetch::convert_html_to_markdown(text);
        } else if (format == "text") {
            output = webfetch::extract_text_from_html(text);
        } else {
            output = text;
        }
        output = utf8_sanitize(output);

        if (output.size() > webfetch::kMaxOutputBytes) {
            output = webfetch::truncate_utf8(output, webfetch::kMaxOutputBytes) +
                     "\n... (preview truncated; complete content exceeds " +
                     std::to_string(webfetch::kMaxOutputBytes) + " bytes)";
        }
        return output;
    }
};

#endif