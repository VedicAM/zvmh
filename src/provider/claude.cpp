#include "claude.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <sstream>

const char* API_URL = "https://api.anthropic.com/v1/messages";
const char* API_VERSION = "2023-06-01";
const char* BETA_HEADER = "claude-code-20250219,oauth-2025-04-20,interleaved-thinking-2025-05-14";

Claude::Claude(const ClaudeCredentials& credentials)
    : Provider(credentials.access_token, "claude-sonnet-4-20250514"), credentials_(credentials) {}

const char* Claude::name() const {
    return "claude";
}

nlohmann::json Claude::convert_messages(const std::vector<Message>& messages) {
    nlohmann::json result = nlohmann::json::array();

    for (const auto& msg : messages) {
        std::string role = (msg.role == Role::User) ? "user" : "assistant";
        nlohmann::json content = nlohmann::json::array();

        for (const auto& block : msg.content) {
            if (auto* text = std::get_if<TextBlock>(&block)) {
                content.push_back({{"type", "text"}, {"text", text->text}});
            } else if (auto* tool_use = std::get_if<ToolUseBlock>(&block)) {
                content.push_back({
                    {"type", "tool_use"},
                    {"id", tool_use->id},
                    {"name", tool_use->name},
                    {"input", tool_use->input}
                });
            } else if (auto* tool_result = std::get_if<ToolResultBlock>(&block)) {
                content.push_back({
                    {"type", "tool_result"},
                    {"tool_use_id", tool_result->tool_use_id},
                    {"content", tool_result->content}
                });
            }
        }

        result.push_back({{"role", role}, {"content", content}});
    }

    return result;
}

nlohmann::json Claude::convert_tools(const std::vector<ToolDefinition>& tools) {
    nlohmann::json result = nlohmann::json::array();

    for (const auto& tool : tools) {
        result.push_back({
            {"name", tool.name},
            {"description", tool.description},
            {"input_schema", tool.input_schema}
        });
    }

    return result;
}

ChatResponse Claude::chat(const std::string& prompt) {
    nlohmann::json request_body = {
        {"model", model_},
        {"max_tokens", 8192},
        {"messages", {{{"role", "user"}, {"content", prompt}}}}
    };

    auto response = cpr::Post(
        cpr::Url{API_URL},
        cpr::Header{{"x-api-key", credentials_.access_token}},
        cpr::Header{{"anthropic-version", API_VERSION}},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{request_body.dump()}
    );

    auto json_response = nlohmann::json::parse(response.text);

    ChatResponse result;
    result.message = json_response["content"][0]["text"].get<std::string>();
    result.model = json_response["model"].get<std::string>();
    result.prompt_tokens = json_response["usage"]["input_tokens"].get<int>();
    result.completion_tokens = json_response["usage"]["output_tokens"].get<int>();

    last_usage_.prompt_tokens = result.prompt_tokens;
    last_usage_.completion_tokens = result.completion_tokens;

    return result;
}

EventStream Claude::complete(
    const std::vector<Message>& messages,
    const std::vector<ToolDefinition>& tools,
    const std::string& system
) {
    nlohmann::json request_body = {
        {"model", model_},
        {"max_tokens", 8192},
        {"system", system},
        {"messages", convert_messages(messages)},
        {"stream", true}
    };

    if (!tools.empty()) {
        request_body["tools"] = convert_tools(tools);
    }

    auto response = cpr::Post(
        cpr::Url{API_URL},
        cpr::Header{{"x-api-key", credentials_.access_token}},
        cpr::Header{{"anthropic-version", API_VERSION}},
        cpr::Header{{"anthropic-beta", BETA_HEADER}},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{request_body.dump()}
    );

    if (response.status_code != 200) {
        throw std::runtime_error("Claude API error: " + std::to_string(response.status_code) + " " + response.text);
    }

    EventStream events;
    std::istringstream stream(response.text);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.substr(0, 6) == "data: ") {
            std::string data = line.substr(6);

            try {
                auto chunk = nlohmann::json::parse(data);
                std::string type = chunk.value("type", "");

                if (type == "message_start") {
                    if (chunk.contains("message") && chunk["message"].contains("usage")) {
                        last_usage_.prompt_tokens = chunk["message"]["usage"].value("input_tokens", 0);
                        last_usage_.completion_tokens = chunk["message"]["usage"].value("output_tokens", 0);
                    }
                } else if (type == "content_block_start") {
                    auto& block = chunk["content_block"];
                    std::string block_type = block.value("type", "");
                    if (block_type == "tool_use") {
                        events.push_back(ToolUseStartEvent{
                            0,
                            block["id"].get<std::string>(),
                            block["name"].get<std::string>()
                        });
                    }
                } else if (type == "content_block_delta") {
                    auto& delta = chunk["delta"];
                    std::string delta_type = delta.value("type", "");
                    if (delta_type == "text_delta") {
                        events.push_back(TextDeltaEvent{delta["text"].get<std::string>()});
                    } else if (delta_type == "input_json_delta") {
                        events.push_back(ToolInputDeltaEvent{0, delta["partial_json"].get<std::string>()});
                    }
                } else if (type == "content_block_stop") {
                    events.push_back(ToolUseEndEvent{});
                } else if (type == "message_delta") {
                    auto& delta = chunk["delta"];
                    if (delta.contains("stop_reason") && !delta["stop_reason"].is_null()) {
                        events.push_back(MessageEndEvent{delta["stop_reason"].get<std::string>()});
                    }
                    if (chunk.contains("usage") && chunk["usage"].contains("output_tokens")) {
                        last_usage_.completion_tokens = chunk["usage"]["output_tokens"].get<int>();
                    }
                } else if (type == "message_stop") {
                    events.push_back(MessageEndEvent{""});
                }
            } catch (const nlohmann::json::exception&) {
                // Skip malformed JSON
            }
        }
    }

    return events;
}
