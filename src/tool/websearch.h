#ifndef TOOL_WEBSEARCH_H
#define TOOL_WEBSEARCH_H

#include "tool.h"
#include <cpr/cpr.h>
#include <cstdlib>
#include <cstdint>
#include <sstream>

// Provider-independent local web search retained for launch parity. Invokes the
// legacy Exa/Parallel MCP product backends directly over JSON-RPC. Distinct
// from provider-hosted web search tools, which execute at the model provider.
class WebSearchTool : public Tool {
public:
    static constexpr const char* NO_RESULTS = "No search results found. Please try a different query.";
    static constexpr const char* EXA_URL = "https://mcp.exa.ai/mcp";
    static constexpr const char* PARALLEL_URL = "https://search.parallel.ai/mcp";
    static constexpr int MAX_NUM_RESULTS = 20;
    static constexpr int MAX_CONTEXT_CHARACTERS = 50000;
    static constexpr size_t MAX_RESPONSE_BYTES = 256 * 1024;
    static constexpr int TIMEOUT_MS = 25000;

    explicit WebSearchTool(std::string seed = "") : seed_(std::move(seed)) {}

    const char* name() const override { return "websearch"; }
    const char* description() const override {
        return R"(Search the web using the local websearch provider (Exa or Parallel) and return the relevant results as text.

WHEN TO USE
- current information beyond the model's knowledge cutoff: fresh news, prices, docs changes, or anything time-sensitive
- verifying facts that must be checked online before answering

WHEN NOT TO USE
- the answer is already in context or the repository: use grep/read first instead of a network round-trip
- the fact you need already appeared in the conversation: reuse it

DO NOT USE FOR
- fetching a specific URL's raw contents: this returns search snippets, not full-page dumps
- repository-local questions: the tool only searches the public web

USAGE
- query: required search string
- numResults: optional integer, default 8, maximum 20
- type: "auto" (default), "fast", or "deep"
- livecrawl: "fallback" (default) or "preferred"
- contextMaxCharacters: optional integer, default 10000, maximum 50000, only sent when provided
- provider: ZVMH_WEBSEARCH_PROVIDER ("exa"/"parallel") overrides; else first of ZVMH_ENABLE_PARALLEL / ZVMH_ENABLE_EXA; else a deterministic default
- API keys read from EXA_API_KEY / PARALLEL_API_KEY; both backends work without a key
- results capped at 256KB and 25s timeout; failures return "Error: ..."

EXAMPLES
- {"query": "zvmh architecture"}
- {"query": "best free LLM 2026", "numResults": 5, "type": "fast"}
- {"query": "OpenRouter news", "livecrawl": "preferred", "contextMaxCharacters": 15000})";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"query", {
                    {"type", "string"},
                    {"description", "Websearch query"}
                }},
                {"numResults", {
                    {"type", "integer"},
                    {"description", "Number of search results to return (default: 8, maximum: 20)"}
                }},
                {"livecrawl", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"fallback", "preferred"})},
                    {"description", "Live crawl mode - 'fallback': live crawling as backup if cached unavailable, 'preferred': prioritize live crawling (default: 'fallback')"}
                }},
                {"type", {
                    {"type", "string"},
                    {"enum", nlohmann::json::array({"auto", "fast", "deep"})},
                    {"description", "Search type - 'auto': balanced search (default), 'fast': quick results, 'deep': comprehensive search"}
                }},
                {"contextMaxCharacters", {
                    {"type", "integer"},
                    {"description", "Maximum characters for the context string optimized for the model (default: 10000, maximum: 50000)"}
                }}
            }},
            {"required", nlohmann::json::array({"query"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string query = input["query"].get<std::string>();
        int num_results = clamp_int(input.value("numResults", 8), 1, MAX_NUM_RESULTS);
        std::string livecrawl = input.value("livecrawl", "fallback");
        if (livecrawl != "fallback" && livecrawl != "preferred") livecrawl = "fallback";
        std::string type = input.value("type", "auto");
        if (type != "auto" && type != "fast" && type != "deep") type = "auto";
        int context_max = clamp_int(input.value("contextMaxCharacters", 10000), 1, MAX_CONTEXT_CHARACTERS);
        bool has_context_max = input.contains("contextMaxCharacters");

        std::string provider = select_provider();
        std::string text;
        if (provider == "exa") {
            nlohmann::json args = {
                {"query", query},
                {"type", type},
                {"numResults", num_results},
                {"livecrawl", livecrawl},
            };
            if (has_context_max) args["contextMaxCharacters"] = context_max;
            text = call_mcp(exa_url(), "web_search_exa", args, {});
        } else {
            nlohmann::json args = {
                {"objective", query},
                {"search_queries", nlohmann::json::array({query})},
                {"session_id", seed_.empty() ? "zvmh" : seed_},
            };
            std::map<std::string, std::string> headers;
            const char* api_key = getenv("PARALLEL_API_KEY");
            if (api_key && *api_key) {
                headers["Authorization"] = std::string("Bearer ") + api_key;
            }
            text = call_mcp(PARALLEL_URL, "web_search", args, headers);
        }

        if (text.empty()) return NO_RESULTS;
        return text;
    }

private:
    std::string seed_;

    static int clamp_int(int v, int lo, int hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    static bool env_truthy(const char* name) {
        const char* v = getenv(name);
        if (!v || !*v) return false;
        std::string s(v);
        return s != "0" && s != "false" && s != "FALSE" && s != "off";
    }

    static std::string select_provider() {
        const char* override = getenv("ZVMH_WEBSEARCH_PROVIDER");
        if (override) {
            std::string s(override);
            if (s == "exa" || s == "parallel") return s;
        }
        if (env_truthy("ZVMH_ENABLE_PARALLEL")) return "parallel";
        if (env_truthy("ZVMH_ENABLE_EXA")) return "exa";
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : std::string("zvmh-websearch")) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return (h % 2 == 0) ? "exa" : "parallel";
    }

    static std::string percent_encode(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                static const char* hex = "0123456789ABCDEF";
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return out;
    }

    static std::string exa_url() {
        const char* key = getenv("EXA_API_KEY");
        if (!key || !*key) return EXA_URL;
        return std::string(EXA_URL) + "?exaApiKey=" + percent_encode(key);
    }

    // Find the first text content item of an MCP result payload.
    static std::string payload_text(const std::string& json_str) {
        try {
            auto j = nlohmann::json::parse(json_str);
            auto& content = j.at("result").at("content");
            for (auto& item : content) {
                if (item.contains("text")) return item["text"].get<std::string>();
            }
        } catch (const std::exception&) {
        }
        return "";
    }

    static std::string trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    // MCP over HTTP may return a bare JSON-RPC body or an SSE frame body. A
    // payload is usable only when it is a JSON object; otherwise the hit is
    // skipped the same way opencode's parsePayload does.
    static std::string parse_response(const std::string& body) {
        std::string trimmed_body = trim(body);
        if (!trimmed_body.empty() && trimmed_body.front() == '{') {
            std::string direct = payload_text(trimmed_body);
            if (!direct.empty()) return direct;
        }
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("data: ", 0) != 0) continue;
            std::string text = payload_text(trim(line.substr(6)));
            if (!text.empty()) return text;
        }
        return "";
    }

    static std::string call_mcp(const std::string& url,
                                const std::string& tool_name,
                                const nlohmann::json& args,
                                const std::map<std::string, std::string>& extra_headers) {
        nlohmann::json body = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", {{"name", tool_name}, {"arguments", args}}}
        };

        cpr::Header headers = {
            {"Content-Type", "application/json"},
            {"Accept", "application/json, text/event-stream"}
        };
        for (const auto& [key, value] : extra_headers) {
            headers[key] = value;
        }

        cpr::Response response = cpr::Post(
            cpr::Url{url},
            headers,
            cpr::Body{body.dump()},
            cpr::Timeout{TIMEOUT_MS}
        );

        if (response.error) {
            return "Error: " + tool_name + " request failed: " + response.error.message;
        }
        if (response.status_code != 200) {
            return "Error: " + tool_name + " request failed with HTTP " + std::to_string(response.status_code);
        }
        if (response.text.size() > MAX_RESPONSE_BYTES) {
            return "Error: " + tool_name + " response exceeded " + std::to_string(MAX_RESPONSE_BYTES) + " bytes";
        }
        return parse_response(response.text);
    }
};

#endif