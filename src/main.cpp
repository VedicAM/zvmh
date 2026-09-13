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

#include <sys/socket.h>
#include <arpa/inet.h>

#include "auth.h"
#include "remote/remote_agent.h"
#include "server/server.h"
#include "system/builtins.h"

using namespace std;

struct Args {
    string message;
    string cwd;
    string server_cmd;       // "", "start", "stop", "status"
    string addr = "127.0.0.1";
    int port = 5500;
    int poll_ms = 2000;
    bool session = false;
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
         << "      --session                Target the auto-provisioned session daemon instead\n"
         << "                               (port 5501, ~/.zvmh/session.pid); also used by the\n"
         << "                               solo-session spawn when no --connect is given\n"
         << "      --poll-ms <MS>           Filesystem poll interval (default 2000)\n"
         << "      --daemon                 Fork into the background (start only)\n"
         << "      --pidfile <FILE>         Override pidfile path (default ~/.zvmh/server.pid)\n"
         << "\n"
         << "Swarm client:\n"
         << "  --connect <HOST[:PORT]>      Join a swarm server; unusable at a session endpoint\n"
         << "                               that has no daemon yet, spawns one automatically\n";
}

filesystem::path default_dir() {
    const char* home = getenv("HOME");
    return home ? filesystem::path(home) / ".zvmh" : filesystem::path(".zvmh");
}

filesystem::path default_pidfile(const Args& a) {
    return a.pidfile.empty() ? default_dir() / "server.pid" : filesystem::path(a.pidfile);
}

filesystem::path session_pidfile() {
    return default_dir() / "session.pid";
}

filesystem::path default_logfile() {
    return default_dir() / "server.log";
}

filesystem::path session_logfile() {
    return default_dir() / "session.log";
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

// TCP connect probe: does anything answer on host:port right now?
bool port_alive(const string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<unsigned short>(port));
    bool ok = ::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) == 1 &&
              ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0;
    ::close(fd);
    return ok;
}

int run_server(const Args& a, const filesystem::path& pidfile,
               const filesystem::path& logfile) {
    int port = a.session ? swarm::kSessionPort : a.port;
    // Env var wins; otherwise fall back to the key stored by the TUI's
    // /connect command (~/.zvmh/auth.json).
    std::string key = auth::active_api_key();
    swarm::SwarmsServer server(a.addr, port, a.poll_ms, key, a.session);

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
        int logfd = ::open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
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
    filesystem::path pidfile = a.session ? session_pidfile() : default_pidfile(a);
    filesystem::path logfile = a.session ? session_logfile() : default_logfile();
    int port = a.session ? swarm::kSessionPort : a.port;
    pid_t existing = read_pid(pidfile);

    if (a.server_cmd == "start") {
        if (existing > 0 && process_alive(existing)) {
            cerr << "Error: server already running (pid " << existing << ")\n";
            return 1;
        }
        if (!a.daemon) {
            cerr << (a.session ? "Session server" : "Swarm server")
                 << " listening on " << a.addr << ":" << port
                 << " (Ctrl-C to stop; use --daemon to background)\n";
        } else {
            cout << (a.session ? "Session server" : "Swarm server")
                 << " starting on " << a.addr << ":" << port << " (pid " << getpid() << ")\n";
        }
        return run_server(a, pidfile, logfile);
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
            cout << (a.session ? "Session server" : "Swarm server")
                 << " is running (pid " << existing << ", " << a.addr << ":" << port << ")\n";
            return 0;
        }
        cout << "Server is not running\n";
        return 0;
    }
    cerr << "Error: unknown --server command '" << a.server_cmd << "'\n";
    return 1;
}

// Boot a per-session backend on kSessionPort if nothing answers there yet.
// The daemon inherits our cwd and the effective API key (env var winning over
// ~/.zvmh/auth.json), self-terminates when its last client disconnects, and
// keeps a pidfile for explicit `--server stop --session`. Runs fine without a
// key: the TUI opens and prompts the user to run /connect; prompts are
// rejected with a warning until a provider is configured.
int ensure_session_backend(const Args& a) {
    if (port_alive(swarm::kSessionHost, swarm::kSessionPort)) {
        return 0;
    }

    std::string key = auth::active_api_key();

    filesystem::path pidfile = session_pidfile();
    filesystem::create_directories(pidfile.parent_path());

    if (filesystem::exists(pidfile)) {
        pid_t stale = read_pid(pidfile);
        if (stale <= 0 || !process_alive(stale)) {
            filesystem::remove(pidfile);
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        cerr << "Error: fork failed: " << strerror(errno) << "\n";
        return 1;
    }
    if (pid > 0) {
        // Parent: wait for the daemon to start listening.
        for (int i = 0; i < 120; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (port_alive(swarm::kSessionHost, swarm::kSessionPort)) {
                return 0;
            }
        }
        cerr << "Error: session backend did not start on "
             << swarm::kSessionHost << ":" << swarm::kSessionPort << "\n";
        return 1;
    }

    // Child: the session daemon.
    setsid();
    int logfd = ::open(session_logfile().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
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
    swarm::SwarmsServer server(swarm::kSessionHost, swarm::kSessionPort, a.poll_ms, key, true);
    int rc = server.run();
    filesystem::remove(pidfile);
    ::_exit(rc);
    return rc;
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
        } else if (arg == "--session") {
            args.session = true;
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

std::string session_agent_name() {
    char hostbuf[256] = {0};
    gethostname(hostbuf, sizeof(hostbuf) - 1);
    return string("zvmh-") + hostbuf + "-" + to_string(getpid());
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

    // Every session is a thin client. Without --connect we auto-provision a
    // session daemon on 127.0.0.1:5501 that we attach to.
    if (args.connect_host.empty()) {
        if (ensure_session_backend(args) != 0) {
            return 1;
        }
        args.connect_host = swarm::kSessionHost;
        args.connect_port = swarm::kSessionPort;
    }

    filesystem::path cwd = filesystem::current_path();
    filesystem::path root = sysctx::find_vcs_root(cwd);
    string repo = (root.empty() ? cwd : root).string();

    remote::RemoteAgent agent(args.connect_host, args.connect_port);
    if (!agent.connect(repo, session_agent_name())) {
        cerr << "Error: could not connect to server at "
             << args.connect_host << ":" << args.connect_port << "\n";
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