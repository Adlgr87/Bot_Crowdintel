#ifndef WS_MARKET_LISTENER_HPP
#define WS_MARKET_LISTENER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// WsMarketListener: REAL Polymarket CLOB market-channel feed.
//
// A hand-rolled RFC 6455 WebSocket client over OpenSSL TLS — no third-party
// WebSocket library, no JSON library. Rationale: the market feed is THE input
// to the hot path, so it must be dependency-light, allocation-free in
// steady state, and directly wired to the OrderBookL2 (producer side of the
// seqlock). Endpoints/messages per docs.polymarket.com (market channel):
//
//   wss://ws-subscriptions-clob.polymarket.com/ws/market
//   subscribe: {"assets_ids":["<token_id>"],"type":"market"}
//   events:    book (snapshot), price_change (deltas), tick_size_change
//
// Robustness: auto-reconnect with 1 s backoff + resubscribe; keepalive pings
// every 20 s; pong replies; fragmented-message reassembly; 1 MB message cap.
// JSON is scanned with a zero-allocation scanner tuned to this fixed schema
// (tolerates both {"price":..,"size":..} objects and [price,size] pairs).
// ─────────────────────────────────────────────────────────────────────────────

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../crypto/fast_random.hpp"
#include "../crypto/sha256_engine.hpp"
#include "../include/order_book.hpp"
#include "polymarket_order.hpp"

class WsMarketListener {
public:
    WsMarketListener(const MarketConfig& cfg, OrderBookL2& book)
        : cfg_(cfg), book_(book) {}

    ~WsMarketListener() { stop(); }

    void start() {
        running_ = true;
        thread_ = std::thread([this] { run_loop(); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    bool connected() const { return connected_.load(std::memory_order_acquire); }
    uint64_t events_seen() const { return events_.load(std::memory_order_relaxed); }
    uint64_t reconnects() const { return reconnects_.load(std::memory_order_relaxed); }

private:
    // ── SHA-1 (WebSocket handshake Accept validation only — not for security) ─
    static void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
        uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        uint64_t bits = len * 8;
        uint8_t msg[128];
        size_t full = len & ~63;
        for (size_t i = 0; i < full; i += 64) sha1_block(h, data + i);
        size_t rem = len - full;
        std::memcpy(msg, data + full, rem);
        msg[rem++] = 0x80;
        if (rem > 56) {
            std::memset(msg + rem, 0, 64 - rem);
            sha1_block(h, msg);
            rem = 0;
            std::memset(msg, 0, 56);
        } else {
            std::memset(msg + rem, 0, 56 - rem);
        }
        for (int i = 0; i < 8; i++) msg[56 + i] = (uint8_t)(bits >> (56 - i * 8));
        sha1_block(h, msg);
        for (int i = 0; i < 5; i++) {
            out[i*4]   = (uint8_t)(h[i] >> 24);
            out[i*4+1] = (uint8_t)(h[i] >> 16);
            out[i*4+2] = (uint8_t)(h[i] >> 8);
            out[i*4+3] = (uint8_t)h[i];
        }
    }
    static void sha1_block(uint32_t h[5], const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
                   (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
        for (int i = 16; i < 80; i++) {
            const uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }

    // ── Main listener loop (reconnect wrapper) ────────────────────────────────
    void run_loop() {
        while (running_) {
            if (!session()) {
                reconnects_.fetch_add(1, std::memory_order_relaxed);
                for (uint32_t i = 0; i < 100 && running_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    // One connection lifecycle. Returns false to trigger reconnect.
    bool session() {
        // 1. Parse wss://host/path
        char host[160] = {0}, path[160] = "/ws/market";
        bool tls = true;
        {
            const char* u = cfg_.ws_host;
            if (std::strncmp(u, "wss://", 6) == 0) { tls = true; u += 6; }
            else if (std::strncmp(u, "ws://", 5) == 0) { tls = false; u += 5; }
            const char* slash = std::strchr(u, '/');
            if (slash) {
                const size_t hl = (size_t)(slash - u);
                if (hl >= sizeof(host)) return false;
                std::memcpy(host, u, hl);
                std::snprintf(path, sizeof(path), "%.150s", slash);
            } else {
                std::snprintf(host, sizeof(host), "%.150s", u);
            }
            if (const char* colon = std::strchr(host, ':')) *(char*)colon = '\0';
        }

        // 2. TCP connect (blocking, with its own reconnect timeout upstream)
        int fd = tcp_connect(host, tls ? 443 : 80);
        if (fd < 0) return false;
        set_common_sockopts(fd);

        // 3. TLS
        SSL_CTX* sctx = ssl_ctx(tls);
        SSL* ssl = nullptr;
        if (tls) {
            ssl = SSL_new(sctx);
            if (!ssl) { ::close(fd); return false; }
            SSL_set_fd(ssl, fd);
            SSL_set_tlsext_host_name(ssl, host);          // SNI
            SSL_set1_host(ssl, host);                     // hostname verification
            if (SSL_connect(ssl) != 1) {
                SSL_free(ssl);
                ::close(fd);
                return false;
            }
        }

        // 4. WebSocket upgrade handshake
        uint8_t key_raw[16];
        {
            FastRandom r;
            for (auto& b : key_raw) b = (uint8_t)r.next_u64();
        }
        char key_b64[32];
        base64_encode(key_raw, 16, key_b64);
        char req[768];
        std::snprintf(req, sizeof(req),
            "GET %.150s HTTP/1.1\r\n"
            "Host: %.150s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            path, host, key_b64);
        if (!send_all(ssl, fd, req, std::strlen(req))) { cleanup(ssl, fd); return false; }

        char accept_expect[32];
        {
            static const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            char buf[64];
            const size_t kl = std::strlen(key_b64);
            std::memcpy(buf, key_b64, kl);
            std::memcpy(buf + kl, GUID, 36);
            uint8_t sha[20];
            sha1((const uint8_t*)buf, kl + 36, sha);
            base64_encode(sha, 20, accept_expect);
        }
        if (!read_http_upgrade(ssl, fd, accept_expect)) { cleanup(ssl, fd); return false; }

        // 5. Subscribe to the configured market
        char sub[160];
        const int sub_len = std::snprintf(sub, sizeof(sub),
            "{\"assets_ids\":[\"%s\"],\"type\":\"market\"}", cfg_.token_id_dec);
        if (!send_frame(ssl, fd, 0x1, sub, (size_t)sub_len)) { cleanup(ssl, fd); return false; }

        connected_.store(true, std::memory_order_release);

        // 6. Read/parse loop
        bool ok = read_frames(ssl, fd);
        connected_.store(false, std::memory_order_release);
        cleanup(ssl, fd);
        return ok;
    }

    int tcp_connect(const char* host, int port) {
        char service[8];
        std::snprintf(service, sizeof(service), "%d", port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        if (getaddrinfo(host, service, &hints, &list) != 0 || !list) return -1;
        int fd = -1;
        for (addrinfo* ai = list; ai; ai = ai->ai_next) {
            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(list);
        return fd;
    }

    static void set_common_sockopts(int fd) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    }

    SSL_CTX* ssl_ctx(bool tls) {
        static SSL_CTX* ctx = nullptr;
        if (!tls) return nullptr;
        if (!ctx) {
            ctx = SSL_CTX_new(TLS_client_method());
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(ctx);
        }
        return ctx;
    }

    static bool send_all(SSL* ssl, int fd, const char* data, size_t len) {
        size_t off = 0;
        while (off < len) {
            ssize_t n;
            if (ssl) n = SSL_write(ssl, data + off, (int)(len - off));
            else     n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }

    bool read_http_upgrade(SSL* ssl, int fd, const char* accept_expect) {
        // Accumulate until \r\n\r\n, then validate status + Sec-WebSocket-Accept.
        size_t n = 0;
        while (n < sizeof(hdr_) - 1) {
            if (!recv_some(ssl, fd, hdr_ + n, 1)) return false;
            ++n;
            hdr_[n] = '\0';
            if (n >= 4 && std::memcmp(hdr_ + n - 4, "\r\n\r\n", 4) == 0) {
                if (std::strncmp(hdr_, "HTTP/1.1 101", 12) != 0) return false;
                char want[64];
                std::snprintf(want, sizeof(want), "Sec-WebSocket-Accept: %s", accept_expect);
                // Case-insensitive-ish search (Polymarket sends canonical casing).
                if (!std::strstr(hdr_, want)) return false;
                return true;
            }
        }
        return false;
    }

    bool recv_some(SSL* ssl, int fd, char* out, size_t max_len) {
        size_t got = 0;
        while (got == 0) {
            if (ssl) {
                int r = SSL_read(ssl, out + got, (int)max_len);
                if (r <= 0) return false;
                got = (size_t)r;
            } else {
                ssize_t r = ::recv(fd, out + got, max_len, 0);
                if (r <= 0) return false;
                got = (size_t)r;
            }
        }
        return true;
    }

    bool send_frame(SSL* ssl, int fd, uint8_t opcode, const char* payload, size_t len) {
        uint8_t hdr[14];
        size_t hl = 0;
        hdr[hl++] = 0x80 | opcode;              // FIN + opcode
        uint8_t mask[4];
        {
            FastRandom r;
            const uint64_t m = r.next_u64();
            std::memcpy(mask, &m, 4);
        }
        if (len < 126) {
            hdr[hl++] = (uint8_t)(0x80 | len);
        } else if (len <= 0xFFFF) {
            hdr[hl++] = (uint8_t)(0x80 | 126);
            hdr[hl++] = (uint8_t)(len >> 8);
            hdr[hl++] = (uint8_t)len;
        } else {
            hdr[hl++] = (uint8_t)(0x80 | 127);
            for (int i = 7; i >= 0; --i) hdr[hl++] = (len >> (8 * i)) & 0xFF;
        }
        std::memcpy(hdr + hl, mask, 4);
        hl += 4;
        if (!send_all(ssl, fd, (const char*)hdr, hl)) return false;
        if (len) {
            // Masked payload (client→server MUST be masked per RFC 6455)
            thread_local std::vector<char> masked;
            if (masked.size() < len) masked.resize(len);
            for (size_t i = 0; i < len; ++i)
                masked[i] = payload[i] ^ mask[i & 3];
            return send_all(ssl, fd, masked.data(), len);
        }
        return true;
    }

    bool read_frames(SSL* ssl, int fd) {
        size_t len = 0;                        // buffered bytes in rbuf_
        size_t msg_len = 0;                    // accumulated fragmented message
        bool fragmenting = false;
        uint8_t frag_op = 0;
        uint64_t last_ping_ms = now_mono_ms();

        while (running_) {
            // Keepalive ping every 20 s
            const uint64_t now = now_mono_ms();
            if (now - last_ping_ms >= 20000) {
                if (!send_frame(ssl, fd, 0x9, "ci", 2)) return false;
                last_ping_ms = now;
            }

            // Top up the buffer
            if (rbuf_len_ < sizeof(rbuf_)) {
                fd_set rfds;
                FD_ZERO(&rfds);
                const int maxfd = fd;
                FD_SET(fd, &rfds);
                timeval tv{0, 200000};         // 200 ms select so we can ping/stop
                const int sel = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
                if (!running_) return true;
                if (sel > 0) {
                    ssize_t r;
                    if (ssl) r = SSL_read(ssl, rbuf_ + rbuf_len_, (int)(sizeof(rbuf_) - rbuf_len_));
                    else     r = ::recv(fd, rbuf_ + rbuf_len_, sizeof(rbuf_) - rbuf_len_, 0);
                    if (r <= 0) return false;
                    rbuf_len_ += (size_t)r;
                    len = rbuf_len_;
                } else if (sel < 0) {
                    return false;
                } else {
                    continue;                  // timeout — loop for ping/stop
                }
            } else {
                return false;                  // buffer overrun of unparseable data
            }

            // Parse as many complete frames as the buffer holds
            size_t off = 0;
            while (true) {
                if (len - off < 2) break;
                const uint8_t b0 = (uint8_t)rbuf_[off];
                const uint8_t b1 = (uint8_t)rbuf_[off + 1];
                const bool fin = b0 & 0x80;
                const uint8_t opcode = b0 & 0x0F;
                if (b1 & 0x80) return false;   // server MUST NOT mask
                uint64_t plen = b1 & 0x7F;
                size_t hlen = 2;
                if (plen == 126) {
                    if (len - off < 4) break;
                    plen = ((uint64_t)(uint8_t)rbuf_[off+2] << 8) | (uint8_t)rbuf_[off+3];
                    hlen = 4;
                } else if (plen == 127) {
                    if (len - off < 10) break;
                    plen = 0;
                    for (int i = 0; i < 8; ++i)
                        plen = (plen << 8) | (uint8_t)rbuf_[off + 2 + i];
                    hlen = 10;
                }
                if (plen > (1ULL << 20)) return false;   // 1 MB message cap
                if (len - off < hlen + plen) break;      // need more bytes

                const char* payload = rbuf_ + off + hlen;
                off += hlen + (size_t)plen;

                if (opcode == 0x8) {                     // close
                    const uint8_t code_be[2] = {0x03, 0xE8};   // 1000 normal
                    send_frame(ssl, fd, 0x8, (const char*)code_be, 2);
                    return true;                          // graceful → resubscribe
                } else if (opcode == 0x9) {              // ping → pong
                    send_frame(ssl, fd, 0xA, payload, (size_t)plen);
                } else if (opcode == 0xA) {              // pong
                } else if (opcode == 0x1) {              // text
                    handle_message(payload, (size_t)plen);
                } else if (opcode == 0x2) {              // binary (unused)
                } else if (opcode == 0x0) {              // continuation
                    if (fragmenting && msg_len + plen <= (1ULL << 20)) {
                        std::memcpy(msg_ + msg_len, payload, (size_t)plen);
                        msg_len += (size_t)plen;
                        if (fin) {
                            handle_message(msg_, msg_len);
                            fragmenting = false;
                        }
                    }
                }
                if ((opcode == 0x1 || opcode == 0x2) && !fin) {
                    fragmenting = true;
                    frag_op = opcode;
                    std::memcpy(msg_, payload, (size_t)plen);
                    msg_len = (size_t)plen;
                    (void)frag_op;
                }
            }

            // Compact the buffer
            if (off) {
                std::memmove(rbuf_, rbuf_ + off, len - off);
                rbuf_len_ = len - off;
                len = rbuf_len_;
            }
        }
        return true;
    }

public:
    // ── Message classification (public static: unit-testable) ────────────────
    static bool msg_is_book(const char* j, size_t len) {
        static constexpr char K_BOOK[] = "\"event_type\":\"book\"";
        return search(j, len, K_BOOK, sizeof(K_BOOK) - 1);
    }
    static bool msg_is_price_change(const char* j, size_t len) {
        static constexpr char K_CHG[] = "\"event_type\":\"price_change\"";
        return search(j, len, K_CHG, sizeof(K_CHG) - 1);
    }

    // ── Market JSON → OrderBookL2 ────────────────────────────────────────────
    void handle_message(const char* j, size_t len) {
        events_.fetch_add(1, std::memory_order_relaxed);
        static constexpr char K_BOOK[] = "\"event_type\":\"book\"";
        static constexpr char K_CHG[]  = "\"event_type\":\"price_change\"";
        static constexpr char K_TICK[] = "\"event_type\":\"tick_size_change\"";

        if (search(j, len, K_BOOK, sizeof(K_BOOK) - 1)) {
            parse_book_snapshot(j, len);
        } else if (search(j, len, K_CHG, sizeof(K_CHG) - 1)) {
            parse_price_change(j, len);
        } else if (search(j, len, K_TICK, sizeof(K_TICK) - 1)) {
            // The engine clamps prices defensively; nothing to do in the
            // hot book — the snapshot on the next book event refreshes it.
        }
    }

    static bool search(const char* j, size_t len, const char* needle, size_t nlen) {
        if (len < nlen) return false;
        for (size_t i = 0; i + nlen <= len; ++i)
            if (j[i] == needle[0] && std::memcmp(j + i, needle, nlen) == 0) return true;
        return false;
    }

    // Parse "bids":[ ... ] / "asks":[ ... ] arrays of pairs or objects.
    void parse_book_snapshot(const char* j, size_t len) {
        Level2Entry bids[OrderBookL2::MAX_LEVELS];
        Level2Entry asks[OrderBookL2::MAX_LEVELS];
        size_t nb = parse_levels(j, len, "\"bids\"", bids, MAXLVL);
        size_t na = parse_levels(j, len, "\"asks\"", asks, MAXLVL);
        sort_levels(bids, nb, true);    // descending
        sort_levels(asks, na, false);   // ascending
        book_.set_bids(bids, nb);
        book_.set_asks(asks, na);
    }

    void parse_price_change(const char* j, size_t len) {
        // "changes":[{"price":"0.4","side":"BUY","size":"0"}, ...]
        const char* arr = std::strstr(j, "\"changes\"");
        if (!arr) return;
        const char* p = std::strchr(arr, '[');
        if (!p) return;
        Level2Entry bids[OrderBookL2::MAX_LEVELS];
        Level2Entry asks[OrderBookL2::MAX_LEVELS];
        size_t nb = current_levels(true, bids);
        size_t na = current_levels(false, asks);

        while ((p = std::strchr(p + 1, '{')) != nullptr) {
            const char* obj_end = std::strchr(p, '}');
            if (!obj_end) break;
            char price_s[24] = {0}, side_s[8] = {0}, size_s[24] = {0};
            extract_string(p, (size_t)(obj_end - p), "\"price\"", price_s, sizeof(price_s));
            extract_string(p, (size_t)(obj_end - p), "\"side\"", side_s, sizeof(side_s));
            extract_string(p, (size_t)(obj_end - p), "\"size\"", size_s, sizeof(size_s));
            uint64_t price = 0, size = 0;
            if (price_s[0] && size_s[0] &&
                parse_fixed1e6(price_s, std::strlen(price_s), price) &&
                parse_fixed1e6(size_s, std::strlen(size_s), size)) {
                const bool is_buy = (side_s[0] == 'B' || side_s[0] == 'b');
                upsert_level(is_buy ? bids : asks, is_buy ? nb : na, price, size);
            }
            p = obj_end;
        }
        sort_levels(bids, nb, true);
        sort_levels(asks, na, false);
        book_.set_bids(bids, nb);
        book_.set_asks(asks, na);
    }

    static constexpr size_t MAXLVL = OrderBookL2::MAX_LEVELS;

    size_t current_levels(bool is_bid, Level2Entry* out) {
        size_t n = 0;
        for (size_t i = 0; i < MAXLVL; ++i) {
            const Level2Entry e = is_bid ? book_.get_bid(i) : book_.get_ask(i);
            if (e.size == 0) break;
            out[n++] = e;
        }
        return n;
    }

    // Scan a JSON array keyed by `key` ("bids"/"asks") into entries.
    static size_t parse_levels(const char* j, size_t len, const char* key,
                               Level2Entry* out, size_t cap) {
        const char* key_pos = std::strstr(j, key);
        if (!key_pos) return 0;
        const char* arr = std::strchr(key_pos, '[');
        if (!arr) return 0;
        const char* p = arr + 1;
        size_t n = 0;
        while (*p && *p != ']' && n < cap) {
            if (*p == '{') {
                const char* end = std::strchr(p, '}');
                if (!end) break;
                char price_s[24] = {0}, size_s[24] = {0};
                extract_string(p, (size_t)(end - p), "\"price\"", price_s, sizeof(price_s));
                extract_string(p, (size_t)(end - p), "\"size\"", size_s, sizeof(size_s));
                uint64_t pr = 0, sz = 0;
                if (price_s[0] && size_s[0] &&
                    parse_fixed1e6(price_s, std::strlen(price_s), pr) &&
                    parse_fixed1e6(size_s, std::strlen(size_s), sz))
                    out[n++] = {pr, sz};
                p = end + 1;
            } else if (*p == '[') {
                const char* end = std::strchr(p, ']');
                if (!end) break;
                char vals[2][24] = {{0},{0}};
                int vi = 0;
                const char* q = p + 1;
                while (q < end && vi < 2) {
                    if (*q == '"') {
                        const char* close = std::strchr(q + 1, '"');
                        if (!close) break;
                        const size_t cl = (size_t)(close - q - 1);
                        if (cl < sizeof(vals[0])) {
                            std::memcpy(vals[vi], q + 1, cl);
                            ++vi;
                        }
                        q = close + 1;
                    } else {
                        ++q;
                    }
                }
                uint64_t pr = 0, sz = 0;
                if (vi == 2 &&
                    parse_fixed1e6(vals[0], std::strlen(vals[0]), pr) &&
                    parse_fixed1e6(vals[1], std::strlen(vals[1]), sz))
                    out[n++] = {pr, sz};
                p = end + 1;
            } else if (*p == ',') {
                ++p;
            } else {
                ++p;
            }
        }
        return n;
    }

    static void extract_string(const char* obj, size_t obj_len, const char* key,
                               char* out, size_t cap) {
        const char* k = std::strstr(obj, key);
        if (!k || (size_t)(k - obj) > obj_len) return;
        const char* q1 = std::strchr(k + std::strlen(key), '"');
        if (!q1) return;
        const char* q2 = std::strchr(q1 + 1, '"');
        if (!q2) return;
        const size_t n = (size_t)(q2 - q1 - 1);
        if (n < cap) {
            std::memcpy(out, q1 + 1, n);
            out[n] = '\0';
        }
    }

    static void upsert_level(Level2Entry* levels, size_t& n, uint64_t price, uint64_t size) {
        for (size_t i = 0; i < n; ++i) {
            if (levels[i].price == price) {
                if (size == 0) {
                    for (size_t k2 = i; k2 + 1 < n; ++k2) levels[k2] = levels[k2 + 1];
                    --n;
                } else {
                    levels[i].size = size;
                }
                return;
            }
        }
        if (size != 0 && n < MAXLVL) levels[n++] = {price, size};
    }

    static void sort_levels(Level2Entry* v, size_t n, bool descending) {
        // Tiny insertion sort — n ≤ 100, cache-friendly, no alloc.
        for (size_t i = 1; i < n; ++i) {
            const Level2Entry key = v[i];
            size_t jj = i;
            while (jj > 0 && (descending ? v[jj-1].price < key.price
                                         : v[jj-1].price > key.price)) {
                v[jj] = v[jj-1];
                --jj;
            }
            v[jj] = key;
        }
    }

    static uint64_t now_mono_ms() {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    }

    static void cleanup(SSL* ssl, int fd) {
        if (ssl) SSL_free(ssl);
        if (fd >= 0) ::close(fd);
    }

    const MarketConfig& cfg_;
    OrderBookL2& book_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> events_{0};
    std::atomic<uint64_t> reconnects_{0};
    char rbuf_[65536];
    size_t rbuf_len_ = 0;
    char hdr_[2048];
    char msg_[1 << 20];
};

#endif // WS_MARKET_LISTENER_HPP
