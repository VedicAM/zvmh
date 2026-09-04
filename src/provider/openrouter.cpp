#include "openrouter.h"
#include <cpr/cpr.h>
#include <sstream>

OpenRouter::OpenRouter(const std::string& api_key) : Provider(api_key, "openai/gpt-4o") {}

const char* OpenRouter::name() const {
    return "openrouter";
}

ChatResponse OpenRouter::chat(const std::string& prompt) {
    nlohmann::json request_body = {
        {"model", model_},
        {"messages", {{{"role", "user"}, {"content", prompt}}}}
    };

    auto response = cpr::Post(
        cpr::Url{"https://openrouter.ai/api/v1/chat/completions"},
        cpr::Header{{"Authorization", "Bearer " + api_key_}},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{request_body.dump()}
    );

    auto json_response = nlohmann::json::parse(response.text);

    ChatResponse result;
    result.message = json_response["choices"][0]["message"]["content"];
    result.model = json_response["model"];
    result.prompt_tokens = json_response["usage"]["prompt_tokens"];
    result.completion_tokens = json_response["usage"]["completion_tokens"];

    last_usage_.prompt_tokens = result.prompt_tokens;
    last_usage_.completion_tokens = result.completion_tokens;

    return result;
}

EventStream OpenRouter::complete(
    const std::vector<Message>& messages,
    const std::vector<ToolDefinition>& tools,
    const std::string& system
) {
    nlohmann::json request_body = {
        {"model", model_},
        {"stream", true}
    };

    nlohmann::json api_messages = nlohmann::json::array();
    api_messages.push_back({{"role", "system"}, {"content", system}});

    for (const auto& msg : messages) {
        if (msg.role == Role::User) {
            for (const auto& block : msg.content) {
                if (auto* text = std::get_if<TextBlock>(&block)) {
                    api_messages.push_back({{"role", "user"}, {"content", text->text}});
                } else if (auto* tool_result = std::get_if<ToolResultBlock>(&block)) {
                    api_messages.push_back({
                        {"role", "tool"},
                        {"content", tool_result->content},
                        {"tool_call_id", tool_result->tool_use_id}
                    });
                }
            }
        } else if (msg.role == Role::Assistant) {
            std::string text_content;
            nlohmann::json tool_calls = nlohmann::json::array();

            for (const auto& block : msg.content) {
                if (auto* text = std::get_if<TextBlock>(&block)) {
                    text_content += text->text;
                } else if (auto* tool_use = std::get_if<ToolUseBlock>(&block)) {
                    tool_calls.push_back({
                        {"id", tool_use->id},
                        {"type", "function"},
                        {"function", {
                            {"name", tool_use->name},
                            {"arguments", tool_use->input.dump()}
                        }}
                    });
                }
            }

            nlohmann::json api_msg = {{"role", "assistant"}};
            if (!text_content.empty()) {
                api_msg["content"] = text_content;
            }
            if (!tool_calls.empty()) {
                api_msg["tool_calls"] = tool_calls;
            }
            api_messages.push_back(api_msg);
        }
    }
    request_body["messages"] = api_messages;

    if (!tools.empty()) {
        nlohmann::json api_tools = nlohmann::json::array();
        for (const auto& tool : tools) {
            api_tools.push_back({
                {"type", "function"},
                {"function", {
                    {"name", tool.name},
                    {"description", tool.description},
                    {"parameters", tool.input_schema}
                }}
            });
        }
        request_body["tools"] = api_tools;
    }

    auto response = cpr::Post(
        cpr::Url{"https://openrouter.ai/api/v1/chat/completions"},
        cpr::Header{{"Authorization", "Bearer " + api_key_}},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{request_body.dump()}
    );

    if (response.status_code != 200) {
        throw std::runtime_error("OpenRouter API error: " + std::to_string(response.status_code) + " " + response.text);
    }

    EventStream events;
    std::istringstream stream(response.text);
    std::string line;

    std::string current_tool_id;
    std::string current_tool_name;

    while (std::getline(stream, line)) {
        if (line.substr(0, 6) == "data: ") {
            std::string data = line.substr(6);

            if (data == "[DONE]") {
                events.push_back(MessageEndEvent{""});
                break;
            }

            try {
                auto chunk = nlohmann::json::parse(data);

                if (chunk.contains("usage") && !chunk["usage"].is_null()) {
                    last_usage_.prompt_tokens = chunk["usage"].value("prompt_tokens", 0);
                    last_usage_.completion_tokens = chunk["usage"].value("completion_tokens", 0);
                }

                if (chunk.contains("choices") && !chunk["choices"].empty()) {
                    auto& choice = chunk["choices"][0];

                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        if (!current_tool_id.empty()) {
                            current_tool_id.clear();
                            current_tool_name.clear();
                            events.push_back(ToolUseEndEvent{});
                        }
                        events.push_back(MessageEndEvent{choice["finish_reason"].get<std::string>()});
                    }

                    if (choice.contains("delta")) {
                        auto& delta = choice["delta"];

                        if (delta.contains("content") && !delta["content"].is_null()) {
                            events.push_back(TextDeltaEvent{delta["content"].get<std::string>()});
                        }

                        if (delta.contains("tool_calls")) {
                            for (auto& tc : delta["tool_calls"]) {
                                int index = tc.value("index", 0);

                                if (tc.contains("id") && !tc["id"].is_null()) {
                                    current_tool_id = tc["id"].get<std::string>();
                                    current_tool_name = tc.contains("function") && tc["function"].contains("name")
                                        ? tc["function"]["name"].get<std::string>()
                                        : "";
                                    events.push_back(ToolUseStartEvent{index, current_tool_id, current_tool_name});
                                }

                                if (tc.contains("function") && tc["function"].contains("arguments")
                                    && !tc["function"]["arguments"].is_null()) {
                                    events.push_back(ToolInputDeltaEvent{index, tc["function"]["arguments"].get<std::string>()});
                                }
                            }
                        }
                    }
                }
            } catch (const nlohmann::json::exception&) {
                // Skip malformed JSON
            }
        }
    }

    return events;
}
