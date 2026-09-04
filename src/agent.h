#ifndef AGENT_H
#define AGENT_H

#include <memory>
#include <vector>
#include "provider/provider.h"
#include "message/message.h"
#include "tool/registry.h"

class Agent {
private:
    std::unique_ptr<Provider> provider_;
    std::vector<Message> messages_;
    std::string system_prompt_;
    Registry registry_;

    int run_turn(const std::string& prompt);
    void handle_command(const std::string& input);

public:
    explicit Agent(std::unique_ptr<Provider> provider);
    int run_once(const std::string& prompt);
    int repl();
    void add_message(const Message& message);
    void set_system_prompt(const std::string& prompt);
};

#endif