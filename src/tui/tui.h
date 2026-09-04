#ifndef TUI_H
#define TUI_H

#include <memory>

class Agent;

class Tui {
public:
    explicit Tui(Agent& agent);
    ~Tui();
    int run();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

#endif