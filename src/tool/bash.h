#ifndef TOOL_BASH_H
#define TOOL_BASH_H

#include "tool.h"
#include <array>
#include <chrono>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

class BashTool : public Tool {
public:
    static constexpr int DEFAULT_TIMEOUT_MS = 120000;
    static constexpr int MAX_TIMEOUT_MS = 600000;
    static constexpr size_t MAX_CAPTURE_BYTES = 1024 * 1024;

    const char* name() const override { return "bash"; }
    const char* description() const override {
        return "Execute a shell command and return its output";
    }

    nlohmann::json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"command", {
                    {"type", "string"},
                    {"description", "Shell command string to execute"}
                }},
                {"workdir", {
                    {"type", "string"},
                    {"description", "Working directory (default: current directory)"}
                }},
                {"timeout", {
                    {"type", "integer"},
                    {"description", "Timeout in milliseconds (default: 120000, max: 600000)"}
                }}
            }},
            {"required", nlohmann::json::array({"command"})}
        };
    }

    std::string execute(const nlohmann::json& input) override {
        std::string command = input["command"].get<std::string>();
        std::string workdir = input.value("workdir", ".");
        int timeout_ms = input.value("timeout", DEFAULT_TIMEOUT_MS);
        if (timeout_ms > MAX_TIMEOUT_MS) timeout_ms = MAX_TIMEOUT_MS;

        int pipefd[2];
        if (pipe(pipefd) != 0) {
            return "Error: Could not create pipe";
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            return "Error: Could not fork process";
        }

        if (pid == 0) {
            close(pipefd[0]);
            setpgid(0, 0);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);
            chdir(workdir.c_str());
            execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            _exit(127);
        }

        close(pipefd[1]);

        std::string result;
        result.reserve(MAX_CAPTURE_BYTES);
        auto start = std::chrono::steady_clock::now();
        bool timed_out = false;
        std::array<char, 4096> buffer;

        while (true) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(pipefd[0], &fds);

            auto elapsed = std::chrono::steady_clock::now() - start;
            auto remaining_ms = timeout_ms - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            if (remaining_ms <= 0) {
                timed_out = true;
                break;
            }

            struct timeval tv;
            tv.tv_sec = remaining_ms / 1000;
            tv.tv_usec = (remaining_ms % 1000) * 1000;

            int ready = select(pipefd[0] + 1, &fds, nullptr, nullptr, &tv);
            if (ready <= 0) {
                if (ready == 0) timed_out = true;
                break;
            }

            ssize_t n = read(pipefd[0], buffer.data(), buffer.size());
            if (n <= 0) break;
            result.append(buffer.data(), n);
        }

        close(pipefd[0]);

        if (timed_out) {
            kill(-pid, SIGTERM);
            usleep(100000);
            kill(-pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        } else {
            waitpid(pid, nullptr, 0);
        }

        bool truncated = result.size() > MAX_CAPTURE_BYTES;
        if (truncated) {
            result.resize(MAX_CAPTURE_BYTES);
        }

        if (result.empty() && !timed_out) {
            result = "(no output)";
        }

        if (timed_out) {
            result += "\nCommand timed out after " + std::to_string(timeout_ms) + "ms. Retry with a larger timeout if expected to take longer.";
        } else if (truncated) {
            result += "\n[output capture truncated at 1MB limit]";
        }

        return result;
    }
};

#endif