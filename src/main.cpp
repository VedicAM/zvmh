#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <unistd.h>

#include "agent.h"
#include "provider/openrouter.h"
#include "provider/claude.h"
#include "provider/codex.h"
#include "auth/claudeauth.h"
#include "auth/codexauth.h"

using namespace std;

enum class ProviderChoice {
    Claude,
    Codex,
    OpenRouter,
    Auto
};

struct Args {
    ProviderChoice provider = ProviderChoice::Auto;
    string message;
    string cwd;
};

void print_usage() {
    cout << "Usage: zvmh [OPTIONS]\n"
         << "\n"
         << "Options:\n"
         << "  -p, --provider <PROVIDER>  Provider to use (claude, codex, openrouter, auto)\n"
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
        } else if (arg == "-p" || arg == "--provider") {
            if (i + 1 >= argc) {
                cerr << "Error: " << arg << " requires a value\n";
                return false;
            }
            string value = argv[++i];
            if (value == "claude") {
                args.provider = ProviderChoice::Claude;
            } else if (value == "codex") {
                args.provider = ProviderChoice::Codex;
            } else if (value == "openrouter") {
                args.provider = ProviderChoice::OpenRouter;
            } else if (value == "auto") {
                args.provider = ProviderChoice::Auto;
            } else {
                cerr << "Error: Unknown provider '" << value << "'\n";
                return false;
            }
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

unique_ptr<Provider> create_provider(ProviderChoice choice) {
    switch (choice) {
        case ProviderChoice::Claude:
            try {
                auto creds = load_claude_credentials();
                return make_unique<Claude>(creds);
            } catch (const std::exception& e) {
                cerr << "Error: " << e.what() << "\n";
                return nullptr;
            }

        case ProviderChoice::Codex:
            try {
                auto creds = load_codex_credentials();
                return make_unique<Codex>(creds);
            } catch (const std::exception& e) {
                cerr << "Error: " << e.what() << "\n";
                return nullptr;
            }

        case ProviderChoice::OpenRouter: {
            const char* api_key = getenv("OPENROUTER_API_KEY");
            if (!api_key) {
                cerr << "Error: OPENROUTER_API_KEY environment variable not set\n";
                return nullptr;
            }
            return make_unique<OpenRouter>(api_key);
        }

        case ProviderChoice::Auto:
            try {
                auto creds = load_claude_credentials();
                cerr << "Using Claude provider\n";
                return make_unique<Claude>(creds);
            } catch (...) {
                try {
                    auto creds = load_codex_credentials();
                    cerr << "Using Codex provider\n";
                    return make_unique<Codex>(creds);
                } catch (...) {
                    const char* api_key = getenv("OPENROUTER_API_KEY");
                    if (api_key) {
                        cerr << "Using OpenRouter provider\n";
                        return make_unique<OpenRouter>(api_key);
                    }
                    cerr << "Error: No credentials found. Set up Claude, Codex, or set OPENROUTER_API_KEY.\n";
                    return nullptr;
                }
            }
    }

    return nullptr;
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

    auto provider = create_provider(args.provider);
    if (!provider) {
        return 1;
    }

    Agent agent(std::move(provider));

    if (!args.message.empty()) {
        return agent.run_once(args.message);
    }

    return agent.repl();
}
