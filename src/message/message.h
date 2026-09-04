#ifndef MESSAGE_H
#define MESSAGE_H

#include <string>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>

enum class Role {
    User,
    Assistant,
    System
};

struct TextBlock {
    std::string text;
};

struct ToolUseBlock {
    std::string id;
    std::string name;
    nlohmann::json input;
};

struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
};

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock>;

struct Message {
    Role role;
    std::vector<ContentBlock> content;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
};

enum class StreamEventType {
    TextDelta,
    ToolUseStart,
    ToolInputDelta,
    ToolUseEnd,
    MessageEnd
};

struct TextDeltaEvent {
    std::string text;
};

struct ToolUseStartEvent {
    int index;
    std::string id;
    std::string name;
};

struct ToolInputDeltaEvent {
    int index;
    std::string input;
};

struct ToolUseEndEvent {};

struct MessageEndEvent {
    std::string stop_reason;
};

using StreamEvent = std::variant<
    TextDeltaEvent,
    ToolUseStartEvent,
    ToolInputDeltaEvent,
    ToolUseEndEvent,
    MessageEndEvent
>;

using EventStream = std::vector<StreamEvent>;

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void on_event(const StreamEvent& event) = 0;
};

#endif
