#ifndef WS_URL_HPP
#define WS_URL_HPP

// WebSocket URL parsing and the RFC 6455 Sec-WebSocket-Accept computation.
// Kept free of OpenSSL/socket dependencies so both can be unit tested offline
// (the accept value is pinned by the RFC 6455 §1.3 known-answer vector).

#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../crypto/sha1.hpp"
#include "../crypto/sha256_engine.hpp"

namespace ws_url {

// Parses ws:// and wss:// URLs.  Rejects credentials in the URL, queries,
// fragments, whitespace, ambiguous ports and IPv6 literals (the venue
// endpoints are DNS names).
inline bool parse_url(const char* url, char* host, size_t host_cap, char* path,
                      size_t path_cap, int& port, bool& tls) noexcept {
    const char* cursor = url;
    if (!cursor || !host || !path || host_cap == 0 || path_cap == 0) return false;
    if (std::strncmp(cursor, "wss://", 6) == 0) {
        tls = true; port = 443; cursor += 6;
    } else if (std::strncmp(cursor, "ws://", 5) == 0) {
        tls = false; port = 80; cursor += 5;
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
        else if (*p == '@' || *p == '?' || *p == '#' || std::isspace(c)) return false;
    }
    const char* host_end = colon ? colon : authority_end;
    const size_t host_len = static_cast<size_t>(host_end - cursor);
    if (host_len == 0 || host_len >= host_cap) return false;
    std::memcpy(host, cursor, host_len);
    host[host_len] = '\0';
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
        port = static_cast<int>(parsed);
    }
    if (slash) {
        const size_t path_len = std::strlen(slash);
        if (path_len == 0 || path_len >= path_cap || std::strchr(slash, '#') ||
            std::strchr(slash, ' '))
            return false;
        std::memcpy(path, slash, path_len + 1);
    } else {
        if (path_cap < 2) return false;
        path[0] = '/';
        path[1] = '\0';
    }
    return true;
}

// base64(SHA1(client_key ‖ "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
inline void websocket_accept(const char* key, char out[32]) noexcept {
    static constexpr char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if (!key || !out) return;
    out[0] = '\0';
    char input[96];
    const size_t key_len = std::strlen(key);
    if (key_len + sizeof(GUID) - 1 > sizeof(input)) return;
    std::memcpy(input, key, key_len);
    std::memcpy(input + key_len, GUID, sizeof(GUID) - 1);
    uint8_t hash[20];
    sha1(reinterpret_cast<const uint8_t*>(input), key_len + sizeof(GUID) - 1, hash);
    base64_encode(hash, sizeof(hash), out);
}

}  // namespace ws_url

#endif  // WS_URL_HPP
