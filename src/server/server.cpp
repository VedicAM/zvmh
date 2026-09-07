#include "server.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <random>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

namespace swarm {

namespace {
volatile std::sig_atomic_t g_stop_signal = 0;

void handle_signal(int) {
    g_stop_signal = 1;
}

std::string random_hex_id() {
    static std::mt19937 rng{std::random_device{}() ^ static_cast<unsigned>(::getpid())};
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.reserve(4);
    for (int i = 0; i < 4; ++i) s += hex[rng() & 15];
    return s;
}
}  // namespace

int SwarmsServer::listen_socket() {
    std::string port_str = std::to_string(port_);
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host_.empty() ? nullptr : host_.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "Error: getaddrinfo(" << host_ << ":" << port_ << "): " << gai_strerror(rc) << "\n";
        return -1;
    }

    int sfd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        sfd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (sfd < 0) continue;
        int one = 1;
        ::setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::bind(sfd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(sfd, SOMAXCONN) == 0) {
            break;
        }
        ::close(sfd);
        sfd = -1;
    }
    freeaddrinfo(res);
    if (sfd < 0) {
        std::cerr << "Error: could not bind " << host_ << ":" << port_ << "\n";
    }
    return sfd;
}

void SwarmsServer::refresh_watch_locked(const std::string& path) {
    WatchedFile& wf = file_state_[path];
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        wf.watched = true;
        wf.exists = false;
        wf.mtime = 0;
        wf.size = 0;
        wf.hash.clear();
        return;
    }
    wf.watched = true;
    wf.exists = true;
    wf.mtime = static_cast<uint64_t>(st.st_mtime);
    wf.size = static_cast<uint64_t>(st.st_size);
    wf.hash = hash_file(path);
}

void SwarmsServer::update_conflict_state_locked(const std::string& path, const std::string& new_hash,
                                                const std::string& by, const std::string&,
                                                std::vector<std::string>& notify_out) {
    auto& reads = read_hashes_[path];
    std::vector<std::string> ids;
    for (auto& [id, h] : reads) ids.push_back(id);
    for (auto& id : ids) {
        const std::string& had = reads[id];
        if (id != by && had != new_hash) notify_out.push_back(id);
        reads[id] = new_hash;
    }
    refresh_watch_locked(path);
}

void SwarmsServer::notify_file_changed(const std::string& path, const std::string& new_hash,
                                       const std::string& by, const std::string& at) {
    std::vector<std::string> notify;
    {
        std::lock_guard<std::mutex> lk(mu_);
        update_conflict_state_locked(path, new_hash, by, at, notify);
    }
    for (auto& id : notify) {
        send_to(id, conflict_notice(path, by, at, new_hash));
    }
}

void SwarmsServer::send_to(const std::string& id, const nlohmann::json& msg) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = clients_.find(id);
        if (it == clients_.end() || it->second.fd < 0) return;
        fd = it->second.fd;
    }
    std::lock_guard<std::mutex> lk(send_mu_);
    send_json(fd, msg);
}

void SwarmsServer::remove_client_locked(const std::string& id) {
    auto it = clients_.find(id);
    if (it == clients_.end()) return;
    const std::string& repo = it->second.repo;
    for (auto& [path, reads] : read_hashes_) {
        reads.erase(id);
    }
    auto members = repo_members_.find(repo);
    if (members != repo_members_.end()) {
        auto& vec = members->second;
        vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
        if (vec.empty()) repo_members_.erase(members);
    }
    clients_.erase(it);
}

void SwarmsServer::handle_client(int fd) {
    set_nosigpipe(fd);
    LineReader reader(fd, &closing_);

    std::string line;
    if (reader.next_line(line) != ReadResult::Ok) {
        std::lock_guard<std::mutex> lk(send_mu_);
        ::close(fd);
        return;
    }

    nlohmann::json first;
    try {
        first = nlohmann::json::parse(line);
    } catch (...) {
        std::lock_guard<std::mutex> lk(send_mu_);
        ::close(fd);
        return;
    }

    if (type_of(first) != "hello") {
        {
            std::lock_guard<std::mutex> lk(send_mu_);
            send_json(fd, bye_msg());
        }
        std::lock_guard<std::mutex> lk(send_mu_);
        ::close(fd);
        return;
    }

    std::string name = first.value("name", "");
    std::string repo = first.value("repo", "");
    if (name.empty() || repo.empty() || name.size() > 64) {
        {
            std::lock_guard<std::mutex> lk(send_mu_);
            send_json(fd, bye_msg());
        }
        std::lock_guard<std::mutex> lk(send_mu_);
        ::close(fd);
        return;
    }

    std::string id = random_hex_id();
    std::vector<nlohmann::json> peers;
    {
        std::lock_guard<std::mutex> lk(mu_);
        clients_[id] = Client{id, name, repo, fd};
        repo_members_[repo].push_back(id);
        for (auto& [pid, pc] : clients_) {
            peers.push_back(agent_info(pid, pc.name, pc.repo));
        }
    }
    send_to(id, hello_ack(id, nlohmann::json(peers)));

    for (;;) {
        if (reader.next_line(line) != ReadResult::Ok) break;
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(line);
        } catch (...) {
            break;
        }
        std::string t = type_of(msg);
        if (t == "read") {
            std::string path = msg.value("path", "");
            std::string hash = msg.value("hash", "");
            if (path.empty()) continue;
            std::string canon = canonical_path(repo, path);
            std::lock_guard<std::mutex> lk(mu_);
            read_hashes_[canon][id] = hash;
            refresh_watch_locked(canon);
        } else if (t == "write") {
            std::string path = msg.value("path", "");
            std::string hash = msg.value("hash", "");
            if (path.empty()) continue;
            notify_file_changed(canonical_path(repo, path), hash, id, now_iso());
        } else if (t == "msg") {
            std::string to = msg.value("to", "");
            std::string text = msg.value("text", "");
            if (to.empty() || text.empty()) continue;
            std::vector<std::string> targets;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (to == "all") {
                    for (auto& [pid, pc] : clients_) {
                        if (pid != id) targets.push_back(pid);
                    }
                } else if (to == "repo") {
                    auto members = repo_members_.find(repo);
                    if (members != repo_members_.end()) {
                        for (auto& pid : members->second) {
                            if (pid != id) targets.push_back(pid);
                        }
                    }
                } else {
                    if (clients_.count(to) && to != id) targets.push_back(to);
                }
            }
            std::string at = now_iso();
            for (auto& tid : targets) {
                send_to(tid, msg_notice(name, text, at));
            }
        } else if (t == "agents") {
            std::vector<nlohmann::json> agents;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& [pid, pc] : clients_) {
                    agents.push_back(agent_info(pid, pc.name, pc.repo));
                }
            }
            send_to(id, agents_list(agents));
        } else if (t == "ping") {
            send_to(id, pong_msg());
        } else if (t == "bye") {
            send_to(id, bye_msg());
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        remove_client_locked(id);
    }
    std::lock_guard<std::mutex> lk(send_mu_);
    ::close(fd);
}

void SwarmsServer::poller_loop() {
    while (!closing_.load()) {
        int elapsed = 0;
        while (elapsed < poll_ms_ && !closing_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            elapsed += 250;
        }
        if (closing_.load()) break;

        struct Changed {
            std::string path;
            std::string hash;
            std::string at;
        };
        std::vector<Changed> changed;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [path, wf] : file_state_) {
                if (!wf.watched) continue;
                struct stat st;
                if (::stat(path.c_str(), &st) != 0) {
                    if (wf.exists) {
                        wf.exists = false;
                        wf.mtime = 0;
                        wf.size = 0;
                        wf.hash.clear();
                    }
                    continue;
                }
                bool dirty = !wf.exists ||
                             static_cast<uint64_t>(st.st_mtime) != wf.mtime ||
                             static_cast<uint64_t>(st.st_size) != wf.size;
                std::string old_hash = wf.hash;
                wf.exists = true;
                wf.mtime = static_cast<uint64_t>(st.st_mtime);
                wf.size = static_cast<uint64_t>(st.st_size);
                if (dirty || wf.hash.empty()) {
                    wf.hash = hash_file(path);
                }
                if (!old_hash.empty() && !wf.hash.empty() && wf.hash != old_hash) {
                    changed.push_back({path, wf.hash, now_iso()});
                }
            }
        }
        for (auto& c : changed) {
            notify_file_changed(c.path, c.hash, "fs", c.at);
        }
    }
}

void SwarmsServer::shutdown_clients() {
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, c] : clients_) {
            if (c.fd >= 0) fds.push_back(c.fd);
        }
    }
    {
        std::lock_guard<std::mutex> lk(send_mu_);
        for (int fd : fds) send_json(fd, shutdown_msg());
        for (int fd : fds) ::close(fd);
    }
}

int SwarmsServer::run() {
    int sfd = listen_socket();
    if (sfd < 0) return 1;

    g_stop_signal = 0;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);

    poller_ = std::thread([this] { poller_loop(); });

    while (!g_stop_signal) {
        struct pollfd pfd;
        pfd.fd = sfd;
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (pfd.revents & POLLIN) {
            int cfd = ::accept(sfd, nullptr, nullptr);
            if (cfd >= 0) {
                std::thread t(&SwarmsServer::handle_client, this, cfd);
                t.detach();
            }
        }
    }

    closing_ = true;
    if (poller_.joinable()) poller_.join();
    shutdown_clients();
    ::close(sfd);

    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    ::sigaction(SIGTERM, &dfl, nullptr);
    ::sigaction(SIGINT, &dfl, nullptr);

    return 0;
}

}  // namespace swarm