#include "openrouter.h"
#include <cpr/cpr.h>

OpenRouter::OpenRouter(const std::string& api_key) : Provider(api_key, "openai/gpt-4o") {}

static std::string api_url() {
    const char* base = getenv("OPENROUTER_BASE_URL");
    return base ? base : "https://openrouter.ai/api/v1/chat/completions";
}

const char* OpenRouter::name() const {
    return "openrouter";
}

ChatResponse OpenRouter::chat(const std::string& prompt) {
    nlohmann::json request_body = {
        {"model", model_},
        {"messages", {{{"role", "user"}, {"content", prompt}}}}
    };

    auto response = cpr::Post(
        cpr::Url{api_url()},
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

void OpenRouter::emit_sse_data(
    const std::string& data,
    EventSink& sink,
    std::string& current_tool_id,
    std::string& current_tool_name
) {
    if (data == "[DONE]") {
        sink.on_event(MessageEndEvent{""});
        return;
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
                    sink.on_event(ToolUseEndEvent{});
                }
                sink.on_event(MessageEndEvent{choice["finish_reason"].get<std::string>()});
            }

            if (choice.contains("delta")) {
                auto& delta = choice["delta"];

                if (delta.contains("content") && !delta["content"].is_null()) {
                    sink.on_event(TextDeltaEvent{delta["content"].get<std::string>()});
                }

                if (delta.contains("tool_calls")) {
                    for (auto& tc : delta["tool_calls"]) {
                        int index = tc.value("index", 0);

                        if (tc.contains("id") && !tc["id"].is_null()) {
                            current_tool_id = tc["id"].get<std::string>();
                            current_tool_name = tc.contains("function") && tc["function"].contains("name")
                                ? tc["function"]["name"].get<std::string>()
                                : "";
                            sink.on_event(ToolUseStartEvent{index, current_tool_id, current_tool_name});
                        }

                        if (tc.contains("function") && tc["function"].contains("arguments")
                            && !tc["function"]["arguments"].is_null()) {
                            sink.on_event(ToolInputDeltaEvent{index, tc["function"]["arguments"].get<std::string>()});
                        }
                    }
                }
            }
        }
    } catch (const nlohmann::json::exception&) {
        // Skip malformed JSON
    }
}

void OpenRouter::complete(
    const std::vector<Message>& messages,
    const std::vector<ToolDefinition>& tools,
    const std::string& system,
    EventSink& sink
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

    cpr::Session session;
    session.SetUrl(cpr::Url{api_url()});
    session.SetHeader(cpr::Header{
        {"Authorization", "Bearer " + api_key_},
        {"Content-Type", "application/json"},
    });
    session.SetBody(cpr::Body{request_body.dump()});

    std::string pending_line;
    std::string current_tool_id;
    std::string current_tool_name;
    std::string error_tail;
    constexpr size_t kErrorTail = 16384;

    session.SetWriteCallback(cpr::WriteCallback([&](std::string_view data, intptr_t) {
        error_tail.append(data);
        if (error_tail.size() > kErrorTail) {
            error_tail = error_tail.substr(error_tail.size() - kErrorTail);
        }

        pending_line.append(data);
        size_t pos;
        while ((pos = pending_line.find('\n')) != std::string::npos) {
            std::string line = pending_line.substr(0, pos);
            pending_line.erase(0, pos + 1);

            if (line.substr(0, 6) == "data: ") {
                emit_sse_data(line.substr(6), sink, current_tool_id, current_tool_name);
            }
        }
        return true;
    }));

    auto response = session.Post();

    if (response.error) {
        throw std::runtime_error("OpenRouter request error: " + response.error.message
            + (error_tail.empty() ? "" : " " + error_tail));
    }
    if (response.status_code != 200) {
        throw std::runtime_error(
            "OpenRouter API error: " + std::to_string(response.status_code) + " " + error_tail);
    }
}
