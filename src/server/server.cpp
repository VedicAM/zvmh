#include "server.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <random>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "../provider/openrouter.h"
#include "wire_sink.h"

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

// Maps a wire provider id to a fresh provider instance. Only ids registered in
// src/auth.h supported_providers() construct something; unknown ids return
// nullptr so the caller can report the problem instead of silently ignoring it.
std::unique_ptr<OpenRouter> make_provider(const std::string& provider,
                                          const std::string& api_key) {
    if (provider != "openrouter" || api_key.empty()) return nullptr;
    return std::make_unique<OpenRouter>(api_key);
}
}  // namespace

// ---------------------------------------------------------------------------
// PeerView — in-process swarm view for a server-hosted agent.
// ---------------------------------------------------------------------------

SwarmsServer::PeerView::PeerView(SwarmsServer* owner, std::string client_id)
    : owner_(owner), client_id_(std::move(client_id)) {}

void SwarmsServer::PeerView::set_handler(MessageHandler handler) {
    std::lock_guard<std::mutex> lk(mu_);
    handler_ = std::move(handler);
}

void SwarmsServer::PeerView::inject(const std::string& type, const nlohmann::json& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (handler_) handler_(type, msg);
}

bool SwarmsServer::PeerView::knows_peer(const std::string& id) const {
    std::lock_guard<std::mutex> lk(owner_->mu_);
    return id != client_id_ && owner_->clients_.count(id) != 0;
}

bool SwarmsServer::PeerView::send_message(const std::string& to, const std::string& text) {
    if (to.empty() || text.empty()) return false;
    owner_->route_message(client_id_, to, text);
    return true;
}

bool SwarmsServer::PeerView::request_peers() {
    return true;
}

std::vector<nlohmann::json> SwarmsServer::PeerView::peers() const {
    std::lock_guard<std::mutex> lk(owner_->mu_);
    std::vector<nlohmann::json> out;
    for (const auto& [pid, pc] : owner_->clients_) {
        if (pid != client_id_) out.push_back(agent_info(pid, pc.name, pc.repo));
    }
    return out;
}

bool SwarmsServer::PeerView::register_read(const std::string& path) {
    std::string hash = hash_file(path);
    if (hash.empty()) return false;
    std::lock_guard<std::mutex> lk(owner_->mu_);
    owner_->read_hashes_[path][client_id_] = hash;
    owner_->refresh_watch_locked(path);
    return true;
}

bool SwarmsServer::PeerView::report_write(const std::string& path) {
    std::string hash = hash_file(path);
    if (hash.empty()) return false;
    owner_->notify_file_changed(path, hash, client_id_, now_iso());
    return true;
}

// ---------------------------------------------------------------------------
// Socket plumbing.
// ---------------------------------------------------------------------------

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
    nlohmann::json notice = conflict_notice(path, by, at, new_hash);
    for (const auto& id : notify) {
        deliver_inbox(id, "conflict", notice);
    }
}

bool SwarmsServer::send_to(const std::string& id, const nlohmann::json& msg) {
    int fd = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = clients_.find(id);
        if (it == clients_.end() || it->second.fd < 0) return false;
        fd = it->second.fd;
    }
    std::lock_guard<std::mutex> lk(send_mu_);
    return send_json(fd, msg);
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

    auto ait = agents_.find(id);
    if (ait != agents_.end() && !ait->second.busy) {
        agents_.erase(ait);
    }
}

// ---------------------------------------------------------------------------
// Agent-slot lifecycle (server-hosted runtime).
// ---------------------------------------------------------------------------

void SwarmsServer::hello_register(const std::string& id, const std::string& name,
                                   const std::string& repo, int fd,
                                   std::vector<nlohmann::json>& peers) {
    clients_[id] = Client{id, name, repo, fd};
    repo_members_[repo].push_back(id);
    for (const auto& [pid, pc] : clients_) {
        peers.push_back(agent_info(pid, pc.name, pc.repo));
    }
}

void SwarmsServer::create_agent_slot(const std::string& id, const std::string& repo) {
    std::string api_key;
    {
        std::lock_guard<std::mutex> lk(mu_);
        api_key = api_key_;
    }
    std::unique_ptr<OpenRouter> provider = make_provider("openrouter", api_key);

    AgentSlot slot;
    slot.agent = std::make_unique<Agent>(std::move(provider));
    slot.agent->set_tool_cwd_base(repo);
    slot.agent->attach_swarm(std::make_unique<PeerView>(this, id), true);

    std::lock_guard<std::mutex> lk(mu_);
    agents_[id] = std::move(slot);
    agents_[id].peer_view = nullptr;  // owned by the Agent's swarm_ member
}

void SwarmsServer::send_state(const std::string& id) {
    std::string provider;
    std::string model;
    std::vector<nlohmann::json> tools;
    int ctx_total = -1;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto ait = agents_.find(id);
        if (ait == agents_.end()) return;
        Agent& agent = *ait->second.agent;
        provider = agent.provider_name();
        model = agent.model();
        ctx_total = agent.context_window();
        for (const auto& def : agent.tools()) {
            tools.push_back({{"name", def.name},
                             {"description", def.description},
                             {"parameters", def.input_schema}});
        }
    }
    send_to(id, state_packet(provider, model, tools, ctx_total));
}

void SwarmsServer::clear_agent_history(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto ait = agents_.find(id);
    if (ait == agents_.end()) return;
    ait->second.agent->clear_messages();
}

void SwarmsServer::set_agent_model(const std::string& id, const std::string& model) {
    if (model.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto ait = agents_.find(id);
        if (ait == agents_.end()) return;
        ait->second.agent->set_model(model);
    }
    send_state(id);
}

void SwarmsServer::set_agent_credentials(const std::string& id, const std::string& provider,
                                         const std::string& api_key) {
    bool unsupported = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (agents_.find(id) == agents_.end()) return;

        auto fresh = make_provider(provider, api_key);
        if (!fresh) {
            unsupported = true;
        } else {
            // Swap the backing provider (history/tools survive) and remember the
            // key so agent slots created later use it too.
            agents_[id].agent->set_provider(std::move(fresh));
            api_key_ = api_key;
        }
    }
    if (unsupported) {
        send_to(id, stream_packet({{"event", "warning"},
                                   {"text", "unsupported provider '" + provider + "'"}}));
        return;
    }
    send_state(id);
}

void SwarmsServer::schedule_prompt(const std::string& id, const std::string& text) {
    if (text.empty()) return;

    Agent* agent = nullptr;
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = agents_.find(id);
        if (it == agents_.end()) return;
        if (it->second.busy) {
            rejected = true;
        } else {
            it->second.busy = true;
            agent = it->second.agent.get();
        }
    }

    if (rejected) {
        send_to(id, stream_packet({{"event", "warning"}, {"text", "still running a turn"}}));
        send_to(id, turn_done_packet(1));
        return;
    }

    std::thread([this, id, agent, text] {
        WireSink sink([this, id](const nlohmann::json& frame) -> bool {
            return send_to(id, frame);
        });

        int rc = 1;
        try {
            if (agent->has_provider()) {
                rc = agent->run_turn(text, sink);
            } else {
                sink.warning("prompt not run: no provider api key set — run /connect to store one");
            }
        } catch (const std::exception& e) {
            sink.warning(std::string("server error: ") + e.what());
            rc = 1;
        }

        if (sink.alive()) {
            TokenUsage u = agent->usage();
            send_to(id, ctx_packet(u.prompt_tokens, agent->context_window()));
            sink.done(rc);
        }

        bool idle = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = agents_.find(id);
            if (it != agents_.end()) {
                it->second.busy = false;
                if (clients_.count(id) == 0) agents_.erase(it);
            }
            idle = solo_ && clients_.empty();
        }
        if (idle) {
            closing_.store(true);
            g_stop_signal = 1;
        }
    }).detach();
}

void SwarmsServer::deliver_inbox(const std::string& id, const std::string& type,
                                 const nlohmann::json& msg) {
    std::lock_guard<std::mutex> lk(mu_);
    auto ait = agents_.find(id);
    if (ait == agents_.end() || !ait->second.agent->swarm()) return;
    static_cast<PeerView*>(ait->second.agent->swarm())->inject(type, msg);
}

void SwarmsServer::route_message(const std::string& from_id, const std::string& to,
                                 const std::string& text) {
    std::vector<std::string> targets;
    std::string from_name;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto fit = clients_.find(from_id);
        if (fit == clients_.end()) return;
        from_name = fit->second.name;
        if (to == "all") {
            for (const auto& [pid, pc] : clients_) {
                if (pid != from_id) targets.push_back(pid);
            }
        } else if (to == "repo") {
            auto members = repo_members_.find(fit->second.repo);
            if (members != repo_members_.end()) {
                for (const auto& pid : members->second) {
                    if (pid != from_id) targets.push_back(pid);
                }
            }
        } else {
            if (clients_.count(to) && to != from_id) targets.push_back(to);
        }
    }
    nlohmann::json notice = msg_notice(from_name, text, now_iso());
    for (const auto& tid : targets) {
        deliver_inbox(tid, "msg", notice);
    }
}

void SwarmsServer::maybe_shutdown_if_idle() {
    bool idle = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        idle = solo_ && clients_.empty();
        if (idle) {
            for (const auto& [id, slot] : agents_) {
                if (slot.busy) {
                    idle = false;
                    break;
                }
            }
        }
    }
    if (idle) {
        closing_.store(true);
        g_stop_signal = 1;
    }
}

// ---------------------------------------------------------------------------
// Client connection.
// ---------------------------------------------------------------------------

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
        hello_register(id, name, repo, fd, peers);
    }
    create_agent_slot(id, repo);
    send_to(id, hello_ack(id, nlohmann::json(peers)));
    send_state(id);

    for (;;) {
        if (reader.next_line(line) != ReadResult::Ok) break;
        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(line);
        } catch (...) {
            break;
        }
        std::string t = type_of(msg);
        if (t == "prompt") {
            schedule_prompt(id, msg.value("text", ""));
        } else if (t == "cancel") {
            // Abort the running turn for this agent. The base Agent::cancel_turn
            // flips the atomic and aborts the provider's in-flight request, so
            // the turn thread returns (and sends turn_done) shortly after.
            std::lock_guard<std::mutex> lk(mu_);
            auto ait = agents_.find(id);
            if (ait != agents_.end() && ait->second.busy) {
                ait->second.agent->cancel_turn();
            }
        } else if (t == "clear") {
            clear_agent_history(id);
        } else if (t == "model_set") {
            set_agent_model(id, msg.value("model", ""));
        } else if (t == "auth_set") {
            set_agent_credentials(id, msg.value("provider", ""),
                                  msg.value("api_key", ""));
        } else if (t == "read") {
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
            route_message(id, msg.value("to", ""), msg.value("text", ""));
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
    maybe_shutdown_if_idle();
}

// ---------------------------------------------------------------------------
// Poller + shutdown + main loop.
// ---------------------------------------------------------------------------

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
    closing_.store(false);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);

    poller_ = std::thread([this] { poller_loop(); });

    while (!g_stop_signal && !closing_.load()) {
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