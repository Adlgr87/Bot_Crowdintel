#ifndef WS_TRANSPORT_HPP
#define WS_TRANSPORT_HPP

// Shared WebSocket-over-TLS plumbing (Phase 3).
//
// Extracted so the user-data channel does not re-implement RFC 6455 by hand.
// Everything here is deterministic and side-effect free except the socket
// helpers, which is what lets the frame codec be unit tested without a network.
//
// Note: the market listener predates this header and still carries its own
// (tested) copies of these primitives. Migrating it here is a mechanical
// cleanup scheduled for the test-hardening phase; until then both paths are
// covered by their own suites.

#include <openssl/ssl.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>

#include "../crypto/fast_random.hpp"
#include "../crypto/sha256_engine.hpp"

namespace wstransport {

struct Url {
    char host[160]{};
    char path[192]{};
    int port = 443;
    bool tls = true;
};

inline bool parse_ws_url(const char* url, Url& out) noexcept {
    out = Url{};
    if (!url) return false;
    const char* cursor = url;
    if (std::strncmp(cursor, "wss://", 6) == 0) {
        out.tls = true;
        out.port = 443;
        cursor += 6;
    } else if (std::strncmp(cursor, "ws://", 5) == 0) {
        // Plain ws:// is accepted by the parser but live transports require
        // wss:// at configuration time.
        out.tls = false;
        out.port = 80;
        cursor += 5;
    } else {
        return false;
    }
    const char* slash = std::strchr(cursor, '/');
    const char* authority_end = slash ? slash : cursor + std::strlen(cursor);
    if (cursor == authority_end) return false;
    const char* colon = nullptr;
    for (const char* p = cursor; p < authority_end; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (*p == ':' && colon) return false;  // IPv6 literals unsupported
        if (*p == ':') colon = p;
        else if (*p == '@' || *p == '?' || *p == '#' || std::isspace(c))
            return false;
    }
    const char* host_end = colon ? colon : authority_end;
    const size_t host_len = static_cast<size_t>(host_end - cursor);
    if (host_len == 0 || host_len >= sizeof(out.host)) return false;
    std::memcpy(out.host, cursor, host_len);
    out.host[host_len] = '\0';
    if (colon) {
        char port_text[8];
        const size_t n = static_cast<size_t>(authority_end - colon - 1);
        if (n == 0 || n >= sizeof(port_text)) return false;
        std::memcpy(port_text, colon + 1, n);
        port_text[n] = '\0';
        errno = 0;
        char* port_end = nullptr;
        const long parsed = std::strtol(port_text, &port_end, 10);
        if (errno == ERANGE || !port_end || *port_end != '\0' || parsed <= 0 ||
            parsed > 65535)
            return false;
        out.port = static_cast<int>(parsed);
    }
    if (slash) {
        const size_t path_len = std::strlen(slash);
        if (path_len == 0 || path_len >= sizeof(out.path) ||
            std::strchr(slash, '#') || std::strchr(slash, ' '))
            return false;
        std::memcpy(out.path, slash, path_len + 1);
    } else {
        std::memcpy(out.path, "/", 2);
    }
    return true;
}

inline int tcp_connect(const char* host, int port, int timeout_ms) noexcept {
    char service[8];
    std::snprintf(service, sizeof(service), "%d", port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (::getaddrinfo(host, service, &hints, &addresses) != 0) return -1;
    int connected_fd = -1;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        int fd = ::socket(address->ai_family, address->ai_socktype,
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
                int error = 0;
                socklen_t error_len = sizeof(error);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len);
                rc = error == 0 ? 0 : -1;
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

inline void set_socket_options(int fd) noexcept {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    timeval timeout{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

// One shared TLS context: peer verification against the system trust store,
// TLS >= 1.2, no compression. Pin verification is applied per connection by
// the caller (SSL_set1_host / X509_VERIFY_PARAM pins).
inline SSL_CTX* shared_ssl_context() noexcept {
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

inline bool send_all(SSL* ssl, int fd, const char* data, size_t len) noexcept {
    size_t offset = 0;
    while (offset < len) {
        const ssize_t n = ssl
            ? SSL_write(ssl, data + offset, static_cast<int>(len - offset))
            : ::send(fd, data + offset, len - offset, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

inline ssize_t receive(SSL* ssl, int fd, char* data, size_t capacity) noexcept {
    return ssl ? SSL_read(ssl, data, static_cast<int>(capacity))
               : ::recv(fd, data, capacity, 0);
}

inline uint64_t now_mono_ms() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// ── RFC 6455 handshake ───────────────────────────────────────────────────────

// SHA-1 here exists only because RFC 6455 mandates it for the handshake check.
inline void sha1_bytes(const uint8_t* data, size_t len, uint8_t out[20]) noexcept {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    size_t offset = 0;
    auto block = [&](const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(p[i * 4]) << 24 |
                   static_cast<uint32_t>(p[i * 4 + 1]) << 16 |
                   static_cast<uint32_t>(p[i * 4 + 2]) << 8 |
                   static_cast<uint32_t>(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i) {
            const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    };
    while (offset + 64 <= len) {
        block(data + offset);
        offset += 64;
    }
    uint8_t tail[128]{};
    const size_t remaining = len - offset;
    std::memcpy(tail, data + offset, remaining);
    tail[remaining] = 0x80;
    const size_t final_offset = remaining >= 56 ? 120 : 56;
    for (int i = 0; i < 8; ++i)
        tail[final_offset + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    block(tail);
    if (remaining >= 56) block(tail + 64);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

inline void websocket_accept(const char* key, char out[32]) noexcept {
    static constexpr char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char input[96];
    const size_t key_len = std::strlen(key);
    std::memcpy(input, key, key_len);
    std::memcpy(input + key_len, GUID, sizeof(GUID) - 1);
    uint8_t hash[20];
    sha1_bytes(reinterpret_cast<const uint8_t*>(input), key_len + sizeof(GUID) - 1,
               hash);
    base64_encode(hash, sizeof(hash), out);
}

inline bool header_value(const char* header, size_t length, const char* name,
                         const char* expected, bool token_list = false,
                         bool case_sensitive = false) noexcept {
    const size_t name_len = std::strlen(name);
    const size_t expected_len = std::strlen(expected);
    size_t matches = 0;
    const char* cursor =
        static_cast<const char*>(std::memchr(header, '\n', length));
    if (!cursor) return false;
    ++cursor;
    const char* end = header + length;
    while (cursor < end) {
        const char* line_end = nullptr;
        for (const char* p = cursor; p + 1 < end; ++p) {
            if (p[0] == '\r' && p[1] == '\n') {
                line_end = p;
                break;
            }
        }
        if (!line_end || line_end == cursor) break;
        const char* colon = static_cast<const char*>(
            std::memchr(cursor, ':', static_cast<size_t>(line_end - cursor)));
        if (colon && static_cast<size_t>(colon - cursor) == name_len &&
            strncasecmp(cursor, name, name_len) == 0) {
            if (++matches > 1) return false;
            const char* value = colon + 1;
            while (value < line_end &&
                   std::isspace(static_cast<unsigned char>(*value))) ++value;
            const char* value_end = line_end;
            while (value_end > value &&
                   std::isspace(static_cast<unsigned char>(value_end[-1])))
                --value_end;
            if (!token_list) {
                if (static_cast<size_t>(value_end - value) != expected_len ||
                    (case_sensitive
                         ? std::memcmp(value, expected, expected_len) != 0
                         : strncasecmp(value, expected, expected_len) != 0))
                    return false;
            } else {
                bool found = false;
                const char* token = value;
                while (token < value_end) {
                    while (token < value_end &&
                           (*token == ',' || std::isspace(
                                static_cast<unsigned char>(*token)))) ++token;
                    const char* token_end = token;
                    while (token_end < value_end && *token_end != ',') ++token_end;
                    const char* trimmed = token_end;
                    while (trimmed > token &&
                           std::isspace(static_cast<unsigned char>(trimmed[-1])))
                        --trimmed;
                    if (static_cast<size_t>(trimmed - token) == expected_len &&
                        strncasecmp(token, expected, expected_len) == 0)
                        found = true;
                    token = token_end;
                }
                if (!found) return false;
            }
        }
        cursor = line_end + 2;
    }
    return matches == 1;
}

inline bool read_http_upgrade(SSL* ssl, int fd, const char* expected_accept,
                              int timeout_ms = 5000) noexcept {
    char header[4096];
    size_t length = 0;
    const uint64_t deadline = now_mono_ms() + static_cast<uint64_t>(timeout_ms);
    while (length + 1 < sizeof(header)) {
        if (now_mono_ms() > deadline) return false;
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(fd, &reads);
        timeval timeout{1, 0};
        const int ready = ::select(fd + 1, &reads, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (ready == 0) continue;
        const ssize_t n = receive(ssl, fd, header + length, 1);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK))
                continue;
            return false;
        }
        length += static_cast<size_t>(n);
        header[length] = '\0';
        if (length >= 4 &&
            std::memcmp(header + length - 4, "\r\n\r\n", 4) == 0) {
            if (length < 13 || std::strncmp(header, "HTTP/1.1 101", 12) != 0 ||
                (header[12] != ' ' && header[12] != '\r'))
                return false;
            return header_value(header, length, "Sec-WebSocket-Accept",
                                expected_accept, false, true) &&
                   header_value(header, length, "Upgrade", "websocket") &&
                   header_value(header, length, "Connection", "upgrade", true);
        }
    }
    return false;
}

// ── RFC 6455 frames ──────────────────────────────────────────────────────────

inline constexpr size_t kWsMaxPayload = 1U << 20;  // 1 MiB

// Encodes one masked client frame into `out`. The caller owns the buffer; this
// function never allocates.
inline bool encode_frame(uint8_t opcode, const char* payload, size_t len,
                         char* out, size_t out_cap, size_t& out_len) noexcept {
    out_len = 0;
    if (len > kWsMaxPayload) return false;
    const size_t header_len = len < 126 ? 2 : (len <= 0xFFFF ? 4 : 10);
    if (header_len + 4 + len > out_cap) return false;
    uint8_t* cursor = reinterpret_cast<uint8_t*>(out);
    *cursor++ = static_cast<uint8_t>(0x80U | opcode);
    if (len < 126) {
        *cursor++ = static_cast<uint8_t>(0x80U | len);
    } else if (len <= 0xFFFF) {
        *cursor++ = 0x80U | 126U;
        *cursor++ = static_cast<uint8_t>(len >> 8);
        *cursor++ = static_cast<uint8_t>(len);
    } else {
        *cursor++ = 0x80U | 127U;
        for (int i = 7; i >= 0; --i)
            *cursor++ = static_cast<uint8_t>(len >> (i * 8));
    }
    FastRandom random;
    const uint64_t mask_word = random.next_u64();
    uint8_t mask[4];
    std::memcpy(mask, &mask_word, sizeof(mask));
    std::memcpy(cursor, mask, sizeof(mask));
    cursor += sizeof(mask);
    for (size_t i = 0; i < len; ++i)
        cursor[i] = static_cast<uint8_t>(payload[i]) ^ mask[i & 3];
    out_len = header_len + 4 + len;
    return true;
}

enum class FrameStatus : uint8_t {
    kComplete = 0,
    kNeedMore = 1,
    kError = 2,
};

struct Frame {
    uint8_t opcode = 0;
    bool final = false;
    const char* payload = nullptr;
    size_t payload_len = 0;
    size_t consumed = 0;
};

// Decodes one server frame (server frames must not be masked). On kNeedMore the
// caller should read more bytes and retry; the frame length is bounded.
inline FrameStatus decode_frame(const char* buffer, size_t length,
                                Frame& out) noexcept {
    out = Frame{};
    if (length < 2) return FrameStatus::kNeedMore;
    const uint8_t b0 = static_cast<uint8_t>(buffer[0]);
    const uint8_t b1 = static_cast<uint8_t>(buffer[1]);
    out.final = (b0 & 0x80U) != 0;
    out.opcode = b0 & 0x0FU;
    if ((b0 & 0x70U) != 0) return FrameStatus::kError;  // reserved bits
    if ((b1 & 0x80U) != 0) return FrameStatus::kError;  // server frames are unmasked
    uint64_t payload_len = b1 & 0x7FU;
    size_t header_len = 2;
    if (payload_len == 126) {
        if (length < 4) return FrameStatus::kNeedMore;
        payload_len = static_cast<uint8_t>(buffer[2]) * 256ULL +
                      static_cast<uint8_t>(buffer[3]);
        if (payload_len < 126) return FrameStatus::kError;  // non-canonical
        header_len = 4;
    } else if (payload_len == 127) {
        if (length < 10) return FrameStatus::kNeedMore;
        if ((static_cast<uint8_t>(buffer[2]) & 0x80U) != 0)
            return FrameStatus::kError;
        payload_len = 0;
        for (int i = 0; i < 8; ++i)
            payload_len = (payload_len << 8) | static_cast<uint8_t>(buffer[2 + i]);
        if (payload_len <= 0xFFFFU) return FrameStatus::kError;
        header_len = 10;
    }
    const bool control = (out.opcode & 0x08U) != 0;
    if (out.opcode != 0x0 && out.opcode != 0x1 && out.opcode != 0x2 &&
        out.opcode != 0x8 && out.opcode != 0x9 && out.opcode != 0xA)
        return FrameStatus::kError;
    if (control && (!out.final || payload_len > 125))
        return FrameStatus::kError;
    if (payload_len > kWsMaxPayload) return FrameStatus::kError;
    if (length < header_len + payload_len) return FrameStatus::kNeedMore;
    out.payload = buffer + header_len;
    out.payload_len = static_cast<size_t>(payload_len);
    out.consumed = header_len + static_cast<size_t>(payload_len);
    return FrameStatus::kComplete;
}

}  // namespace wstransport

#endif  // WS_TRANSPORT_HPP
