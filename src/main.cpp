#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "agent.h"
#include "provider/openrouter.h"
#include "server/server.h"
#include "server/client.h"
#include "system/builtins.h"

using namespace std;

struct Args {
    string message;
    string cwd;
    string server_cmd;       // "", "start", "stop", "status"
    string addr = "127.0.0.1";
    int port = 5500;
    int poll_ms = 2000;
    bool daemon = false;
    string pidfile;
    string connect_host;     // set when --connect given
    int connect_port = 5500;
};

void print_usage() {
    cout << "Usage: zvmh [OPTIONS]\n"
         << "\n"
         << "Options:\n"
         << "  -m, --message <MESSAGE>      Initial prompt (if not provided, starts REPL)\n"
         << "  -C, --cwd <DIR>              Working directory\n"
         << "  -h, --help                   Show this help message\n"
         << "\n"
         << "Swarm server (no API key required):\n"
         << "  --server <start|stop|status> Operate the swarm daemon\n"
         << "      --addr <HOST>            Bind address (default 127.0.0.1)\n"
         << "      --port <PORT>            Bind port (default 5500)\n"
         << "      --poll-ms <MS>           Filesystem poll interval (default 2000)\n"
         << "      --daemon                 Fork into the background (start only)\n"
         << "      --pidfile <FILE>         Override pidfile path (default ~/.zvmh/server.pid)\n"
         << "\n"
         << "Swarm client:\n"
         << "  --connect <HOST[:PORT]>      Join a swarm server; usable with -m or TUI\n";
}

filesystem::path default_dir() {
    const char* home = getenv("HOME");
    return home ? filesystem::path(home) / ".zvmh" : filesystem::path(".zvmh");
}

filesystem::path default_pidfile(const Args& a) {
    return a.pidfile.empty() ? default_dir() / "server.pid" : filesystem::path(a.pidfile);
}

filesystem::path default_logfile() {
    return default_dir() / "server.log";
}

bool process_alive(pid_t pid) {
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

pid_t read_pid(const filesystem::path& pidfile) {
    if (!filesystem::exists(pidfile)) return -1;
    ifstream f(pidfile);
    long long pid;
    f >> pid;
    if (!f || pid <= 0) return -1;
    return static_cast<pid_t>(pid);
}

int run_server(const Args& a, const filesystem::path& pidfile) {
    swarm::SwarmsServer server(a.addr, a.port, a.poll_ms);

    if (a.daemon) {
        if (filesystem::exists(pidfile)) {
            pid_t existing = read_pid(pidfile);
            if (existing > 0 && process_alive(existing)) {
                cerr << "Error: server already running (pid " << existing << ")\n";
                return 1;
            }
            filesystem::remove(pidfile);
        }
        filesystem::create_directories(pidfile.parent_path());

        pid_t pid = fork();
        if (pid < 0) {
            cerr << "Error: fork failed: " << strerror(errno) << "\n";
            return 1;
        }
        if (pid > 0) {
            {
                ofstream pf(pidfile);
                pf << pid << std::flush;
            }
            return 0;  // parent returns to shell immediately
        }

        setsid();
        int logfd = ::open(default_logfile().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            ::close(logfd);
        }
        int nullfd = ::open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            ::close(nullfd);
        }
        {
            ofstream pf(pidfile);
            pf << getpid() << std::flush;
        }
        int rc = server.run();
        filesystem::remove(pidfile);
        return rc;
    }

    return server.run();
}

int cmd_server(const Args& a) {
    filesystem::path pidfile = default_pidfile(a);
    pid_t existing = read_pid(pidfile);

    if (a.server_cmd == "start") {
        if (existing > 0 && process_alive(existing)) {
            cerr << "Error: server already running (pid " << existing << ")\n";
            return 1;
        }
        if (!a.daemon) {
            cerr << "Swarm server listening on " << a.addr << ":" << a.port
                 << " (Ctrl-C to stop; use --daemon to background)\n";
        } else {
            cout << "Swarm server starting on " << a.addr << ":" << a.port << " (pid " << getpid() << ")\n";
        }
        return run_server(a, pidfile);
    }
    if (a.server_cmd == "stop") {
        if (existing <= 0 || !process_alive(existing)) {
            cout << "Server is not running\n";
            if (existing > 0) filesystem::remove(pidfile);
            return 0;
        }
        if (::kill(existing, SIGTERM) != 0) {
            cerr << "Error: kill(" << existing << "): " << strerror(errno) << "\n";
            return 1;
        }
        bool exited = false;
        for (int i = 0; i < 120; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!process_alive(existing)) {
                exited = true;
                break;
            }
        }
        if (exited) {
            filesystem::remove(pidfile);
            cout << "Server stopped\n";
            return 0;
        }
        cerr << "Error: server pid " << existing << " did not exit after SIGTERM; pidfile kept\n";
        return 1;
    }
    if (a.server_cmd == "status") {
        if (existing > 0 && process_alive(existing)) {
            cout << "Server is running (pid " << existing << ", " << a.addr << ":" << a.port << ")\n";
            return 0;
        }
        cout << "Server is not running\n";
        return 0;
    }
    cerr << "Error: unknown --server command '" << a.server_cmd << "'\n";
    return 1;
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
        } else if (arg == "--server") {
            if (i + 1 >= argc) {
                cerr << "Error: --server requires start|stop|status\n";
                return false;
            }
            args.server_cmd = argv[++i];
            if (args.server_cmd != "start" && args.server_cmd != "stop" && args.server_cmd != "status") {
                cerr << "Error: unknown --server command '" << args.server_cmd << "' (start|stop|status)\n";
                return false;
            }
        } else if (arg == "--addr") {
            if (i + 1 >= argc) return false;
            args.addr = argv[++i];
        } else if (arg == "--port") {
            if (i + 1 >= argc) return false;
            try {
                args.port = stoi(argv[++i]);
            } catch (...) {
                cerr << "Error: invalid --port value\n";
                return false;
            }
        } else if (arg == "--poll-ms") {
            if (i + 1 >= argc) return false;
            try {
                args.poll_ms = stoi(argv[++i]);
            } catch (...) {
                cerr << "Error: invalid --poll-ms value\n";
                return false;
            }
        } else if (arg == "--daemon") {
            args.daemon = true;
        } else if (arg == "--pidfile") {
            if (i + 1 >= argc) return false;
            args.pidfile = argv[++i];
        } else if (arg == "--connect") {
            if (i + 1 >= argc) {
                cerr << "Error: --connect requires HOST[:PORT]\n";
                return false;
            }
            string val = argv[++i];
            size_t colon = val.rfind(':');
            if (colon != string::npos) {
                args.connect_host = val.substr(0, colon);
                try {
                    args.connect_port = stoi(val.substr(colon + 1));
                } catch (...) {
                    cerr << "Error: invalid port in --connect " << val << "\n";
                    return false;
                }
            } else {
                args.connect_host = val;
            }
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

int join_swarm(const Args& args, Agent& agent) {
    auto client = make_unique<swarm::ServerClient>(args.connect_host, args.connect_port);
    filesystem::path cwd = filesystem::current_path();
    filesystem::path root = sysctx::find_vcs_root(cwd);
    string repo = (root.empty() ? cwd : root).string();

    char hostbuf[256] = {0};
    gethostname(hostbuf, sizeof(hostbuf) - 1);
    string name = string("zvmh-") + hostbuf + "-" + to_string(getpid());

    if (!client->connect(repo, name)) {
        cerr << "Error: could not connect to swarm server at " << args.connect_host << ":"
             << args.connect_port << "\n";
        return 1;
    }
    cerr << "Connected to swarm server at " << args.connect_host << ":" << args.connect_port
         << " (id " << client->id() << ")\n";
    agent.attach_swarm(std::move(client));
    return 0;
}

int main(int argc, char* argv[]) {
    Args args;

    if (!parse_args(argc, argv, args)) {
        return 1;
    }

    if (!args.server_cmd.empty()) {
        if (!args.cwd.empty()) {
            if (chdir(args.cwd.c_str()) != 0) {
                cerr << "Error: Could not change directory to '" << args.cwd << "'\n";
                return 1;
            }
        }
        return cmd_server(args);
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

    if (!args.connect_host.empty()) {
        if (join_swarm(args, agent) != 0) {
            return 1;
        }
        if (!args.message.empty()) {
            agent.set_swarm_realtime([](const string& line) {
                cerr << "  " << line << "\n";
            });
            return agent.run_once(args.message);
        }
        return agent.run_tui();
    }

    if (!args.message.empty()) {
        return agent.run_once(args.message);
    }

    return agent.run_tui();
}