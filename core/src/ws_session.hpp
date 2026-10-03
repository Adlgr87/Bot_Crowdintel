#ifndef WS_SESSION_HPP
#define WS_SESSION_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Reusable WebSocket/TLS session for the *cold* control channels (currently the
// CLOB user channel).
//
// Responsibilities, strictly transport-level:
//   wss:// URL parsing → TCP connect with timeout → TLS 1.2+ with hostname
//   verification (and optional SPKI pin) → RFC 6455 client handshake with
//   Sec-WebSocket-Accept validation → masked client frames → server frame
//   reassembly (including fragmentation and control frames) → application-level
//   text keep-alive (the CLOB streams use a text "PING"/"PONG" pair, not RFC
//   6455 control pings: https://docs.polymarket.com/market-data/websocket/overview)
//   → idle and stale detection → close.
//
// Protocol semantics stay with the caller through a message callback, so this
// file contains no venue vocabulary and is testable without a socket.
//
// NOTE (tracked debt): core/src/ws_market_listener.hpp still carries its own
// copy of equivalent plumbing.  It was deliberately left untouched here: that
// path is exercised in production-like runs and cannot be re-validated from
// this sandbox (no egress to the venue), so migrating it in the same change
// that introduces the user channel would trade a known-good hot path for an
// untestable refactor.  Migration plan: replace its private statics with
// WsSession and keep handle_message()/read_frames() book logic as the callback.
// ─────────────────────────────────────────────────────────────────────────────

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <optional>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../crypto/fast_random.hpp"
#include "../crypto/sha1.hpp"
#include "ws_url.hpp"
#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"

namespace ws {

inline constexpr size_t K_MAX_MESSAGE = 1U << 20;   // 1 MiB frame cap
inline constexpr size_t K_MAX_CONTROL = 125;        // RFC 6455 control limit
inline constexpr size_t K_RECV_BUFFER = 1U << 20;
inline constexpr size_t K_MAX_HEADER = 4096;

struct SessionConfig {
    char url[256]{};
    int connect_timeout_ms = 3000;
    uint32_t keepalive_interval_ms = 8000;  // text PING cadence (docs: 10 s)
    uint32_t idle_timeout_ms = 20000;       // no bytes → drop and reconnect
    uint32_t pong_timeout_ms = 30000;       // no PONG → treat the feed as stale
    bool require_tls = true;
    // Optional SPKI pin in the repository's spelling: the base64 body of
    // "sha256//<base64>" (44 characters).  Empty disables pinning.
    char pin_spki_base64[65]{};
};

enum class SessionError : uint8_t {
    NONE = 0,
    URL_INVALID,
    DNS_OR_CONNECT,
    TLS_HANDSHAKE,
    TLS_VERIFICATION,
    TLS_PIN_MISMATCH,
    HANDSHAKE_REJECTED,
    PROTOCOL_VIOLATION,
    MESSAGE_TOO_LARGE,
    IDLE_TIMEOUT,
    PONG_TIMEOUT,
    SEND_FAILED,
    CLOSED_BY_PEER,
    STOPPED,
    ENTROPY_UNAVAILABLE
};

inline const char* session_error_name(SessionError error) noexcept {
    switch (error) {
        case SessionError::NONE: return "none";
        case SessionError::URL_INVALID: return "url_invalid";
        case SessionError::DNS_OR_CONNECT: return "dns_or_connect_failed";
        case SessionError::TLS_HANDSHAKE: return "tls_handshake_failed";
        case SessionError::TLS_VERIFICATION: return "tls_verification_failed";
        case SessionError::TLS_PIN_MISMATCH: return "tls_pin_mismatch";
        case SessionError::HANDSHAKE_REJECTED: return "handshake_rejected";
        case SessionError::PROTOCOL_VIOLATION: return "protocol_violation";
        case SessionError::MESSAGE_TOO_LARGE: return "message_too_large";
        case SessionError::IDLE_TIMEOUT: return "idle_timeout";
        case SessionError::PONG_TIMEOUT: return "pong_timeout";
        case SessionError::SEND_FAILED: return "send_failed";
        case SessionError::CLOSED_BY_PEER: return "closed_by_peer";
        case SessionError::STOPPED: return "stopped";
        case SessionError::ENTROPY_UNAVAILABLE: return "entropy_unavailable";
    }
    return "unknown";
}

// Returns true to keep reading, false to end the session.
using MessageCallback = bool (*)(void* context, const char* payload, size_t length);

inline uint64_t mono_ms() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

class Session {
public:
    Session() = default;
    ~Session() { disconnect(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // URL parsing and the RFC 6455 accept value live in ws_url.hpp so they can
    // be unit-tested without OpenSSL.

    // ── Lifecycle ───────────────────────────────────────────────────────────
    // Establishes transport + handshake and sends `subscription` as the first
    // text frame.  Returns false with `error` set on any failure.
    bool connect(const SessionConfig& config, const char* subscription,
                 size_t subscription_len, SessionError& error) {
        config_ = config;
        error = SessionError::NONE;
        // RFC 6455 requires an unpredictable masking key that is not reused, so
        // the session owns one CSPRNG stream for its lifetime.  It is seeded
        // here, not inside send_frame(): FastRandom's constructor throws when the
        // operating system refuses entropy, and send_frame()/send_close() are
        // noexcept - an exception escaping either would call std::terminate in
        // the middle of the user channel.  Entropy failure is reported like any
        // other connection failure and the session simply does not start.
        try {
            mask_random_.emplace();
        } catch (const std::exception&) {
            error = SessionError::ENTROPY_UNAVAILABLE;
            return false;
        }
        char host[160]{};
        char path[192]{};
        int port = 443;
        bool tls = true;
        if (!ws_url::parse_url(config_.url, host, sizeof(host), path, sizeof(path),
                           port, tls)) {
            error = SessionError::URL_INVALID;
            return false;
        }
        if (config_.require_tls && !tls) {
            error = SessionError::URL_INVALID;  // plain ws:// refused in live
            return false;
        }
        const int fd = tcp_connect(host, port, config_.connect_timeout_ms);
        if (fd < 0) {
            error = SessionError::DNS_OR_CONNECT;
            return false;
        }
        fd_ = fd;
        set_socket_options(fd);
        if (tls) {
            SSL_CTX* context = ssl_context();
            if (!context) {
                error = SessionError::TLS_HANDSHAKE;
                disconnect();
                return false;
            }
            ssl_ = SSL_new(context);
            if (!ssl_) {
                error = SessionError::TLS_HANDSHAKE;
                disconnect();
                return false;
            }
            if (SSL_set_fd(ssl_, fd) != 1 ||
                SSL_set_tlsext_host_name(ssl_, host) != 1 ||
                SSL_set1_host(ssl_, host) != 1) {
                error = SessionError::TLS_HANDSHAKE;
                disconnect();
                return false;
            }
            if (SSL_connect(ssl_) != 1 || SSL_get_verify_result(ssl_) != X509_V_OK) {
                error = SessionError::TLS_VERIFICATION;
                disconnect();
                return false;
            }
            if (config_.pin_spki_base64[0] && !pin_matches(config_)) {
                error = SessionError::TLS_PIN_MISMATCH;
                disconnect();
                return false;
            }
        }
        uint8_t key_raw[16];
        FastRandom random;
        for (size_t i = 0; i < sizeof(key_raw); i += 8) {
            const uint64_t word = random.next_u64();
            std::memcpy(key_raw + i, &word, 8);
        }
        char key_b64[32]{};
        base64_encode(key_raw, sizeof(key_raw), key_b64);
        char authority[176];
        const bool default_port = (tls && port == 443) || (!tls && port == 80);
        const int authority_len = default_port
            ? std::snprintf(authority, sizeof(authority), "%s", host)
            : std::snprintf(authority, sizeof(authority), "%s:%d", host, port);
        char request[768];
        const int request_len = std::snprintf(
            request, sizeof(request),
            "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n", path, authority, key_b64);
        if (authority_len <= 0 || static_cast<size_t>(authority_len) >= sizeof(authority) ||
            request_len <= 0 || static_cast<size_t>(request_len) >= sizeof(request) ||
            !send_raw(request, static_cast<size_t>(request_len))) {
            error = SessionError::SEND_FAILED;
            disconnect();
            return false;
        }
        char expected[32]{};
        ws_url::websocket_accept(key_b64, expected);
        if (!read_upgrade(expected)) {
            error = SessionError::HANDSHAKE_REJECTED;
            disconnect();
            return false;
        }
        // The session is usable as soon as the handshake completes: the
        // subscription frame is the first write through send_frame(), which
        // refuses to run on a session that is not marked connected.
        connected_ = true;
        recv_len_ = 0;
        last_receive_ms_ = mono_ms();
        last_pong_ms_ = last_receive_ms_;
        last_ping_ms_ = last_receive_ms_;
        if (subscription_len && !send_text(subscription, subscription_len)) {
            error = SessionError::SEND_FAILED;
            disconnect();
            return false;
        }
        return true;
    }

    // Reads frames until the callback returns false, the peer closes, a timeout
    // expires or `request_stop()` is called.
    bool run(MessageCallback callback, void* context, SessionError& error) {
        error = SessionError::NONE;
        if (!connected_) {
            error = SessionError::CLOSED_BY_PEER;
            return false;
        }
        std::array<char, K_MAX_MESSAGE> message{};
        size_t fragment_len = 0;
        bool fragmenting = false;
        while (!stop_.load(std::memory_order_acquire)) {
            const uint64_t now = mono_ms();
            if (now - last_ping_ms_ >= config_.keepalive_interval_ms) {
                if (!send_text("PING", 4)) {
                    error = SessionError::SEND_FAILED;
                    return false;
                }
                last_ping_ms_ = now;
            }
            if (now - last_receive_ms_ >= config_.idle_timeout_ms) {
                error = SessionError::IDLE_TIMEOUT;
                return false;
            }
            if (now - last_pong_ms_ >= config_.pong_timeout_ms) {
                error = SessionError::PONG_TIMEOUT;
                return false;
            }
            fd_set reads;
            FD_ZERO(&reads);
            FD_SET(fd_, &reads);
            timeval timeout{0, 200000};
            const int ready = ::select(fd_ + 1, &reads, nullptr, nullptr, &timeout);
            if (stop_.load(std::memory_order_acquire)) {
                error = SessionError::STOPPED;
                return true;
            }
            if (ready < 0) {
                if (errno == EINTR) continue;
                error = SessionError::CLOSED_BY_PEER;
                return false;
            }
            if (ready == 0) continue;
            if (recv_len_ == recv_buffer_.size()) {
                error = SessionError::MESSAGE_TOO_LARGE;
                return false;
            }
            const ssize_t n = receive(recv_buffer_.data() + recv_len_,
                                      recv_buffer_.size() - recv_len_);
            if (n <= 0) {
                error = SessionError::CLOSED_BY_PEER;
                return false;
            }
            recv_len_ += static_cast<size_t>(n);
            last_receive_ms_ = mono_ms();

            size_t offset = 0;
            bool violated = false;
            while (offset + 2 <= recv_len_) {
                const uint8_t b0 = static_cast<uint8_t>(recv_buffer_[offset]);
                const uint8_t b1 = static_cast<uint8_t>(recv_buffer_[offset + 1]);
                const bool final = (b0 & 0x80U) != 0;
                const uint8_t opcode = b0 & 0x0FU;
                // Reserved bits must be zero and server frames must not be masked.
                if ((b0 & 0x70U) != 0 || (b1 & 0x80U) != 0) {
                    violated = true;
                    break;
                }
                uint64_t payload_len = b1 & 0x7FU;
                size_t header_len = 2;
                if (payload_len == 126) {
                    if (offset + 4 > recv_len_) break;
                    payload_len = static_cast<uint64_t>(
                        static_cast<uint8_t>(recv_buffer_[offset + 2])) * 256ULL +
                        static_cast<uint8_t>(recv_buffer_[offset + 3]);
                    if (payload_len < 126) { violated = true; break; }  // non-canonical
                    header_len = 4;
                } else if (payload_len == 127) {
                    if (offset + 10 > recv_len_) break;
                    payload_len = 0;
                    for (int i = 0; i < 8; ++i)
                        payload_len = payload_len * 256ULL +
                                      static_cast<uint8_t>(recv_buffer_[offset + 2 + i]);
                    if (payload_len < 65536ULL) { violated = true; break; }
                    header_len = 10;
                }
                if (payload_len > K_MAX_MESSAGE) {
                    violated = true;
                    break;
                }
                if (offset + header_len + payload_len > recv_len_) break;
                const char* payload = recv_buffer_.data() + offset + header_len;
                const size_t length = static_cast<size_t>(payload_len);
                offset += header_len + length;

                if (opcode == 0x8) {  // close
                    error = SessionError::CLOSED_BY_PEER;
                    send_close();
                    return false;
                }
                if (opcode == 0x9) {  // server ping → answer with a pong
                    if (length > K_MAX_CONTROL || !send_frame(0xA, payload, length)) {
                        violated = true;
                        break;
                    }
                    continue;
                }
                if (opcode == 0xA) {  // server pong
                    last_pong_ms_ = mono_ms();
                    continue;
                }
                if (opcode != 0x0 && opcode != 0x1 && opcode != 0x2) {
                    violated = true;  // unknown opcode
                    break;
                }
                const bool text = opcode == 0x1;
                const bool continuation = opcode == 0x0;
                if (fragmenting && !continuation) { violated = true; break; }
                if (!fragmenting && continuation) { violated = true; break; }
                if (fragment_len + length > message.size()) {
                    error = SessionError::MESSAGE_TOO_LARGE;
                    return false;
                }
                std::memcpy(message.data() + fragment_len, payload, length);
                fragment_len += length;
                fragmenting = !final;
                if (final) {
                    // Application-level keep-alive: the CLOB answers the text
                    // frame "PING" with the text frame "PONG".
                    if (fragment_len == 4 && std::memcmp(message.data(), "PONG", 4) == 0)
                        last_pong_ms_ = mono_ms();
                    const bool keep_going = callback(context, message.data(), fragment_len);
                    fragment_len = 0;
                    fragmenting = false;
                    if (!keep_going) {
                        error = SessionError::STOPPED;
                        return true;
                    }
                    (void)text;
                }
            }
            if (violated) {
                error = SessionError::PROTOCOL_VIOLATION;
                return false;
            }
            if (offset) {
                std::memmove(recv_buffer_.data(), recv_buffer_.data() + offset,
                             recv_len_ - offset);
                recv_len_ -= offset;
            }
        }
        error = SessionError::STOPPED;
        return true;
    }

    bool send_text(const char* payload, size_t len) { return send_frame(0x1, payload, len); }

    void request_stop() noexcept { stop_.store(true, std::memory_order_release); }
    bool connected() const noexcept { return connected_; }
    SessionError last_error() const noexcept { return last_error_; }
    uint64_t last_receive_ms() const noexcept { return last_receive_ms_; }
    uint64_t last_pong_ms() const noexcept { return last_pong_ms_; }

    // noexcept: it is called from ~Session(), and an exception escaping a
    // destructor calls std::terminate.  Nothing here allocates any more (the
    // masking CSPRNG is a member, seeded in connect()).
    void disconnect() noexcept {
        if (connected_) send_close();
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        connected_ = false;
        recv_len_ = 0;
        mask_random_.reset();  // the next connection reseeds from the OS
    }

private:
    static int tcp_connect(const char* host, int port, int timeout_ms) noexcept {
        char service[8];
        std::snprintf(service, sizeof(service), "%d", port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        if (::getaddrinfo(host, service, &hints, &addresses) != 0) return -1;
        int connected_fd = -1;
        for (addrinfo* address = addresses; address; address = address->ai_next) {
            const int fd = ::socket(address->ai_family, address->ai_socktype,
                                    address->ai_protocol);
            if (fd < 0) continue;
            const int flags = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int rc = ::connect(fd, address->ai_addr, address->ai_addrlen);
            if (rc != 0 && errno == EINPROGRESS) {
                fd_set writes;
                FD_ZERO(&writes);
                FD_SET(fd, &writes);
                timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
                rc = ::select(fd + 1, nullptr, &writes, nullptr, &timeout);
                if (rc > 0) {
                    int socket_error = 0;
                    socklen_t error_len = sizeof(socket_error);
                    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len);
                    rc = socket_error == 0 ? 0 : -1;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0) {
                ::fcntl(fd, F_SETFL, flags);
                connected_fd = fd;
                break;
            }
            ::close(fd);
        }
        ::freeaddrinfo(addresses);
        return connected_fd;
    }

    static void set_socket_options(int fd) noexcept {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        timeval timeout{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    static SSL_CTX* ssl_context() noexcept {
        static SSL_CTX* context = [] {
            SSL_CTX* value = SSL_CTX_new(TLS_client_method());
            if (!value) return static_cast<SSL_CTX*>(nullptr);
            SSL_CTX_set_verify(value, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_options(value, SSL_OP_NO_COMPRESSION);
            if (SSL_CTX_set_min_proto_version(value, TLS1_2_VERSION) != 1 ||
                SSL_CTX_set_default_verify_paths(value) != 1) {
                SSL_CTX_free(value);
                return static_cast<SSL_CTX*>(nullptr);
            }
            return value;
        }();
        return context;
    }

    bool pin_matches(const SessionConfig& config) const noexcept {
        X509* certificate = SSL_get_peer_certificate(ssl_);
        if (!certificate) return false;
        uint8_t der[4096];
        const int der_len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(certificate), nullptr);
        if (der_len <= 0 || static_cast<size_t>(der_len) > sizeof(der)) {
            X509_free(certificate);
            return false;
        }
        uint8_t* cursor = der;
        if (i2d_X509_PUBKEY(X509_get_X509_PUBKEY(certificate), &cursor) != der_len) {
            X509_free(certificate);
            return false;
        }
        uint8_t digest[32];
        sha256(der, static_cast<size_t>(der_len), digest);
        X509_free(certificate);
        char encoded[64];
        const size_t length = base64_encode(digest, sizeof(digest), encoded);
        if (length == 0 || length >= sizeof(encoded)) return false;
        encoded[length] = '\0';
        return std::strcmp(encoded, config.pin_spki_base64) == 0;
    }

    bool send_raw(const char* data, size_t len) noexcept {
        size_t offset = 0;
        while (offset < len) {
            const ssize_t n = ssl_
                ? SSL_write(ssl_, data + offset, static_cast<int>(len - offset))
                : ::send(fd_, data + offset, len - offset, MSG_NOSIGNAL);
            if (n <= 0) return false;
            offset += static_cast<size_t>(n);
        }
        return true;
    }

    ssize_t receive(char* data, size_t capacity) noexcept {
        if (ssl_) return SSL_read(ssl_, data, static_cast<int>(capacity));
        return ::recv(fd_, data, capacity, 0);
    }

    static bool header_value(const char* header, size_t length, const char* name,
                             const char* expected, bool token_list = false,
                             bool case_sensitive = false) noexcept {
        if (!header || !name || !expected) return false;
        const size_t name_len = std::strlen(name);
        size_t matches = 0;
        const char* line = header;
        const char* const end = header + length;
        while (line < end) {
            const char* newline = static_cast<const char*>(
                std::memchr(line, '\n', static_cast<size_t>(end - line)));
            const size_t line_len = newline ? static_cast<size_t>(newline - line)
                                            : static_cast<size_t>(end - line);
            const char* colon = static_cast<const char*>(
                std::memchr(line, ':', line_len));
            if (colon) {
                const size_t prefix_len = static_cast<size_t>(colon - line);
                if (prefix_len == name_len &&
                    (case_sensitive ? std::strncmp(line, name, name_len) == 0
                                    : strncasecmp(line, name, name_len) == 0)) {
                    const char* value = colon + 1;
                    const char* const value_end = line + line_len;
                    while (value < value_end && (*value == ' ' || *value == '\t')) ++value;
                    size_t value_len = static_cast<size_t>(value_end - value);
                    while (value_len && (value[value_len - 1] == '\r' ||
                                         value[value_len - 1] == ' ' ||
                                         value[value_len - 1] == '\t'))
                        --value_len;
                    bool found = false;
                    if (token_list) {
                        size_t i = 0;
                        while (i < value_len) {
                            size_t j = i;
                            while (j < value_len && value[j] != ',') ++j;
                            size_t token_end = j;
                            while (token_end > i && (value[token_end - 1] == ' ' ||
                                                     value[token_end - 1] == '\t'))
                                --token_end;
                            size_t token_start = i;
                            while (token_start < token_end &&
                                   (value[token_start] == ' ' ||
                                    value[token_start] == '\t'))
                                ++token_start;
                            const size_t token_len = token_end - token_start;
                            if (token_len == std::strlen(expected) &&
                                strncasecmp(value + token_start, expected, token_len) == 0)
                                found = true;
                            i = j + 1;
                        }
                    } else {
                        found = value_len == std::strlen(expected) &&
                                (case_sensitive
                                     ? std::strncmp(value, expected, value_len) == 0
                                     : strncasecmp(value, expected, value_len) == 0);
                    }
                    if (found) ++matches;
                }
            }
            if (!newline) break;
            line = newline + 1;
        }
        return matches == 1;
    }

    bool read_upgrade(const char* expected) noexcept {
        char header[K_MAX_HEADER];
        size_t length = 0;
        while (length + 1 < sizeof(header)) {
            const ssize_t n = receive(header + length, 1);
            if (n <= 0) return false;
            length += static_cast<size_t>(n);
            header[length] = '\0';
            if (length >= 4 && std::memcmp(header + length - 4, "\r\n\r\n", 4) == 0) {
                if (length < 13 || std::strncmp(header, "HTTP/1.1 101", 12) != 0 ||
                    (header[12] != ' ' && header[12] != '\r'))
                    return false;
                return header_value(header, length, "Sec-WebSocket-Accept", expected,
                                    false, true) &&
                       header_value(header, length, "Upgrade", "websocket") &&
                       header_value(header, length, "Connection", "upgrade", true);
            }
        }
        return false;
    }

    bool send_frame(uint8_t opcode, const char* payload, size_t len) noexcept {
        if (len > K_MAX_MESSAGE || !connected_) return false;
        uint8_t header[14];
        size_t header_len = 0;
        header[header_len++] = static_cast<uint8_t>(0x80U | opcode);
        if (len < 126) {
            header[header_len++] = static_cast<uint8_t>(0x80U | len);
        } else if (len <= 0xFFFF) {
            header[header_len++] = static_cast<uint8_t>(0x80U | 126U);
            header[header_len++] = static_cast<uint8_t>(len >> 8);
            header[header_len++] = static_cast<uint8_t>(len & 0xFFU);
        } else {
            header[header_len++] = static_cast<uint8_t>(0x80U | 127U);
            for (int i = 7; i >= 0; --i)
                header[header_len++] = static_cast<uint8_t>((len >> (i * 8)) & 0xFFU);
        }
        if (!mask_random_.has_value()) return false;  // never mask with garbage
        const uint64_t mask_word = mask_random_->next_u64();
        uint8_t mask[4];
        std::memcpy(mask, &mask_word, sizeof(mask));
        std::memcpy(header + header_len, mask, sizeof(mask));
        header_len += sizeof(mask);
        if (!send_raw(reinterpret_cast<const char*>(header), header_len)) return false;
        if (len == 0) return true;
        for (size_t i = 0; i < len; ++i)
            send_buffer_[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
        return send_raw(send_buffer_.data(), len);
    }

    void send_close() noexcept {
        if (!connected_) return;
        const uint8_t code[2] = {0x03, 0xE8};  // 1000 normal closure
        (void)send_frame(0x8, reinterpret_cast<const char*>(code), sizeof(code));
    }

    SessionConfig config_{};
    SSL* ssl_ = nullptr;
    int fd_ = -1;
    bool connected_ = false;
    std::atomic<bool> stop_{false};
    std::array<char, K_RECV_BUFFER> recv_buffer_{};
    std::array<char, K_MAX_MESSAGE> send_buffer_{};
    std::optional<FastRandom> mask_random_;
    size_t recv_len_ = 0;
    uint64_t last_receive_ms_ = 0;
    uint64_t last_pong_ms_ = 0;
    uint64_t last_ping_ms_ = 0;
    SessionError last_error_ = SessionError::NONE;
};

}  // namespace ws

#endif  // WS_SESSION_HPP
