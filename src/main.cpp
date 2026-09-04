#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <unistd.h>

#include "agent.h"
#include "provider/openrouter.h"

using namespace std;

struct Args {
    string message;
    string cwd;
};

void print_usage() {
    cout << "Usage: zvmh [OPTIONS]\n"
         << "\n"
         << "Options:\n"
         << "  -m, --message <MESSAGE>    Initial prompt (if not provided, starts REPL)\n"
         << "  -C, --cwd <DIR>            Working directory\n"
         << "  -h, --help                 Show this help message\n";
}

bool parse_args(int argc, char* argv[], Args& args) {
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage();
            return false;
        } else if (arg == "-m" || arg == "--message") {
            if (i + 1 >= argc) {
                cerr << "Error: " << arg << " requires a value\n";
                return false;
            }
            args.message = argv[++i];
        } else if (arg == "-C" || arg == "--cwd") {
            if (i + 1 >= argc) {
                cerr << "Error: " << arg << " requires a value\n";
                return false;
            }
            args.cwd = argv[++i];
        } else {
            cerr << "Error: Unknown option '" << arg << "'\n";
            print_usage();
            return false;
        }
    }
    return true;
}

unique_ptr<Provider> create_provider() {
    const char* api_key = getenv("OPENROUTER_API_KEY");
    if (!api_key) {
        cerr << "Error: OPENROUTER_API_KEY environment variable not set\n";
        return nullptr;
    }
    return make_unique<OpenRouter>(api_key);
}

int main(int argc, char* argv[]) {
    Args args;

    if (!parse_args(argc, argv, args)) {
        return 1;
    }

    if (!args.cwd.empty()) {
        if (chdir(args.cwd.c_str()) != 0) {
            cerr << "Error: Could not change directory to '" << args.cwd << "'\n";
            return 1;
        }
    }

    auto provider = create_provider();
    if (!provider) {
        return 1;
    }

    Agent agent(std::move(provider));

    if (!args.message.empty()) {
        return agent.run_once(args.message);
    }

    return agent.run_tui();
}