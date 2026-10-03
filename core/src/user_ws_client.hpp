#ifndef USER_WS_CLIENT_HPP
#define USER_WS_CLIENT_HPP

// Phase 3 — authenticated user-data WebSocket channel.
//
// Source of the message contract: Polymarket AsyncAPI "User Channel" spec
// (docs.polymarket.com/asyncapi-user.json), channel `/ws/user`:
//   * connect, then send the authenticated subscription request
//     {"auth":{"apiKey","secret","passphrase"},"type":"user","markets":[condition]}
//   * the client sends the text frame "PING" every 10 seconds; the server
//     replies with the text frame "PONG";
//   * events arrive as OrderEvent (event_type "order") and TradeEvent
//     (event_type "trade") payloads.
//
// Fail-closed rules implemented here:
//   * credentials that cannot be embedded in the JSON payload without escaping
//     abort the session instead of emitting a malformed/lossy subscribe;
//   * an unparsable, unattributable, or unknown-status event is counted as a
//     divergence (the caller must reconcile before trading);
//   * events for another market than the configured condition id are dropped;
//   * a session that stops receiving data is dropped and reconnected.
//
// This header holds no credentials beyond the lifetime of a connection: the
// subscription payload is built in a local buffer and zeroed after sending.

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "../include/bounded_json.hpp"
#include "../include/json_field.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "market_config.hpp"
#include "ws_transport.hpp"

inline constexpr size_t kUserEventQueue = 256;
inline constexpr size_t kUserWsMaxMakers = 8;

enum class UserEventKind : uint8_t { kOrder = 0, kTrade = 1 };
enum class UserOrderType : uint8_t {
    kPlacement = 0, kUpdate = 1, kCancellation = 2, kUnknown = 3,
};
enum class UserOrderStatus : uint8_t {
    kLive = 0, kMatched = 1, kCanceled = 2, kUnknown = 3,
};
enum class UserTradeStatus : uint8_t {
    kMatched = 0, kMined = 1, kConfirmed = 2, kRetrying = 3, kFailed = 4,
    kUnknown = 5,
};
enum class UserSide : uint8_t { kBuy = 0, kSell = 1, kUnknown = 2 };
enum class UserTraderSide : uint8_t { kTaker = 0, kMaker = 1, kUnknown = 2 };

struct UserEvent {
    struct MakerFill {
        char order_id[96]{};
        char owner[80]{};
        uint64_t matched_fixed6 = 0;
    };

    UserEventKind kind = UserEventKind::kOrder;
    char venue_order_id[96]{};   // OrderEvent.id / TradeEvent.taker_order_id
    char trade_id[96]{};         // TradeEvent.id
    char market[67]{};           // condition id
    char asset_id[79]{};         // outcome token id
    char owner[80]{};            // OrderEvent.owner / TradeEvent.owner
    char transaction_hash[80]{}; // TradeEvent only
    UserSide side = UserSide::kUnknown;
    UserOrderType order_type = UserOrderType::kUnknown;
    UserOrderStatus order_status = UserOrderStatus::kUnknown;
    UserTradeStatus trade_status = UserTradeStatus::kUnknown;
    UserTraderSide trader_side = UserTraderSide::kUnknown;
    bool has_trader_side = false;
    uint64_t original_fixed6 = 0;      // shares, 1e-6
    uint64_t size_matched_fixed6 = 0;  // OrderEvent cumulative matched
    uint64_t trade_size_fixed6 = 0;    // TradeEvent size (this trade)
    uint64_t price_fixed6 = 0;
    uint64_t fee_rate_bps = 0;
    uint64_t timestamp_ms = 0;
    size_t maker_count = 0;
    MakerFill makers[kUserWsMaxMakers]{};
};

namespace user_ws {

inline bool fixed_u64(const char* json, size_t length, const char* key,
                      uint64_t& out) noexcept {
    uint64_t scaled = 0;
    if (!json_field::fixed6(json, length, key, scaled)) return false;
    if (scaled % 1000000ULL != 0) return false;  // integers only for this field
    out = scaled / 1000000ULL;
    return true;
}

inline UserSide parse_side(const char* text) noexcept {
    if (std::strcmp(text, "BUY") == 0) return UserSide::kBuy;
    if (std::strcmp(text, "SELL") == 0) return UserSide::kSell;
    return UserSide::kUnknown;
}

inline UserOrderType parse_order_type_field(const char* text) noexcept {
    if (std::strcmp(text, "PLACEMENT") == 0) return UserOrderType::kPlacement;
    if (std::strcmp(text, "UPDATE") == 0) return UserOrderType::kUpdate;
    if (std::strcmp(text, "CANCELLATION") == 0) return UserOrderType::kCancellation;
    return UserOrderType::kUnknown;
}

inline UserOrderStatus parse_order_status(const char* text) noexcept {
    if (std::strcmp(text, "LIVE") == 0) return UserOrderStatus::kLive;
    if (std::strcmp(text, "MATCHED") == 0) return UserOrderStatus::kMatched;
    if (std::strcmp(text, "CANCELED") == 0) return UserOrderStatus::kCanceled;
    return UserOrderStatus::kUnknown;
}

inline UserTradeStatus parse_trade_status(const char* text) noexcept {
    if (std::strcmp(text, "MATCHED") == 0) return UserTradeStatus::kMatched;
    if (std::strcmp(text, "MINED") == 0) return UserTradeStatus::kMined;
    if (std::strcmp(text, "CONFIRMED") == 0) return UserTradeStatus::kConfirmed;
    if (std::strcmp(text, "RETRYING") == 0) return UserTradeStatus::kRetrying;
    if (std::strcmp(text, "FAILED") == 0) return UserTradeStatus::kFailed;
    return UserTradeStatus::kUnknown;
}

// Parses one OrderEvent or TradeEvent object. Every field the AsyncAPI spec
// marks required is required here too; unknown enum values are preserved as
// kUnknown so the caller can treat them as a divergence instead of guessing.
inline bool parse_event_object(const char* json, size_t length,
                               UserEvent& out) noexcept {
    out = UserEvent{};
    if (!json || length == 0 || !bounded_json::valid_document(json, length))
        return false;
    char event_type[16]{};
    if (!json_field::string(json, length, "event_type", event_type,
                            sizeof(event_type)))
        return false;

    if (std::strcmp(event_type, "order") == 0) {
        char side_text[16]{};
        char type_text[16]{};
        if (!json_field::string(json, length, "id", out.venue_order_id,
                                sizeof(out.venue_order_id)) ||
            !json_field::string(json, length, "owner", out.owner,
                                sizeof(out.owner)) ||
            !json_field::string(json, length, "market", out.market,
                                sizeof(out.market)) ||
            !json_field::string(json, length, "asset_id", out.asset_id,
                                sizeof(out.asset_id)) ||
            !json_field::string(json, length, "side", side_text,
                                sizeof(side_text)) ||
            !json_field::fixed6(json, length, "original_size",
                                out.original_fixed6) ||
            !json_field::fixed6(json, length, "size_matched",
                                out.size_matched_fixed6) ||
            !json_field::fixed6(json, length, "price", out.price_fixed6) ||
            !json_field::string(json, length, "type", type_text,
                                sizeof(type_text)) ||
            !fixed_u64(json, length, "timestamp", out.timestamp_ms))
            return false;
        out.kind = UserEventKind::kOrder;
        out.side = parse_side(side_text);
        out.order_type = parse_order_type_field(type_text);
        // status is not in the spec's required list: absence is preserved as
        // kUnknown and treated as a divergence by the applier.
        char status[24]{};
        if (json_field::string(json, length, "status", status, sizeof(status)))
            out.order_status = parse_order_status(status);
        if (out.original_fixed6 == 0) return false;
        return true;
    }

    if (std::strcmp(event_type, "trade") == 0) {
        char side_text[16]{};
        char status_text[24]{};
        char type_field[16]{};
        if (!json_field::string(json, length, "type", type_field,
                                sizeof(type_field)) ||
            std::strcmp(type_field, "TRADE") != 0 ||
            !json_field::string(json, length, "id", out.trade_id,
                                sizeof(out.trade_id)) ||
            !json_field::string(json, length, "taker_order_id",
                                out.venue_order_id,
                                sizeof(out.venue_order_id)) ||
            !json_field::string(json, length, "market", out.market,
                                sizeof(out.market)) ||
            !json_field::string(json, length, "asset_id", out.asset_id,
                                sizeof(out.asset_id)) ||
            !json_field::string(json, length, "side", side_text,
                                sizeof(side_text)) ||
            !json_field::fixed6(json, length, "size", out.trade_size_fixed6) ||
            !json_field::fixed6(json, length, "price", out.price_fixed6) ||
            !json_field::string(json, length, "status", status_text,
                                sizeof(status_text)) ||
            !json_field::string(json, length, "owner", out.owner,
                                sizeof(out.owner)) ||
            !fixed_u64(json, length, "timestamp", out.timestamp_ms))
            return false;
        out.kind = UserEventKind::kTrade;
        out.side = parse_side(side_text);
        out.trade_status = parse_trade_status(status_text);
        if (out.trade_size_fixed6 == 0) return false;
        if (json_field::string(json, length, "transaction_hash",
                               out.transaction_hash, sizeof(out.transaction_hash)))
            (void)0;
        if (fixed_u64(json, length, "fee_rate_bps", out.fee_rate_bps)) (void)0;

        char trader[16]{};
        if (json_field::string(json, length, "trader_side", trader,
                               sizeof(trader))) {
            out.has_trader_side = true;
            if (std::strcmp(trader, "TAKER") == 0)
                out.trader_side = UserTraderSide::kTaker;
            else if (std::strcmp(trader, "MAKER") == 0)
                out.trader_side = UserTraderSide::kMaker;
        }

        // maker_orders[] entries that belong to us carry the fill of our
        // resting order; dropping them would hide inventory.
        const char* array = nullptr;
        if (json_field::locate(json, length, "maker_orders", &array) == 1) {
            const char* cursor = array;
            const char* end = json + length;
            while (cursor < end && json_field::is_space(*cursor)) ++cursor;
            if (cursor < end && *cursor == '[') {
                ++cursor;
                while (cursor < end && json_field::is_space(*cursor)) ++cursor;
                while (cursor < end && *cursor == '{') {
                    const char* object_begin = cursor;
                    if (!json_field::skip_value(cursor, end)) return false;
                    const size_t object_len =
                        static_cast<size_t>(cursor - object_begin);
                    if (out.maker_count >= kUserWsMaxMakers) return false;
                    UserEvent::MakerFill& fill = out.makers[out.maker_count];
                    if (!json_field::string(object_begin, object_len, "order_id",
                                            fill.order_id,
                                            sizeof(fill.order_id)) ||
                        !json_field::string(object_begin, object_len, "owner",
                                            fill.owner, sizeof(fill.owner)) ||
                        !json_field::fixed6(object_begin, object_len,
                                            "matched_amount",
                                            fill.matched_fixed6))
                        return false;
                    ++out.maker_count;
                    while (cursor < end && json_field::is_space(*cursor)) ++cursor;
                    if (cursor < end && *cursor == ',') {
                        ++cursor;
                        while (cursor < end && json_field::is_space(*cursor))
                            ++cursor;
                    }
                }
            }
        }
        return true;
    }
    return false;
}

// Parses a channel payload. The AsyncAPI examples show single event objects;
// the market channel uses arrays, so both encodings are accepted and anything
// else is rejected. Returns the number of events written to `out` (0 = the
// message carried no event).
inline size_t parse_message(const char* json, size_t length, UserEvent* out,
                            size_t capacity) noexcept {
    if (!json || !out || capacity == 0 || length == 0 ||
        !bounded_json::valid_document(json, length))
        return 0;
    const char* cursor = json;
    const char* end = json + length;
    while (cursor < end && json_field::is_space(*cursor)) ++cursor;
    if (cursor == end) return 0;
    if (*cursor != '[') {
        if (*cursor != '{') return 0;
        return parse_event_object(cursor, static_cast<size_t>(end - cursor), out[0])
                   ? 1
                   : 0;
    }
    ++cursor;
    size_t count = 0;
    while (cursor < end) {
        while (cursor < end && json_field::is_space(*cursor)) ++cursor;
        if (cursor == end) break;
        if (*cursor == ']') break;
        if (*cursor != '{') return 0;
        const char* object_begin = cursor;
        if (!json_field::skip_value(cursor, end)) return 0;
        const size_t object_len = static_cast<size_t>(cursor - object_begin);
        if (count >= capacity) return 0;  // more events than the caller can hold
        if (!parse_event_object(object_begin, object_len, out[count])) return 0;
        ++count;
        while (cursor < end && json_field::is_space(*cursor)) ++cursor;
        if (cursor < end && *cursor == ',') {
            ++cursor;
            continue;
        }
    }
    return count;
}

// True when the value can sit inside a JSON string without escaping. Anything
// else aborts the subscribe: a mangled credential payload must never be sent,
// and silently escaping could send the wrong secret.
inline bool json_safe_value(const char* value) noexcept {
    if (!value || !*value) return false;
    for (const char* p = value; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    return true;
}

// Builds the authenticated subscription request. Returns false (and writes
// nothing usable) when credentials are missing, unsafe, or the condition id is
// not a valid identifier.
inline bool build_subscription(const char* api_key, const char* secret,
                               const char* passphrase, const char* condition_id,
                               char* out, size_t out_cap,
                               size_t& out_len) noexcept {
    out_len = 0;
    if (!json_safe_value(api_key) || !json_safe_value(secret) ||
        !json_safe_value(passphrase) || !json_safe_value(condition_id))
        return false;
    const int written = std::snprintf(
        out, out_cap,
        "{\"auth\":{\"apiKey\":\"%s\",\"secret\":\"%s\",\"passphrase\":\"%s\"},"
        "\"type\":\"user\",\"markets\":[\"%s\"]}",
        api_key, secret, passphrase, condition_id);
    if (written <= 0 || static_cast<size_t>(written) >= out_cap) return false;
    out_len = static_cast<size_t>(written);
    return true;
}

// {"operation":"subscribe"|"unsubscribe","markets":["0x…"]}
inline bool build_subscription_update(const char* operation,
                                      const char* condition_id, char* out,
                                      size_t out_cap, size_t& out_len) noexcept {
    out_len = 0;
    if (!json_safe_value(operation) || !json_safe_value(condition_id)) return false;
    if (std::strcmp(operation, "subscribe") != 0 &&
        std::strcmp(operation, "unsubscribe") != 0)
        return false;
    const int written = std::snprintf(out, out_cap,
                                      "{\"operation\":\"%s\",\"markets\":[\"%s\"]}",
                                      operation, condition_id);
    if (written <= 0 || static_cast<size_t>(written) >= out_cap) return false;
    out_len = static_cast<size_t>(written);
    return true;
}

}  // namespace user_ws

class UserWsClient {
public:
    explicit UserWsClient(const MarketConfig& cfg) : cfg_(cfg) {}
    ~UserWsClient() { stop(); }
    UserWsClient(const UserWsClient&) = delete;
    UserWsClient& operator=(const UserWsClient&) = delete;

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        thread_ = std::thread([this] { run_loop(); });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        const int fd = active_fd_.load(std::memory_order_acquire);
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
    }

    // Consumer side (hot loop). Returns false when the queue is empty.
    bool poll(UserEvent& out) { return queue_.try_pop(out); }

    bool connected() const { return connected_.load(std::memory_order_acquire); }
    bool channel_healthy() const {
        return connected() && !auth_error_.load(std::memory_order_acquire);
    }
    bool auth_error() const { return auth_error_.load(std::memory_order_acquire); }
    uint64_t reconnects() const { return reconnects_.load(std::memory_order_relaxed); }
    uint64_t events_queued() const { return events_queued_.load(std::memory_order_relaxed); }
    uint64_t events_dropped() const { return events_dropped_.load(std::memory_order_relaxed); }
    uint64_t pings_sent() const { return pings_sent_.load(std::memory_order_relaxed); }
    uint64_t pongs_seen() const { return pongs_seen_.load(std::memory_order_relaxed); }
    uint64_t malformed_messages() const {
        return malformed_.load(std::memory_order_relaxed);
    }
    uint64_t foreign_market_events() const {
        return foreign_.load(std::memory_order_relaxed);
    }
    uint64_t server_errors() const { return server_errors_.load(std::memory_order_relaxed); }
    const char* last_server_error() const { return last_server_error_; }

    // WS_HOST is the market channel; the user channel lives at "/ws/user" on
    // the same authority (docs.polymarket.com/asyncapi-user.json). An explicit
    // BOT_USER_WS_HOST always wins, and an unrecognized path is refused rather
    // than guessed.
    static bool derive_user_url(const char* user_ws_host, const char* ws_host,
                                wstransport::Url& url) noexcept {
        const char* source = (user_ws_host && *user_ws_host) ? user_ws_host
                                                            : ws_host;
        if (!source || !*source) return false;
        if (!wstransport::parse_ws_url(source, url)) return false;
        if (user_ws_host && *user_ws_host) return true;
        if (std::strcmp(url.path, "/ws/market") == 0) {
            std::memcpy(url.path, "/ws/user", 9);
            return true;
        }
        return std::strcmp(url.path, "/ws/user") == 0;
    }

    // An event that names another market is not ours to apply. Without a
    // resolved condition id nothing can be attributed, so nothing is accepted.
    static bool is_our_market(const MarketConfig& cfg,
                              const UserEvent& event) noexcept {
        return cfg.runtime.condition_id[0] != '\0' &&
               std::strcmp(event.market, cfg.runtime.condition_id) == 0;
    }

private:
    void run_loop() {
        uint32_t backoff_ms = 250;
        while (running_.load(std::memory_order_acquire)) {
            rbuf_len_ = 0;
            connected_.store(false, std::memory_order_release);
            const bool clean = session();
            connected_.store(false, std::memory_order_release);
            if (!running_.load(std::memory_order_acquire)) break;
            reconnects_.fetch_add(1, std::memory_order_relaxed);
            if (clean) backoff_ms = 250;
            const uint32_t slices = std::max(1U, backoff_ms / 10U);
            for (uint32_t i = 0; i < slices && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            backoff_ms = std::min(backoff_ms * 2U, 5000U);
        }
    }

    // Wipes the credential-bearing subscribe payload on every exit path.
    struct SubscriptionGuard {
        char* buffer;
        size_t capacity;
        ~SubscriptionGuard() { secure_zero(buffer, capacity); }
    };

    bool session() {
        const SubscriptionGuard guard{subscription_, sizeof(subscription_)};
        wstransport::Url url;
        if (!user_channel_url(url)) {
            auth_error_.store(true, std::memory_order_release);
            return false;
        }
        size_t subscription_len = 0;
        if (!user_ws::build_subscription(cfg_.owner_api_key, cfg_.api_secret_b64,
                                         cfg_.api_passphrase,
                                         cfg_.runtime.condition_id,
                                         subscription_, sizeof(subscription_),
                                         subscription_len)) {
            auth_error_.store(true, std::memory_order_release);
            return false;
        }

        const int fd = wstransport::tcp_connect(url.host, url.port, 3000);
        if (fd < 0) return false;
        active_fd_.store(fd, std::memory_order_release);
        wstransport::set_socket_options(fd);

        SSL* ssl = nullptr;
        if (url.tls) {
            SSL_CTX* context = wstransport::shared_ssl_context();
            if (!context || !(ssl = SSL_new(context))) {
                cleanup(nullptr, fd);
                return false;
            }
            if (SSL_set_fd(ssl, fd) != 1 ||
                SSL_set_tlsext_host_name(ssl, url.host) != 1 ||
                SSL_set1_host(ssl, url.host) != 1) {
                cleanup(ssl, fd);
                return false;
            }
            if (SSL_connect(ssl) != 1 ||
                SSL_get_verify_result(ssl) != X509_V_OK) {
                cleanup(ssl, fd);
                return false;
            }
            if (cfg_.tls_pin[0] && !pin_matches(ssl)) {
                cleanup(ssl, fd);
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
        const bool default_port = (url.tls && url.port == 443) ||
                                  (!url.tls && url.port == 80);
        const int authority_len = default_port
            ? std::snprintf(authority, sizeof(authority), "%s", url.host)
            : std::snprintf(authority, sizeof(authority), "%s:%d", url.host,
                            url.port);
        char request[768];
        const int request_len = std::snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n", url.path, authority, key_b64);
        if (authority_len <= 0 ||
            static_cast<size_t>(authority_len) >= sizeof(authority) ||
            request_len <= 0 ||
            static_cast<size_t>(request_len) >= sizeof(request) ||
            !wstransport::send_all(ssl, fd, request,
                                   static_cast<size_t>(request_len))) {
            cleanup(ssl, fd);
            return false;
        }

        char accept_expected[32]{};
        wstransport::websocket_accept(key_b64, accept_expected);
        if (!wstransport::read_http_upgrade(ssl, fd, accept_expected)) {
            cleanup(ssl, fd);
            return false;
        }

        // The subscribe payload carries credentials: send it, then wipe it.
        const bool sent = send_text(ssl, fd, subscription_, subscription_len);
        secure_zero(subscription_, sizeof(subscription_));
        subscription_len = 0;
        if (!sent) {
            cleanup(ssl, fd);
            return false;
        }

        connected_.store(true, std::memory_order_release);
        const bool clean = read_frames(ssl, fd);
        cleanup(ssl, fd);
        return clean;
    }

    bool user_channel_url(wstransport::Url& url) const noexcept {
        return derive_user_url(cfg_.user_ws_host, cfg_.ws_host, url);
    }

    bool pin_matches(SSL* ssl) const noexcept {
        X509* certificate = SSL_get1_peer_certificate(ssl);
        if (!certificate) return false;
        uint8_t digest[32];
        unsigned int digest_len = 0;
        const bool hashed = X509_digest(certificate, EVP_sha256(), digest,
                                        &digest_len) == 1 && digest_len == 32;
        X509_free(certificate);
        if (!hashed) return false;
        char encoded[48]{};
        base64_encode(digest, sizeof(digest), encoded);
        // BOT_TLS_PIN accepts one or two sha256// pins separated by ';'.
        const char* cursor = cfg_.tls_pin;
        while (*cursor) {
            const char* separator = std::strchr(cursor, ';');
            const size_t length = separator
                ? static_cast<size_t>(separator - cursor) : std::strlen(cursor);
            if (length == 7 + std::strlen(encoded) &&
                std::strncmp(cursor, "sha256//", 8) == 0 &&
                std::memcmp(cursor + 8, encoded, std::strlen(encoded)) == 0)
                return true;
            if (!separator) break;
            cursor = separator + 1;
        }
        return false;
    }

    bool send_text(SSL* ssl, int fd, const char* payload, size_t len) {
        size_t frame_len = 0;
        if (!wstransport::encode_frame(0x1, payload, len, send_buffer_.data(),
                                       send_buffer_.size(), frame_len))
            return false;
        return wstransport::send_all(ssl, fd, send_buffer_.data(), frame_len);
    }

    bool read_frames(SSL* ssl, int fd) {
        size_t fragment_len = 0;
        bool fragmenting = false;
        uint64_t last_ping = wstransport::now_mono_ms();
        uint64_t last_receive = last_ping;

        while (running_.load(std::memory_order_acquire)) {
            const uint64_t now = wstransport::now_mono_ms();
            if (now - last_ping >= 10000) {
                // AsyncAPI: text PING every 10 seconds, server answers PONG.
                if (!send_text(ssl, fd, "PING", 4)) return false;
                pings_sent_.fetch_add(1, std::memory_order_relaxed);
                last_ping = now;
            }
            if (now - last_receive >= 30000) return false;  // 3 missed heartbeats

            fd_set reads;
            FD_ZERO(&reads);
            FD_SET(fd, &reads);
            timeval timeout{0, 200000};
            const int ready = ::select(fd + 1, &reads, nullptr, nullptr, &timeout);
            if (!running_.load(std::memory_order_acquire)) return true;
            if (ready < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (ready == 0) continue;
            if (rbuf_len_ >= rbuf_.size()) return false;
            const ssize_t n = wstransport::receive(ssl, fd,
                                                   rbuf_.data() + rbuf_len_,
                                                   rbuf_.size() - rbuf_len_);
            if (n <= 0) {
                if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                              errno == EWOULDBLOCK))
                    continue;
                return false;
            }
            rbuf_len_ += static_cast<size_t>(n);
            last_receive = wstransport::now_mono_ms();

            size_t offset = 0;
            while (offset < rbuf_len_) {
                wstransport::Frame frame;
                const wstransport::FrameStatus status = wstransport::decode_frame(
                    rbuf_.data() + offset, rbuf_len_ - offset, frame);
                if (status == wstransport::FrameStatus::kNeedMore) break;
                if (status == wstransport::FrameStatus::kError) return false;
                offset += frame.consumed;

                if (frame.opcode == 0x8) {
                    (void)send_text(ssl, fd, frame.payload,
                                    std::min<size_t>(frame.payload_len, 125));
                    return false;
                }
                if (frame.opcode == 0x9) {  // RFC 6455 control ping
                    size_t pong_len = 0;
                    if (!wstransport::encode_frame(0xA, frame.payload,
                                                   frame.payload_len,
                                                   send_buffer_.data(),
                                                   send_buffer_.size(), pong_len) ||
                        !wstransport::send_all(ssl, fd, send_buffer_.data(),
                                               pong_len))
                        return false;
                    continue;
                }
                if (frame.opcode == 0xA) {
                    pongs_seen_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (frame.opcode == 0x2) continue;  // binary: not part of the API
                if (frame.opcode == 0x1 && frame.final) {
                    if (fragmenting) return false;
                    handle_message(frame.payload, frame.payload_len);
                    continue;
                }
                if (frame.opcode == 0x1 && !frame.final) {
                    if (fragmenting ||
                        frame.payload_len > fragment_.size())
                        return false;
                    std::memcpy(fragment_.data(), frame.payload,
                                frame.payload_len);
                    fragment_len = frame.payload_len;
                    fragmenting = true;
                    continue;
                }
                if (frame.opcode == 0x0) {
                    if (!fragmenting ||
                        frame.payload_len > fragment_.size() - fragment_len)
                        return false;
                    std::memcpy(fragment_.data() + fragment_len, frame.payload,
                                frame.payload_len);
                    fragment_len += frame.payload_len;
                    if (frame.final) {
                        handle_message(fragment_.data(), fragment_len);
                        fragmenting = false;
                        fragment_len = 0;
                    }
                }
            }
            if (offset) {
                std::memmove(rbuf_.data(), rbuf_.data() + offset,
                             rbuf_len_ - offset);
                rbuf_len_ -= offset;
            }
        }
        return true;
    }

    void handle_message(const char* payload, size_t length) {
        if (length == 4 && std::memcmp(payload, "PONG", 4) == 0) {
            pongs_seen_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        UserEvent events[kUserEventQueue > 16 ? 16 : kUserEventQueue];
        const size_t count =
            user_ws::parse_message(payload, length, events, 16);
        if (count == 0) {
            // Not an event: either a documented error payload or something we
            // do not recognize. Both are recorded; credentials are never
            // echoed into the log.
            if (mentions_error(payload, length)) {
                server_errors_.fetch_add(1, std::memory_order_relaxed);
                record_server_error(payload, length);
            } else {
                malformed_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            if (!is_our_market(cfg_, events[i])) {
                foreign_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!queue_.try_push(events[i])) {
                events_dropped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            events_queued_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static bool mentions_error(const char* payload, size_t length) noexcept {
        for (size_t i = 0; i + 5 <= length; ++i) {
            if (strncasecmp(payload + i, "error", 5) == 0) return true;
        }
        return false;
    }

    void record_server_error(const char* payload, size_t length) noexcept {
        const char* key = cfg_.owner_api_key;
        if (key && *key) {
            const size_t key_len = std::strlen(key);
            for (size_t i = 0; i + key_len <= length; ++i) {
                if (std::memcmp(payload + i, key, key_len) == 0) {
                    std::snprintf(last_server_error_,
                                  sizeof(last_server_error_),
                                  "[redacted: server echoed a credential]");
                    return;
                }
            }
        }
        const size_t copied = std::min(length, sizeof(last_server_error_) - 1);
        std::memcpy(last_server_error_, payload, copied);
        last_server_error_[copied] = '\0';
    }

    void cleanup(SSL* ssl, int fd) {
        connected_.store(false, std::memory_order_release);
        active_fd_.store(-1, std::memory_order_release);
        if (ssl) SSL_free(ssl);
        if (fd >= 0) ::close(fd);
    }

    const MarketConfig& cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> auth_error_{false};
    std::atomic<int> active_fd_{-1};
    std::atomic<uint64_t> reconnects_{0};
    std::atomic<uint64_t> events_queued_{0};
    std::atomic<uint64_t> events_dropped_{0};
    std::atomic<uint64_t> pings_sent_{0};
    std::atomic<uint64_t> pongs_seen_{0};
    std::atomic<uint64_t> malformed_{0};
    std::atomic<uint64_t> foreign_{0};
    std::atomic<uint64_t> server_errors_{0};
    SPSC_RingBuffer<UserEvent, kUserEventQueue> queue_;
    std::array<char, wstransport::kWsMaxPayload + 14> rbuf_{};
    size_t rbuf_len_ = 0;
    std::array<char, wstransport::kWsMaxPayload> fragment_{};
    std::array<char, 2048> send_buffer_{};
    char subscription_[768]{};
    char last_server_error_[160]{};
};

#endif  // USER_WS_CLIENT_HPP
