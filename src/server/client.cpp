#include "client.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

namespace swarm {

namespace {
bool read_one(LineReader& reader, nlohmann::json& out) {
    std::string line;
    if (reader.next_line(line) != ReadResult::Ok) return false;
    try {
        out = nlohmann::json::parse(line);
    } catch (...) {
        return false;
    }
    return true;
}
}  // namespace

ServerClient::~ServerClient() {
    disconnect();
}

bool ServerClient::connect(const std::string& repo, const std::string& name) {
    repo_ = repo;
    name_ = name;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port_);
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "Error: getaddrinfo(" << host_ << ":" << port_ << "): "
                  << gai_strerror(rc) << "\n";
        return false;
    }

    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        std::cerr << "Error: could not connect to " << host_ << ":" << port_
                  << ": " << strerror(errno) << "\n";
        return false;
    }

    set_nosigpipe(fd);

    {
        std::lock_guard<std::mutex> lk(send_mu_);
        if (!send_json(fd, hello_req(repo, name))) {
            ::close(fd);
            return false;
        }
    }

    LineReader reader(fd, nullptr, 5000);
    nlohmann::json ack;
    bool ok = read_one(reader, ack) && type_of(ack) == "hello_ack";
    if (!ok) {
        std::cerr << "Error: swarm server at " << host_ << ":" << port_
                  << " did not acknowledge the hello handshake ("
                  << (type_of(ack).empty() ? "no hello_ack" : type_of(ack)) << ")\n";
        ::close(fd);
        return false;
    }

    id_ = ack.value("id", "");
    if (id_.empty()) {
        ::close(fd);
        return false;
    }

    fd_ = fd;
    running_.store(true);
    {
        std::lock_guard<std::mutex> lk(mu_);
        peers_.clear();
        if (ack.contains("peers") && ack["peers"].is_array()) {
            peers_ = ack["peers"].get<std::vector<nlohmann::json>>();
        }
    }
    reader_ = std::thread([this] { reader_loop(); });
    return true;
}

void ServerClient::reader_loop() {
    LineReader reader(fd_);
    for (;;) {
        if (!running_.load()) break;
        std::string line;
        if (reader.next_line(line) != ReadResult::Ok) break;

        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(line);
        } catch (...) {
            continue;
        }
        std::string t = type_of(msg);

        if (t == "agents_list") {
            std::lock_guard<std::mutex> lk(mu_);
            if (msg.contains("agents") && msg["agents"].is_array()) {
                peers_ = msg["agents"].get<std::vector<nlohmann::json>>();
            }
        }

        MessageHandler h;
        {
            std::lock_guard<std::mutex> lk(mu_);
            h = handler_;
        }
        if (h) h(t, msg);

        if (t == "shutdown") {
            running_.store(false);
            break;
        }
    }
    running_.store(false);
}

bool ServerClient::register_read(const std::string& path) {
    std::string canon = canonical_path(repo_, path);
    std::string hash = hash_file(canon);
    if (hash.empty()) return false;
    std::lock_guard<std::mutex> lk(send_mu_);
    return send_json(fd_, read_req(canon, hash));
}

bool ServerClient::report_write(const std::string& path) {
    std::string canon = canonical_path(repo_, path);
    std::string hash = hash_file(canon);
    if (hash.empty()) return false;
    std::lock_guard<std::mutex> lk(send_mu_);
    return send_json(fd_, write_req(canon, hash));
}

bool ServerClient::send_message(const std::string& to, const std::string& text) {
    if (text.empty()) return false;
    std::lock_guard<std::mutex> lk(send_mu_);
    return send_json(fd_, msg_req(to, text));
}

bool ServerClient::request_peers() {
    std::lock_guard<std::mutex> lk(send_mu_);
    return send_json(fd_, agents_req());
}

void ServerClient::disconnect() {
    int fd = fd_;
    if (running_.exchange(false)) {
        if (fd >= 0) {
            {
                std::lock_guard<std::mutex> lk(send_mu_);
                send_json(fd, bye_msg());
            }
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    if (reader_.joinable()) reader_.join();
    if (fd >= 0) {
        ::close(fd);
        fd_ = -1;
    }
}

}  // namespace swarm