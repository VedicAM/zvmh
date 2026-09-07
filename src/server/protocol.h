#ifndef SERVER_PROTOCOL_H
#define SERVER_PROTOCOL_H

// NDJSON-over-TCP wire protocol for the zvmh swarm feature.
//
// Every message is a single JSON object on one line, terminated by '\n'
// (frames are capped at kMaxFrameBytes: a longer line closes the connection).
// Message types are selected by the "type" field:
//
//   client -> server
//     {"type":"hello","repo":...,"name":...}
//     {"type":"read","path":...,"hash":...}
//     {"type":"write","path":...,"hash":...}
//     {"type":"msg","to":"all"|"repo"|"<agent-id>","text":...}
//     {"type":"agents"}
//     {"type":"ping"}
//     {"type":"bye"}
//
//   server -> client
//     {"type":"hello_ack","id":...,"peers":[...]}
//     {"type":"conflict","path":...,"by":...,"at":...,"hash":...}
//     {"type":"msg","from":...,"text":...,"at":...}
//     {"type":"agents_list","agents":[...]}
//     {"type":"pong"}
//     {"type":"bye"}                      reply to client bye
//     {"type":"shutdown"}                 server is going away
//
// peers/agents entries: {"id":..., "name":..., "repo":...}
//
// "hash" is the FNV-1a-64 (decimal string) of the file bytes computed by the
// client that read/wrote it. The server never inspects file contents on the
// wire or stores them; it only matches hashes.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <poll.h>
#include <sstream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

namespace swarm {

inline constexpr const char* kDefaultHost = "127.0.0.1";
inline constexpr int kDefaultPort = 5500;
inline constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024;
inline constexpr size_t kReadChunkBytes = 64 * 1024;

enum class ReadResult {
    Ok,       // out holds one line, '\n' consumed
    Eof,      // peer closed the connection (recv returned 0/error)
    Stopped,  // stop flag set before data arrived
    TooLong,  // line exceeded kMaxFrameBytes without '\n'; stream corrupt
    Error
};

// ---------------------------------------------------------------------------
// Message builders — keep wire shape in one place.
// ---------------------------------------------------------------------------

inline nlohmann::json hello_req(const std::string& repo, const std::string& name) {
    return {{"type", "hello"}, {"repo", repo}, {"name", name}};
}

inline nlohmann::json hello_ack(const std::string& id, const nlohmann::json& peers) {
    return {{"type", "hello_ack"}, {"id", id}, {"peers", peers}};
}

inline nlohmann::json agent_info(const std::string& id, const std::string& name,
                                 const std::string& repo) {
    return {{"id", id}, {"name", name}, {"repo", repo}};
}

inline nlohmann::json read_req(const std::string& path, const std::string& hash) {
    return {{"type", "read"}, {"path", path}, {"hash", hash}};
}

inline nlohmann::json write_req(const std::string& path, const std::string& hash) {
    return {{"type", "write"}, {"path", path}, {"hash", hash}};
}

inline nlohmann::json conflict_notice(const std::string& path, const std::string& by,
                                      const std::string& at, const std::string& hash) {
    return {{"type", "conflict"}, {"path", path}, {"by", by}, {"at", at}, {"hash", hash}};
}

inline nlohmann::json msg_req(const std::string& to, const std::string& text) {
    return {{"type", "msg"}, {"to", to}, {"text", text}};
}

inline nlohmann::json msg_notice(const std::string& from, const std::string& text,
                                 const std::string& at) {
    return {{"type", "msg"}, {"from", from}, {"text", text}, {"at", at}};
}

inline nlohmann::json agents_req() {
    return {{"type", "agents"}};
}

inline nlohmann::json agents_list(const nlohmann::json& agents) {
    return {{"type", "agents_list"}, {"agents", agents}};
}

inline nlohmann::json ping_msg() { return {{"type", "ping"}}; }
inline nlohmann::json pong_msg() { return {{"type", "pong"}}; }
inline nlohmann::json bye_msg() { return {{"type", "bye"}}; }
inline nlohmann::json shutdown_msg() { return {{"type", "shutdown"}}; }

inline std::string type_of(const nlohmann::json& obj) {
    auto it = obj.find("type");
    if (it == obj.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

// ---------------------------------------------------------------------------
// Path + hash + time helpers (metadata only, never file contents on the wire).
// ---------------------------------------------------------------------------

inline std::string canonical_path(const std::filesystem::path& base, const std::string& rel) {
    std::filesystem::path combined;
    std::filesystem::path p(rel);
    if (p.is_absolute()) {
        combined = p.lexically_normal();
    } else {
        combined = (base / rel).lexically_normal();
    }
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(combined, ec);
    if (!ec) combined = resolved;
    return combined.generic_string();
}

inline uint64_t fnv1a64(const std::string& bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : bytes) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string hash_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return std::to_string(fnv1a64(ss.str()));
}

inline std::string iso_timestamp(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

inline std::string now_iso() {
    return iso_timestamp(std::time(nullptr));
}

// ---------------------------------------------------------------------------
// Socket helpers.
// ---------------------------------------------------------------------------

inline void set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

inline bool send_all(int fd, const char* data, size_t n) {
    size_t sent = 0;
    while (sent < n) {
#ifdef MSG_NOSIGNAL
        ssize_t r = ::send(fd, data + sent, n - sent, MSG_NOSIGNAL);
#else
        ssize_t r = ::send(fd, data + sent, n - sent, 0);
#endif
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(r);
    }
    return true;
}

inline bool send_json(int fd, const nlohmann::json& obj) {
    std::string line = obj.dump() + '\n';
    return send_all(fd, line.data(), line.size());
}

// Reads newline-terminated frames from a socket owned by a single thread.
// poll() runs in short ticks so a caller-supplied atomic stop flag can break a
// blocked read. On TooLong the stream is corrupt and must be dropped. An
// optional max_wait_ms bounds the whole read (returns Stopped on expiry).
class LineReader {
public:
    LineReader(int fd, std::atomic<bool>* stop = nullptr, int max_wait_ms = 0)
        : fd_(fd), stop_(stop), max_wait_ms_(max_wait_ms) {}

    ReadResult next_line(std::string& out) {
        out.clear();
        for (;;) {
            if (stop_ && stop_->load()) return ReadResult::Stopped;

            size_t newline = buf_.find('\n', pos_);
            if (newline != std::string::npos) {
                out = buf_.substr(pos_, newline - pos_);
                pos_ = newline + 1;
                return ReadResult::Ok;
            }
            if (buf_.size() - pos_ > kMaxFrameBytes) {
                return ReadResult::TooLong;
            }
            // Drop consumed prefix so the cap check above stays meaningful.
            buf_.erase(0, pos_);
            pos_ = 0;

            struct pollfd pfd;
            pfd.fd = fd_;
            pfd.events = POLLIN;
            int pr = ::poll(&pfd, 1, 200);
            if (pr < 0) {
                if (errno == EINTR) continue;
                return ReadResult::Error;
            }
            elapsed_ms_ += 200;
            if (max_wait_ms_ > 0 && elapsed_ms_ >= max_wait_ms_) {
                return ReadResult::Stopped;
            }
            if (pr == 0) continue;  // timeout tick; re-check stop flag

            if (pfd.revents & POLLIN) {
                char chunk[kReadChunkBytes];
                ssize_t r = ::recv(fd_, chunk, sizeof(chunk), 0);
                if (r == 0) return ReadResult::Eof;
                if (r < 0) {
                    if (errno == EINTR) continue;
                    return ReadResult::Error;
                }
                buf_.append(chunk, static_cast<size_t>(r));
            } else {
                return ReadResult::Error;
            }
        }
    }

private:
    int fd_;
    std::atomic<bool>* stop_;
    int max_wait_ms_;
    int elapsed_ms_ = 0;
    std::string buf_;
    size_t pos_ = 0;
};

}  // namespace swarm

#endif