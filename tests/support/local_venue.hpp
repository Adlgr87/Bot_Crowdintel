#ifndef LOCAL_VENUE_HPP
#define LOCAL_VENUE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// In-process test venue: a minimal HTTP/1.1 + WebSocket server bound to
// 127.0.0.1 on an ephemeral port.
//
// Purpose: exercise the *production* components — CurlTransport, ClobApiClient,
// OrderGateway, LedgerOrderObserver, OrderRecorder, OrderHeartbeat,
// Reconciler, UserWsClient, EventLedger — against real sockets, real TLS-less
// loopback transport and real threads, so ThreadSanitizer can see the whole
// topology race.  No venue, no credentials and no money are involved.
//
// It is deliberately scriptable and adversarial:
//   * canned responses per (method, path prefix);
//   * faults: no response (client timeout), close mid-body, invalid JSON with
//     HTTP 200, delayed responses, connection refused after accept;
//   * WebSocket: push text frames on demand, push a *partial* frame and then
//     close (disconnect mid-message), answer the application-level PING with
//     PONG, and record every frame the client sends (including the
//     subscription frame and masked data frames).
//
// Test-only.  It lives under tests/ and is never linked into crowdintel_bot.
// ─────────────────────────────────────────────────────────────────────────────

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../core/src/ws_url.hpp"

namespace testvenue {

inline constexpr size_t K_MAX_RULES = 64;
inline constexpr size_t K_MAX_REQUESTS = 256;
inline constexpr size_t K_MAX_WS_FRAMES = 128;
inline constexpr size_t K_MAX_BODY = 65536;

enum class Fault {
    NONE = 0,
    NO_RESPONSE,        // read the request, never answer, close after the delay
    CLOSE_MID_BODY,     // announce a longer body, then close the socket
    DELAY,              // answer normally after delay_ms
    REFUSE_AFTER_ACCEPT  // close immediately without reading
};

struct ResponseSpec {
    int status = 200;
    std::string body;
    Fault fault = Fault::NONE;
    int delay_ms = 0;
};

struct RecordedRequest {
    std::string method;
    std::string path;    // without the query string
    std::string query;
    std::string body;
    std::vector<std::string> headers;  // "Name: value" as received
};

struct Rule {
    std::string method;
    std::string path_prefix;
    ResponseSpec spec;
    bool once = false;
    bool consumed = false;
};

class Server {
public:
    Server() = default;
    ~Server() { stop(); }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const std::string& ws_path = "/ws/user") {
        ws_path_ = ws_path;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;  // ephemeral
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        port_ = ntohs(address.sin_port);
        if (::listen(listen_fd_, 8) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        stop_.store(false, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        if (ws_fd_ >= 0) { ::close(ws_fd_); ws_fd_ = -1; }
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    }

    int port() const noexcept { return port_; }
    std::string http_base() const {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "http://127.0.0.1:%d", port_);
        return buffer;
    }
    std::string http_url(const std::string& path) const { return http_base() + path; }
    std::string ws_url() const {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "ws://127.0.0.1:%d%s", port_,
                      ws_path_.c_str());
        return buffer;
    }

    // ── Scripting ───────────────────────────────────────────────────────────
    void on(const std::string& method, const std::string& path_prefix,
            const ResponseSpec& spec) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rules_.size() >= K_MAX_RULES) return;
        Rule rule;
        rule.method = method;
        rule.path_prefix = path_prefix;
        rule.spec = spec;
        rule.once = false;
        rules_.push_back(rule);
    }

    // A one-shot rule that takes precedence over the persistent ones (fault
    // injection in front of a healthy endpoint).
    void once(const std::string& method, const std::string& path_prefix,
              const ResponseSpec& spec) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rules_.size() >= K_MAX_RULES) return;
        Rule rule;
        rule.method = method;
        rule.path_prefix = path_prefix;
        rule.spec = spec;
        rule.once = true;
        rules_.push_back(rule);
    }

    void clear_rules() {
        std::lock_guard<std::mutex> lock(mutex_);
        rules_.clear();
    }

    // ── WebSocket control ───────────────────────────────────────────────────
    void ws_send_text(const std::string& payload) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_frames_.push_back(payload);
    }

    // Sends the first `bytes` of a frame header+payload and then closes the
    // socket: a disconnect in the middle of a message.
    void ws_send_partial_and_close(const std::string& payload, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        partial_frame_ = payload;
        partial_bytes_ = bytes;
        partial_requested_ = true;
    }

    void ws_close() {
        std::lock_guard<std::mutex> lock(mutex_);
        close_ws_requested_ = true;
    }

    bool ws_connected() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ws_fd_ >= 0;
    }

    // Waits (bounded) until a WebSocket client has completed the handshake.
    bool wait_for_ws(int timeout_ms = 5000) const {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (ws_connected()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return ws_connected();
    }

    // ── Observations ────────────────────────────────────────────────────────
    std::vector<RecordedRequest> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }
    size_t request_count(const std::string& method,
                         const std::string& path_fragment) const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& request : requests_) {
            std::string target = request.path;
            if (!request.query.empty()) target += "?" + request.query;
            if (request.method == method && target.find(path_fragment) != std::string::npos)
                ++count;
        }
        return count;
    }
    std::vector<std::string> ws_frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ws_frames_;
    }
    size_t ws_frame_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ws_frames_.size();
    }
    size_t ws_handshakes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ws_handshakes_;
    }

private:
    void run() {
        while (!stop_.load(std::memory_order_acquire)) {
            pollfd fds[2];
            size_t count = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (listen_fd_ >= 0) {
                    fds[count].fd = listen_fd_;
                    fds[count].events = POLLIN;
                    fds[count].revents = 0;
                    ++count;
                }
                if (ws_fd_ >= 0) {
                    fds[count].fd = ws_fd_;
                    fds[count].events = POLLIN;
                    fds[count].revents = 0;
                    ++count;
                }
            }
            if (count == 0) return;
            (void)::poll(fds, static_cast<nfds_t>(count), 40);
            if (stop_.load(std::memory_order_acquire)) return;
            for (size_t i = 0; i < count; ++i) {
                if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                std::lock_guard<std::mutex> lock(mutex_);
                if (fds[i].fd == listen_fd_) {
                    const int fd = ::accept(listen_fd_, nullptr, nullptr);
                    if (fd >= 0) handle_connection(fd);
                } else if (fds[i].fd == ws_fd_) {
                    if (!service_ws()) {
                        ::close(ws_fd_);
                        ws_fd_ = -1;
                    }
                }
            }
            // Flush queued frames even when the client is quiet.
            std::lock_guard<std::mutex> lock(mutex_);
            if (ws_fd_ >= 0) flush_ws_locked();
        }
    }

    // Reads one HTTP request; upgrades to WebSocket or answers from the script.
    void handle_connection(int fd) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval timeout{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        std::string head;
        char buffer[4096];
        while (head.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) { ::close(fd); return; }
            head.append(buffer, static_cast<size_t>(n));
            if (head.size() > K_MAX_BODY) { ::close(fd); return; }
        }
        const size_t head_end = head.find("\r\n\r\n") + 4;
        std::string extra = head.substr(head_end);
        head.resize(head_end);

        RecordedRequest request;
        size_t content_length = 0;
        bool upgrade = false;
        std::string ws_key;
        if (!parse_request(head, request, content_length, upgrade, ws_key)) {
            ::close(fd);
            return;
        }
        while (extra.size() < content_length) {
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            extra.append(buffer, static_cast<size_t>(n));
        }
        request.body = extra.substr(0, content_length);
        // Bytes past the request body belong to the next protocol message.  For a
        // WebSocket upgrade the client typically pipelines its subscription frame
        // immediately after the handshake, so dropping them would silently lose
        // the first frame.
        const std::string pipelined = extra.size() > content_length
                                          ? extra.substr(content_length) : std::string();
        requests_.push_back(request);
        if (requests_.size() > K_MAX_REQUESTS)
            requests_.erase(requests_.begin());

        if (upgrade && request.path == ws_path_) {
            char accept_value[32]{};
            ws_url::websocket_accept(ws_key.c_str(), accept_value);
            std::string response =
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
            response += accept_value;
            response += "\r\n\r\n";
            if (!send_all(fd, response.data(), response.size())) {
                ::close(fd);
                return;
            }
            if (ws_fd_ >= 0) ::close(ws_fd_);
            ws_fd_ = fd;
            ++ws_handshakes_;
            ws_read_buffer_ = pipelined;
            // Decode anything the client already pipelined, and flush queued
            // server frames so a script written before connect() still works.
            if (!ws_read_buffer_.empty() && !decode_client_frames()) {
                ::close(ws_fd_);
                ws_fd_ = -1;
                return;
            }
            flush_ws_locked();
            return;
        }

        ResponseSpec spec;
        // Rules match against the full request target (path + query), so a rule
        // can key on a query parameter such as asset_type=COLLATERAL.
        std::string target = request.path;
        if (!request.query.empty()) target += "?" + request.query;
        if (!match_rule(request.method, target, spec)) {
            const std::string body = "{\"error\":\"no fixture\"}";
            const std::string response = http_response(404, body);
            (void)send_all(fd, response.data(), response.size());
            ::close(fd);
            return;
        }
        switch (spec.fault) {
            case Fault::REFUSE_AFTER_ACCEPT:
                ::close(fd);
                return;
            case Fault::NO_RESPONSE:
                if (spec.delay_ms)
                    std::this_thread::sleep_for(std::chrono::milliseconds(spec.delay_ms));
                ::close(fd);  // the client sees a timeout or an empty reply
                return;
            case Fault::DELAY:
                std::this_thread::sleep_for(std::chrono::milliseconds(spec.delay_ms));
                break;
            case Fault::CLOSE_MID_BODY: {
                // Announce more bytes than we send, then hang up.
                char header[256];
                const int written = std::snprintf(
                    header, sizeof(header),
                    "HTTP/1.1 %d OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                    spec.status, spec.body.size() + 4096);
                if (written > 0)
                    (void)send_all(fd, header, static_cast<size_t>(written));
                (void)send_all(fd, spec.body.data(), spec.body.size());
                ::close(fd);
                return;
            }
            case Fault::NONE:
            default:
                break;
        }
        const std::string response = http_response(spec.status, spec.body);
        (void)send_all(fd, response.data(), response.size());
        ::close(fd);
    }

    bool match_rule(const std::string& method, const std::string& path,
                    ResponseSpec& out) {
        // One-shot rules win, then persistent ones in registration order.
        for (int pass = 0; pass < 2; ++pass) {
            for (auto& rule : rules_) {
                if (rule.once != (pass == 0)) continue;
                if (rule.once && rule.consumed) continue;
                if (!rule.method.empty() && rule.method != method) continue;
                // Substring match on the request target, like the fixture
                // transport used by the offline unit tests, so a rule can key on
                // a path fragment or a query parameter.
                if (rule.path_prefix.empty()) continue;
                if (path.find(rule.path_prefix) == std::string::npos) continue;
                out = rule.spec;
                if (rule.once) rule.consumed = true;
                return true;
            }
        }
        return false;
    }

    static std::string http_response(int status, const std::string& body) {
        char header[256];
        const char* reason = status == 200 ? "OK" : (status == 404 ? "Not Found"
                                                                   : "Error");
        const int written = std::snprintf(
            header, sizeof(header),
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
            status, reason, body.size());
        std::string response(header, static_cast<size_t>(written > 0 ? written : 0));
        response += body;
        return response;
    }

    static bool send_all(int fd, const char* data, size_t len) {
        size_t sent = 0;
        while (sent < len) {
            const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    static bool parse_request(const std::string& head, RecordedRequest& out,
                              size_t& content_length, bool& upgrade,
                              std::string& ws_key) {
        const size_t line_end = head.find("\r\n");
        if (line_end == std::string::npos) return false;
        const std::string request_line = head.substr(0, line_end);
        size_t first = request_line.find(' ');
        if (first == std::string::npos) return false;
        size_t second = request_line.find(' ', first + 1);
        if (second == std::string::npos) return false;
        out.method = request_line.substr(0, first);
        const std::string target = request_line.substr(first + 1, second - first - 1);
        const size_t question = target.find('?');
        out.path = question == std::string::npos ? target : target.substr(0, question);
        out.query = question == std::string::npos ? "" : target.substr(question + 1);
        content_length = 0;
        size_t cursor = line_end + 2;
        while (cursor < head.size()) {
            const size_t end = head.find("\r\n", cursor);
            if (end == std::string::npos || end == cursor) break;
            const std::string line = head.substr(cursor, end - cursor);
            out.headers.push_back(line);
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.erase(value.begin());
                for (auto& character : name)
                    character = static_cast<char>(::tolower(static_cast<unsigned char>(character)));
                if (name == "content-length")
                    content_length = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
                else if (name == "upgrade")
                    upgrade = strcasecmp(value.c_str(), "websocket") == 0;
                else if (name == "sec-websocket-key")
                    ws_key = value;
            }
            cursor = end + 2;
        }
        return true;
    }

    // Writes queued frames and reads whatever the client sent.  Caller holds the
    // mutex.  Returns false when the connection is gone.
    bool service_ws() {
        flush_ws_locked();
        if (partial_requested_) {
            partial_requested_ = false;
            const std::string frame = encode_text_frame(partial_frame_);
            const size_t bytes = partial_bytes_ < frame.size() ? partial_bytes_
                                                               : frame.size();
            (void)send_all(ws_fd_, frame.data(), bytes);
            ::close(ws_fd_);
            ws_fd_ = -1;
            return false;
        }
        if (close_ws_requested_) {
            close_ws_requested_ = false;
            const std::string frame = encode_close_frame();
            (void)send_all(ws_fd_, frame.data(), frame.size());
            ::close(ws_fd_);
            ws_fd_ = -1;
            return false;
        }
        char buffer[8192];
        const ssize_t n = ::recv(ws_fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n == 0) return false;
        if (n < 0) return true;  // EAGAIN: nothing to read
        ws_read_buffer_.append(buffer, static_cast<size_t>(n));
        return decode_client_frames();
    }

    void flush_ws_locked() {
        while (!pending_frames_.empty()) {
            const std::string frame = encode_text_frame(pending_frames_.front());
            if (!send_all(ws_fd_, frame.data(), frame.size())) {
                pending_frames_.clear();
                ::close(ws_fd_);
                ws_fd_ = -1;
                return;
            }
            pending_frames_.erase(pending_frames_.begin());
        }
    }

    // Server frames are never masked (RFC 6455 §5.1).
    static std::string encode_text_frame(const std::string& payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x81));  // FIN + text
        const size_t len = payload.size();
        if (len < 126) {
            frame.push_back(static_cast<char>(len));
        } else if (len <= 0xFFFF) {
            frame.push_back(static_cast<char>(126));
            frame.push_back(static_cast<char>((len >> 8) & 0xFF));
            frame.push_back(static_cast<char>(len & 0xFF));
        } else {
            frame.push_back(static_cast<char>(127));
            for (int i = 7; i >= 0; --i)
                frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
        }
        frame += payload;
        return frame;
    }

    static std::string encode_close_frame() {
        std::string frame;
        frame.push_back(static_cast<char>(0x88));  // FIN + close
        frame.push_back(static_cast<char>(2));
        frame.push_back(static_cast<char>(0x03));  // 1000 normal closure
        frame.push_back(static_cast<char>(0xE8));
        return frame;
    }

    // Client frames are masked; unmask and record text payloads.  Answers the
    // application-level "PING" with "PONG" like the real CLOB stream.
    bool decode_client_frames() {
        size_t offset = 0;
        while (offset + 2 <= ws_read_buffer_.size()) {
            const uint8_t b0 = static_cast<uint8_t>(ws_read_buffer_[offset]);
            const uint8_t b1 = static_cast<uint8_t>(ws_read_buffer_[offset + 1]);
            const uint8_t opcode = b0 & 0x0FU;
            const bool masked = (b1 & 0x80U) != 0;
            uint64_t len = b1 & 0x7FU;
            size_t header = 2;
            if (len == 126) {
                if (offset + 4 > ws_read_buffer_.size()) break;
                len = static_cast<uint64_t>(
                    static_cast<uint8_t>(ws_read_buffer_[offset + 2])) * 256 +
                    static_cast<uint8_t>(ws_read_buffer_[offset + 3]);
                header = 4;
            } else if (len == 127) {
                if (offset + 10 > ws_read_buffer_.size()) break;
                len = 0;
                for (int i = 0; i < 8; ++i)
                    len = len * 256 +
                          static_cast<uint8_t>(ws_read_buffer_[offset + 2 + i]);
                header = 10;
            }
            uint8_t mask[4] = {0, 0, 0, 0};
            if (masked) {
                if (offset + header + 4 > ws_read_buffer_.size()) break;
                std::memcpy(mask, ws_read_buffer_.data() + offset + header, 4);
                header += 4;
            }
            if (offset + header + len > ws_read_buffer_.size()) break;
            std::string payload(len, '\0');
            for (uint64_t i = 0; i < len; ++i) {
                const uint8_t byte =
                    static_cast<uint8_t>(ws_read_buffer_[offset + header + i]);
                payload[static_cast<size_t>(i)] =
                    static_cast<char>(masked ? byte ^ mask[i & 3] : byte);
            }
            offset += header + static_cast<size_t>(len);
            if (opcode == 0x8) return false;  // client closed
            if (opcode == 0x9) {  // protocol ping → pong
                std::string pong;
                pong.push_back(static_cast<char>(0x8A));
                pong.push_back(static_cast<char>(payload.size()));
                pong += payload;
                if (!send_all(ws_fd_, pong.data(), pong.size())) return false;
                continue;
            }
            if (opcode != 0x1 && opcode != 0x2 && opcode != 0x0) continue;
            if (ws_frames_.size() < K_MAX_WS_FRAMES) ws_frames_.push_back(payload);
            if (payload == "PING") {
                const std::string pong = encode_text_frame("PONG");
                if (!send_all(ws_fd_, pong.data(), pong.size())) return false;
            }
        }
        if (offset) ws_read_buffer_.erase(0, offset);
        return true;
    }

    mutable std::mutex mutex_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int listen_fd_ = -1;
    int ws_fd_ = -1;
    int port_ = 0;
    std::string ws_path_ = "/ws/user";
    std::vector<Rule> rules_;
    std::vector<RecordedRequest> requests_;
    std::vector<std::string> pending_frames_;
    std::vector<std::string> ws_frames_;
    std::string ws_read_buffer_;
    size_t ws_handshakes_ = 0;
    bool partial_requested_ = false;
    std::string partial_frame_;
    size_t partial_bytes_ = 0;
    bool close_ws_requested_ = false;
};

}  // namespace testvenue

#endif  // LOCAL_VENUE_HPP
