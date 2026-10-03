#ifndef CLOB_REST_CLIENT_HPP
#define CLOB_REST_CLIENT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Cold-path CLOB / Gamma REST client.
//
// Layering (deliberate, so every layer is testable offline):
//   HttpTransport   – dumb executor: method + URL + body + headers → response.
//                     Implementations: CurlHttpTransport (network build) and
//                     FixtureTransport (tests/replay/paper).
//   ClobApiClient   – builds paths and query strings, signs L1/L2 headers,
//                     classifies transport outcomes, parses bodies with the
//                     pure parsers in venue_metadata.hpp / user_event.hpp.
//
// Authentication (verified against the official SDKs and docs):
//   L2 message = <unix_seconds> + <METHOD> + <path> [+ <exact body>]
//   (the path excludes the query string — Polymarket/py-sdk
//    _make_l2_header_resolver and Polymarket/py-clob-client
//    create_level_2_headers both sign the bare path while sending query
//    parameters separately.)
//   signature = urlsafeBase64(HMAC-SHA256(base64url_decode(secret), message))
//   headers: POLY_ADDRESS, POLY_SIGNATURE, POLY_TIMESTAMP, POLY_API_KEY,
//            POLY_PASSPHRASE
//   L1 headers (credential creation/derivation): POLY_ADDRESS, POLY_SIGNATURE
//            (EIP-712 ClobAuth), POLY_TIMESTAMP, POLY_NONCE
//
// Ambiguity is a first-class result.  Anything that is not provably a
// pre-send failure is reported as `ambiguous`, because the venue may already
// have acted on the request.  Callers must then move the affected order to
// UNKNOWN and reconcile instead of retrying.
// ─────────────────────────────────────────────────────────────────────────────

#include <time.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"
#include "../include/json_scan.hpp"
#include "../include/venue_metadata.hpp"

namespace clob {

inline constexpr size_t K_MAX_BODY = 196608;      // 192 KiB bounded response
inline constexpr size_t K_MAX_URL = 768;
inline constexpr size_t K_MAX_PATH = 256;
inline constexpr size_t K_MAX_QUERY = 256;
inline constexpr size_t K_MAX_HEADER = 256;
inline constexpr size_t K_MAX_HEADERS = 8;
inline constexpr size_t K_MAX_PAGE_ITEMS = 64;
inline constexpr size_t K_CURSOR_END = 32;

// Cursor value that terminates CLOB pagination (py-clob-client END_CURSOR).
inline constexpr const char* K_CURSOR_END_VALUE = "LTE=";
inline constexpr const char* K_CURSOR_INITIAL_VALUE = "MA==";

struct HttpResponse {
    long code = 0;
    size_t body_len = 0;
    bool truncated = false;
    bool transport_ok = false;  // a response was received and fully read
    bool ambiguous = false;     // the venue may already have processed it
    uint64_t elapsed_us = 0;
    char error[128]{};
    char* body = nullptr;       // owned by the caller/transport context
};

struct RequestOptions {
    long connect_timeout_ms = 2000;
    long total_timeout_ms = 5000;
    bool allow_redirects = false;
};

// ── Transport interface ─────────────────────────────────────────────────────
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    // Executes one request.  `body` may be null.  Headers are "Key: Value".
    // Implementations must fill HttpResponse (including ambiguity).
    virtual void request(const char* method, const char* url, const char* body,
                         size_t body_len, const char* const* headers,
                         size_t header_count, const RequestOptions& options,
                         char* body_buffer, size_t body_capacity,
                         HttpResponse& out) = 0;
};

// ── Wire records ────────────────────────────────────────────────────────────
struct OrderWire {
    char id[80]{};
    char market[70]{};          // condition id
    char asset_id[80]{};        // token id
    char owner[80]{};
    char maker_address[43]{};
    char outcome[24]{};
    char order_type[8]{};
    char status[24]{};
    uint8_t side = 0;
    uint64_t price_raw = 0;
    uint64_t original_size = 0;
    uint64_t size_matched = 0;
    uint64_t created_at_s = 0;
    uint64_t expiration_s = 0;
    bool valid = false;
};

struct TradeWire {
    char id[80]{};
    char taker_order_id[80]{};
    char market[70]{};
    char asset_id[80]{};
    char outcome[24]{};
    char owner[80]{};
    char trade_owner[80]{};
    char maker_address[43]{};
    char transaction_hash[70]{};
    char status[40]{};
    uint8_t side = 0;
    uint64_t size_raw = 0;
    uint64_t price_raw = 0;
    uint64_t fee_rate_bps = 0;
    uint64_t match_time_s = 0;
    uint64_t last_update_s = 0;
    bool valid = false;
};

struct OrderPage {
    OrderWire items[K_MAX_PAGE_ITEMS]{};
    size_t count = 0;
    char next_cursor[K_CURSOR_END]{};
    bool has_more = false;
    bool complete = false;  // every item parsed; false → fail closed
};

struct TradePage {
    TradeWire items[K_MAX_PAGE_ITEMS]{};
    size_t count = 0;
    char next_cursor[K_CURSOR_END]{};
    bool has_more = false;
    bool complete = false;
};

struct OrderPostResult {
    bool success = false;
    char order_id[80]{};
    char status[24]{};
    char error_msg[192]{};
    uint64_t making_amount = 0;  // base units (pUSD for BUY, tokens for SELL)
    uint64_t taking_amount = 0;
    char trade_ids[8][80]{};
    size_t trade_id_count = 0;
    bool parsed = false;
};

struct HeartbeatResult {
    bool accepted = false;
    bool rejected_invalid_id = false;  // 400 + expected id supplied
    char heartbeat_id[80]{};           // new id on success, expected id on 400
    char error_msg[128]{};
    bool parsed = false;
};

struct CallResult {
    HttpResponse response{};
    bool ok = false;          // transport ok, HTTP 2xx, body parsed
    bool http_ok = false;
    bool ambiguous = false;
    char detail[128]{};
};

// ── Credentials ─────────────────────────────────────────────────────────────
struct Credentials {
    char address[43]{};         // POLY_ADDRESS (signer EOA)
    char api_key[96]{};
    char api_secret_b64[128]{};
    char api_passphrase[160]{};
    uint8_t signature_type = 0;
    bool complete() const noexcept {
        return address[0] && api_key[0] && api_secret_b64[0] && api_passphrase[0];
    }
};

// ── Parsers (pure) ──────────────────────────────────────────────────────────
inline bool parse_side(const char* text, uint8_t& side) noexcept {
    if (!text) return false;
    if (std::strcmp(text, "BUY") == 0 || std::strcmp(text, "buy") == 0 ||
        std::strcmp(text, "B") == 0) { side = 0; return true; }
    if (std::strcmp(text, "SELL") == 0 || std::strcmp(text, "sell") == 0 ||
        std::strcmp(text, "S") == 0) { side = 1; return true; }
    return false;
}

// Parses one order object (GET /data/order/{id}, GET /data/orders items, and
// the user-WebSocket order payload share this shape).
inline bool parse_order_wire(const char* data, size_t len, OrderWire& out) noexcept {
    out = OrderWire{};
    if (!data || len == 0) return false;
    if (!json_scan::get_string(data, len, "id", out.id, sizeof(out.id))) return false;
    if (!out.id[0]) return false;
    (void)json_scan::get_string(data, len, "market", out.market, sizeof(out.market));
    if (!json_scan::get_string(data, len, "asset_id", out.asset_id,
                               sizeof(out.asset_id)))
        (void)json_scan::get_string(data, len, "token_id", out.asset_id,
                                    sizeof(out.asset_id));
    (void)json_scan::get_string(data, len, "owner", out.owner, sizeof(out.owner));
    (void)json_scan::get_string(data, len, "maker_address", out.maker_address,
                                sizeof(out.maker_address));
    (void)json_scan::get_string(data, len, "outcome", out.outcome, sizeof(out.outcome));
    (void)json_scan::get_string(data, len, "order_type", out.order_type,
                                sizeof(out.order_type));
    if (!json_scan::get_string(data, len, "status", out.status, sizeof(out.status)))
        return false;
    char side[8]{};
    if (!json_scan::get_string(data, len, "side", side, sizeof(side))) return false;
    if (!parse_side(side, out.side)) return false;
    if (!json_scan::get_fixed(data, len, "price", 1000000, out.price_raw)) return false;
    if (!json_scan::get_fixed(data, len, "original_size", 1000000, out.original_size))
        return false;
    // size_matched is optional on some payloads; absence means zero matched.
    if (!json_scan::get_fixed(data, len, "size_matched", 1000000, out.size_matched))
        out.size_matched = 0;
    if (out.size_matched > out.original_size) return false;
    (void)json_scan::get_u64(data, len, "created_at", out.created_at_s);
    if (!json_scan::get_u64(data, len, "expiration", out.expiration_s)) {
        char text[24]{};
        if (json_scan::get_string(data, len, "expiration", text, sizeof(text)))
            (void)json_scan::parse_fixed(text, std::strlen(text), 1, out.expiration_s);
    }
    if (!json_scan::is_decimal_integer(out.asset_id, std::strlen(out.asset_id)))
        return false;
    out.valid = true;
    return true;
}

inline bool parse_trade_wire(const char* data, size_t len, TradeWire& out) noexcept {
    out = TradeWire{};
    if (!data || len == 0) return false;
    if (!json_scan::get_string(data, len, "id", out.id, sizeof(out.id))) return false;
    if (!out.id[0]) return false;
    (void)json_scan::get_string(data, len, "taker_order_id", out.taker_order_id,
                                sizeof(out.taker_order_id));
    (void)json_scan::get_string(data, len, "market", out.market, sizeof(out.market));
    if (!json_scan::get_string(data, len, "asset_id", out.asset_id,
                               sizeof(out.asset_id)))
        (void)json_scan::get_string(data, len, "token_id", out.asset_id,
                                    sizeof(out.asset_id));
    (void)json_scan::get_string(data, len, "outcome", out.outcome, sizeof(out.outcome));
    (void)json_scan::get_string(data, len, "owner", out.owner, sizeof(out.owner));
    (void)json_scan::get_string(data, len, "trade_owner", out.trade_owner,
                                sizeof(out.trade_owner));
    (void)json_scan::get_string(data, len, "maker_address", out.maker_address,
                                sizeof(out.maker_address));
    (void)json_scan::get_string(data, len, "transaction_hash", out.transaction_hash,
                                sizeof(out.transaction_hash));
    if (!json_scan::get_string(data, len, "status", out.status, sizeof(out.status)))
        return false;
    char side[8]{};
    if (json_scan::get_string(data, len, "side", side, sizeof(side))) {
        if (!parse_side(side, out.side)) return false;
    } else {
        return false;
    }
    if (!json_scan::get_fixed(data, len, "size", 1000000, out.size_raw)) return false;
    if (!json_scan::get_fixed(data, len, "price", 1000000, out.price_raw)) return false;
    (void)json_scan::get_u64(data, len, "fee_rate_bps", out.fee_rate_bps);
    (void)json_scan::get_u64(data, len, "match_time", out.match_time_s);
    (void)json_scan::get_u64(data, len, "last_update", out.last_update_s);
    if (!json_scan::is_decimal_integer(out.asset_id, std::strlen(out.asset_id)))
        return false;
    out.valid = true;
    return true;
}

inline bool parse_order_page(const char* data, size_t len, OrderPage& out) noexcept {
    out = OrderPage{};
    if (!data || len == 0) return false;
    size_t start = 0;
    size_t end = 0;
    if (!json_scan::find_array(data, len, "data", start, end)) return false;
    const size_t total = json_scan::array_count(data, len, start, end);
    if (total > K_MAX_PAGE_ITEMS) { out.complete = false; return false; }
    for (size_t i = 0; i < total; ++i) {
        size_t es = 0;
        size_t ee = 0;
        if (!json_scan::array_element(data, len, start, end, i, es, ee)) return false;
        if (!parse_order_wire(data + es, ee - es, out.items[out.count])) return false;
        ++out.count;
    }
    char cursor[K_CURSOR_END]{};
    if (json_scan::get_string(data, len, "next_cursor", cursor, sizeof(cursor))) {
        std::snprintf(out.next_cursor, sizeof(out.next_cursor), "%s", cursor);
        out.has_more = std::strcmp(cursor, K_CURSOR_END_VALUE) != 0 && cursor[0] != '\0';
    }
    out.complete = true;
    return true;
}

inline bool parse_trade_page(const char* data, size_t len, TradePage& out) noexcept {
    out = TradePage{};
    if (!data || len == 0) return false;
    size_t start = 0;
    size_t end = 0;
    if (!json_scan::find_array(data, len, "data", start, end)) return false;
    const size_t total = json_scan::array_count(data, len, start, end);
    if (total > K_MAX_PAGE_ITEMS) { out.complete = false; return false; }
    for (size_t i = 0; i < total; ++i) {
        size_t es = 0;
        size_t ee = 0;
        if (!json_scan::array_element(data, len, start, end, i, es, ee)) return false;
        if (!parse_trade_wire(data + es, ee - es, out.items[out.count])) return false;
        ++out.count;
    }
    char cursor[K_CURSOR_END]{};
    if (json_scan::get_string(data, len, "next_cursor", cursor, sizeof(cursor))) {
        std::snprintf(out.next_cursor, sizeof(out.next_cursor), "%s", cursor);
        out.has_more = std::strcmp(cursor, K_CURSOR_END_VALUE) != 0 && cursor[0] != '\0';
    }
    out.complete = true;
    return true;
}

inline bool parse_order_post(const char* data, size_t len,
                             OrderPostResult& out) noexcept {
    out = OrderPostResult{};
    if (!data || len == 0) return false;
    for (const char* key : {"success", "orderID", "status", "errorMsg"})
        if (json_scan::count_key(data, len, key) > 1) return false;
    if (!json_scan::get_bool(data, len, "success", out.success)) return false;
    (void)json_scan::get_string(data, len, "orderID", out.order_id,
                                sizeof(out.order_id));
    (void)json_scan::get_string(data, len, "status", out.status, sizeof(out.status));
    (void)json_scan::get_string(data, len, "errorMsg", out.error_msg,
                                sizeof(out.error_msg));
    if (!out.error_msg[0])
        (void)json_scan::get_string(data, len, "error", out.error_msg,
                                    sizeof(out.error_msg));
    // makingAmount/takingAmount are decimal strings in base units.
    (void)json_scan::get_fixed(data, len, "makingAmount", 1, out.making_amount);
    (void)json_scan::get_fixed(data, len, "takingAmount", 1, out.taking_amount);
    size_t start = 0;
    size_t end = 0;
    if (json_scan::find_array(data, len, "tradeIDs", start, end)) {
        const size_t total = json_scan::array_count(data, len, start, end);
        for (size_t i = 0; i < total && out.trade_id_count < 8; ++i) {
            size_t es = 0;
            size_t ee = 0;
            if (!json_scan::array_element(data, len, start, end, i, es, ee)) break;
            if (ee - es < 2 || data[es] != '"') break;
            const size_t id_len = ee - es - 2;
            if (id_len == 0 || id_len >= sizeof(out.trade_ids[0])) break;
            std::memcpy(out.trade_ids[out.trade_id_count], data + es + 1, id_len);
            out.trade_ids[out.trade_id_count][id_len] = '\0';
            ++out.trade_id_count;
        }
    }
    out.parsed = true;
    return true;
}

inline bool parse_heartbeat(const char* data, size_t len, long http_code,
                            HeartbeatResult& out) noexcept {
    out = HeartbeatResult{};
    if (!data || len == 0) return false;
    char id[80]{};
    if (json_scan::get_string(data, len, "heartbeat_id", id, sizeof(id))) {
        std::snprintf(out.heartbeat_id, sizeof(out.heartbeat_id), "%s", id);
        if (http_code == 400) {
            char msg[128]{};
            if (json_scan::get_string(data, len, "error_msg", msg, sizeof(msg)) &&
                std::strstr(msg, "Invalid Heartbeat ID") != nullptr) {
                out.rejected_invalid_id = true;
                std::snprintf(out.error_msg, sizeof(out.error_msg), "%s", msg);
                out.parsed = true;
                return true;
            }
        }
        out.accepted = http_code >= 200 && http_code < 300;
        out.parsed = true;
        return true;
    }
    // Some deployments acknowledge with {"status":"ok"} and no new id; the
    // caller then keeps the previous id and records the acknowledgement.
    char status[24]{};
    if (json_scan::get_string(data, len, "status", status, sizeof(status)) &&
        std::strcmp(status, "ok") == 0) {
        out.accepted = http_code >= 200 && http_code < 300;
        out.parsed = true;
        return true;
    }
    return false;
}

// ── The client ──────────────────────────────────────────────────────────────
class ClobApiClient {
public:
    ClobApiClient(HttpTransport& transport, const Credentials& credentials,
                  const char* clob_host, const char* gamma_host)
        : transport_(transport), credentials_(credentials) {
        std::snprintf(clob_host_, sizeof(clob_host_), "%s",
                      clob_host && *clob_host ? clob_host : venue::K_DEFAULT_CLOB_HOST);
        std::snprintf(gamma_host_, sizeof(gamma_host_), "%s",
                      gamma_host && *gamma_host ? gamma_host
                                                : venue::K_DEFAULT_GAMMA_HOST);
        const size_t encoded_len = std::strlen(credentials_.api_secret_b64);
        secret_len_ = base64url_decode(credentials_.api_secret_b64, encoded_len,
                                       secret_raw_, sizeof(secret_raw_));
        if (secret_len_ && secret_len_ != SIZE_MAX) hmac_.set_key(secret_raw_, secret_len_);
    }

    ~ClobApiClient() {
        secure_zero(secret_raw_, sizeof(secret_raw_));
        secure_zero(&hmac_, sizeof(hmac_));
        secure_zero(&credentials_, sizeof(credentials_));
    }

    ClobApiClient(const ClobApiClient&) = delete;
    ClobApiClient& operator=(const ClobApiClient&) = delete;

    // Transport accessor: preflight needs it for the L1 credential probe, which
    // uses headers the L2 signer does not produce.
    HttpTransport& transport() noexcept { return transport_; }

    bool credentials_ready() const noexcept {
        return credentials_.complete() && secret_len_ != 0 && secret_len_ != SIZE_MAX;
    }
    const char* clob_host() const noexcept { return clob_host_; }
    const char* gamma_host() const noexcept { return gamma_host_; }
    uint8_t signature_type() const noexcept { return credentials_.signature_type; }
    const char* address() const noexcept { return credentials_.address; }

    // Exchange that must be approved for this order (spender), per py-sdk
    // resolve_order_exchange_address.
    void exchange_for_token(const char* token_id, bool neg_risk,
                            uint8_t out[20], char out_hex[43]) const noexcept {
        const bool v2 = venue::is_protocol_v2_position_id(token_id);
        const uint8_t* source = v2 ? venue::K_EXCHANGE_V3
                                   : (neg_risk ? venue::K_EXCHANGE_NEG_RISK
                                               : venue::K_EXCHANGE_STANDARD);
        std::memcpy(out, source, 20);
        bytes_to_hex(source, 20, out_hex, 43);
    }

    // ── Public endpoints ────────────────────────────────────────────────────
    bool health(CallResult& result, const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s", clob_host_, venue::K_PATH_HEALTH);
        call("GET", url, nullptr, 0, false, options, result);
        return result.ok && venue::parse_health(result.response.body,
                                                result.response.body_len);
    }

    bool server_time(uint64_t& seconds, CallResult& result,
                     const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s", clob_host_, venue::K_PATH_TIME);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        return venue::parse_server_time(result.response.body,
                                        result.response.body_len, seconds);
    }

    bool book(const char* token_id, venue::BookSnapshot& snapshot, CallResult& result,
              const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s?token_id=%s", clob_host_,
                      venue::K_PATH_BOOK, token_id);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        return venue::parse_book(result.response.body, result.response.body_len,
                                 snapshot);
    }

    bool tick_size(const char* token_id, uint64_t& tick_raw, CallResult& result,
                   const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s?token_id=%s", clob_host_,
                      venue::K_PATH_TICK_SIZE, token_id);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        return venue::parse_tick_size(result.response.body, result.response.body_len,
                                      tick_raw);
    }

    bool neg_risk(const char* token_id, bool& value, CallResult& result,
                  const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s?token_id=%s", clob_host_,
                      venue::K_PATH_NEG_RISK, token_id);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        return venue::parse_neg_risk(result.response.body, result.response.body_len,
                                     value);
    }

    bool fee_rate_bps(const char* token_id, uint64_t& bps, CallResult& result,
                      const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s?token_id=%s", clob_host_,
                      venue::K_PATH_FEE_RATE, token_id);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        return venue::parse_fee_rate_bps(result.response.body,
                                         result.response.body_len, bps);
    }

    bool clob_market(const char* condition_id, venue::MarketMetadata& metadata,
                     CallResult& result, const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s%s", clob_host_,
                      venue::K_PATH_CLOB_MARKETS, condition_id);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        return venue::parse_clob_market(result.response.body,
                                        result.response.body_len, metadata);
    }

    bool market_by_token(const char* token_id, char* condition_id, size_t cap,
                         CallResult& result, const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s%s", clob_host_,
                      venue::K_PATH_MARKETS_BY_TOKEN, token_id);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        const char* data = result.response.body;
        const size_t len = result.response.body_len;
        // The response is either a market object or a list of them.
        size_t scan_len = len;
        size_t start = 0;
        size_t end = 0;
        if (json_scan::find_array(data, scan_len, "data", start, end)) {
            size_t es = 0;
            size_t ee = 0;
            if (!json_scan::array_element(data, scan_len, start, end, 0, es, ee))
                return false;
            data += es;
            scan_len = ee - es;
        }
        if (!json_scan::get_string(data, scan_len, "condition_id", condition_id, cap))
            return false;
        return json_scan::is_hex_bytes(condition_id, std::strlen(condition_id), 32);
    }

    bool gamma_market_by_slug(const char* slug, venue::MarketMetadata& metadata,
                              CallResult& result, const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s/markets/slug/%s", gamma_host_, slug);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        return venue::parse_gamma_market(result.response.body,
                                         result.response.body_len, metadata);
    }

    bool gamma_market_by_condition(const char* condition_id,
                                   venue::MarketMetadata& metadata,
                                   CallResult& result,
                                   const RequestOptions& options = {}) {
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s/markets?condition_ids=%s&limit=1",
                      gamma_host_, condition_id);
        call("GET", url, nullptr, 0, false, options, result);
        if (!result.ok) return false;
        const char* data = result.response.body;
        const size_t len = result.response.body_len;
        size_t begin = 0;
        while (begin < len && json_scan::is_space(data[begin])) ++begin;
        if (begin < len && data[begin] == '[') {
            const size_t after = json_scan::skip_value(data, len, begin);
            if (!after) return false;
            size_t es = 0;
            size_t ee = 0;
            if (!json_scan::array_element(data, len, begin, after, 0, es, ee))
                return false;  // empty or malformed list
            return venue::parse_gamma_market(data + es, ee - es, metadata);
        }
        return venue::parse_gamma_market(data + begin, len - begin, metadata);
    }

    // ── L2 endpoints ────────────────────────────────────────────────────────
    bool order(const char* order_id, OrderWire& wire, CallResult& result,
               const RequestOptions& options = {}) {
        if (!credentials_ready()) return false;
        char path[K_MAX_PATH];
        std::snprintf(path, sizeof(path), "%s%s", venue::K_PATH_ORDER, order_id);
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s", clob_host_, path);
        call("GET", url, nullptr, 0, true, options, result, path);
        if (!result.ok) return false;
        return parse_order_wire(result.response.body, result.response.body_len, wire);
    }

    bool open_orders(const char* market, const char* asset_id, const char* cursor,
                     OrderPage& page, CallResult& result,
                     const RequestOptions& options = {}) {
        if (!credentials_ready()) return false;
        char path[K_MAX_PATH];
        char query[K_MAX_QUERY];
        build_query(query, sizeof(query), market, asset_id, cursor);
        std::snprintf(path, sizeof(path), "%s", venue::K_PATH_ORDERS);
        char url[K_MAX_URL];
        if (query[0])
            std::snprintf(url, sizeof(url), "%s%s?%s", clob_host_, path, query);
        else
            std::snprintf(url, sizeof(url), "%s%s", clob_host_, path);
        call("GET", url, nullptr, 0, true, options, result, path);
        if (!result.ok) return false;
        return parse_order_page(result.response.body, result.response.body_len, page);
    }

    bool trades(const char* market, const char* asset_id, const char* cursor,
                TradePage& page, CallResult& result,
                const RequestOptions& options = {}) {
        if (!credentials_ready()) return false;
        char path[K_MAX_PATH];
        char query[K_MAX_QUERY];
        build_query(query, sizeof(query), market, asset_id, cursor);
        std::snprintf(path, sizeof(path), "%s", venue::K_PATH_TRADES);
        char url[K_MAX_URL];
        if (query[0])
            std::snprintf(url, sizeof(url), "%s%s?%s", clob_host_, path, query);
        else
            std::snprintf(url, sizeof(url), "%s%s", clob_host_, path);
        call("GET", url, nullptr, 0, true, options, result, path);
        if (!result.ok) return false;
        return parse_trade_page(result.response.body, result.response.body_len, page);
    }

    bool balance_allowance(venue::AssetType asset_type, const char* token_id,
                           const char* spender, venue::BalanceAllowance& out,
                           CallResult& result, const RequestOptions& options = {}) {
        if (!credentials_ready()) return false;
        char path[K_MAX_PATH];
        std::snprintf(path, sizeof(path), "%s", venue::K_PATH_BALANCE_ALLOWANCE);
        char url[K_MAX_URL];
        if (token_id && *token_id)
            std::snprintf(url, sizeof(url),
                          "%s%s?asset_type=%s&signature_type=%u&token_id=%s",
                          clob_host_, path, venue::asset_type_name(asset_type),
                          static_cast<unsigned>(credentials_.signature_type), token_id);
        else
            std::snprintf(url, sizeof(url), "%s%s?asset_type=%s&signature_type=%u",
                          clob_host_, path, venue::asset_type_name(asset_type),
                          static_cast<unsigned>(credentials_.signature_type));
        call("GET", url, nullptr, 0, true, options, result, path);
        if (!result.ok) return false;
        return venue::parse_balance_allowance(result.response.body,
                                              result.response.body_len, spender, out);
    }

    bool cancel_all(CallResult& result, const RequestOptions& options = {}) {
        if (!credentials_ready()) return false;
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s", clob_host_, venue::K_PATH_CANCEL_ALL);
        call("DELETE", url, nullptr, 0, true, options, result,
             venue::K_PATH_CANCEL_ALL);
        return result.ok;
    }

    // DELETE /cancel-market-orders with {"market":<condition id>,"asset_id":<token>}
    // (Polymarket/py-clob-client endpoints.py:24 and client.py:729-747; the SDK
    // serialises the body with compact separators, so the bytes below are the same
    // ones its L2 signature covers).  Scope is narrower than cancel_all and wider
    // than cancel_order: every resting order of one market under these
    // credentials.  Cancels are exposure-reducing, so - like the other two cancel
    // entry points - this is deliberately not behind the cold-egress latch; an
    // ambiguous result (transport timeout) is reported as ambiguous and the caller
    // must fall back to reconciliation instead of assuming the cancel landed.
    bool cancel_market_orders(const char* market, const char* asset_id,
                              CallResult& result,
                              const RequestOptions& options = {}) {
        if (!credentials_ready()) return false;
        char body[256];
        const int written = std::snprintf(body, sizeof(body),
                                          "{\"market\":\"%s\",\"asset_id\":\"%s\"}",
                                          market ? market : "",
                                          asset_id ? asset_id : "");
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(body)) return false;
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s", clob_host_,
                      venue::K_PATH_CANCEL_MARKET);
        call("DELETE", url, body, static_cast<size_t>(written), true, options, result,
             venue::K_PATH_CANCEL_MARKET);
        return result.ok;
    }

    bool cancel_order(const char* order_id, CallResult& result,
                      const RequestOptions& options = {}) {
        if (!credentials_ready()) return false;
        char body[160];
        const int written = std::snprintf(body, sizeof(body), "{\"orderID\":\"%s\"}",
                                          order_id);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(body)) return false;
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s", clob_host_, venue::K_PATH_POST_ORDER);
        call("DELETE", url, body, static_cast<size_t>(written), true, options, result,
             venue::K_PATH_POST_ORDER);
        return result.ok;
    }

    // ── Order heartbeat ─────────────────────────────────────────────────────
    // Path: POST /v1/heartbeats  (Polymarket/py-clob-client endpoints.py:48 and
    // Polymarket/clob-client src/endpoints.ts:81).  The official OpenAPI page
    // (docs api-reference/trade/send-heartbeat) documents POST /heartbeats with
    // {"status":"ok"} instead; both are accepted here and the path that actually
    // answers is remembered, because guessing wrong would silently disable the
    // cancel-on-disconnect protection.  [NO VERIFICADO] which one production
    // serves today: the first canary heartbeat settles it and the choice is
    // logged and persisted in the ledger.
    //
    // Chain-start body: the narrative documentation says to send an empty
    // heartbeat_id (`{"heartbeat_id":""}`), while both official SDKs send JSON
    // null (`{"heartbeat_id":null}` — clob-client src/client.ts:1154
    // `heartbeatId ?? null`; py-clob-client client.py:719 with
    // `heartbeat_id=None`).  The documented form is tried first and the SDK form
    // is used as an automatic fallback, so neither source has to be right for
    // the contract to be established.  [NO VERIFICADO] which form production
    // currently accepts.
    bool post_heartbeat(const char* current_id, HeartbeatResult& out,
                        CallResult& result, const RequestOptions& options = {}) {
        if (!credentials_ready()) return false;
        const bool chain_start = !current_id || !current_id[0];
        // Documented spellings, in preference order: the SDK path first (both
        // official SDKs agree on it), the OpenAPI path as a single fallback.
        const char* candidate_paths[2] = {
            heartbeat_path_[0] ? heartbeat_path_ : venue::K_PATH_HEARTBEAT,
            venue::K_PATH_HEARTBEAT_OPENAPI};
        size_t path_attempts = candidate_paths[0] == candidate_paths[1] ? 1 : 2;
        for (size_t path_index = 0; path_index < path_attempts; ++path_index) {
            const char* path = candidate_paths[path_index];
            // Chain-start body forms: documented empty string first, the SDK's
            // JSON null second.  A chained beat has exactly one form.
            const size_t forms = chain_start ? 2 : 1;
            for (size_t form = 0; form < forms; ++form) {
                char body[160];
                const bool null_form = chain_start && form == 1;
                const int written =
                    null_form
                        ? std::snprintf(body, sizeof(body), "{\"heartbeat_id\":null}")
                        : std::snprintf(body, sizeof(body),
                                        "{\"heartbeat_id\":\"%s\"}",
                                        current_id ? current_id : "");
                if (written <= 0 || static_cast<size_t>(written) >= sizeof(body))
                    return false;
                char url[K_MAX_URL];
                std::snprintf(url, sizeof(url), "%s%s", clob_host_, path);
                call("POST", url, body, static_cast<size_t>(written), true, options,
                     result, path);
                if (!result.response.transport_ok) return false;  // never guess
                const long code = result.response.code;
                if (code == 404) break;  // wrong path: try the other spelling
                if (!parse_heartbeat(result.response.body, result.response.body_len,
                                     code, out)) {
                    // A 4xx we cannot interpret may be the other chain-start form.
                    if (chain_start && form == 0 && code >= 400 && code < 500) continue;
                    return false;
                }
                if (out.accepted || out.rejected_invalid_id) {
                    std::snprintf(heartbeat_path_, sizeof(heartbeat_path_), "%s", path);
                    heartbeat_null_start_ = null_form;
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    const char* heartbeat_path() const noexcept {
        return heartbeat_path_[0] ? heartbeat_path_ : venue::K_PATH_HEARTBEAT;
    }
    bool heartbeat_uses_null_start() const noexcept { return heartbeat_null_start_; }

    // ── Cold-egress latch ───────────────────────────────────────────────────
    // post_raw()/post_order() can put an order on the wire WITHOUT the gateway,
    // the ledger observer, the pre-egress collateral reservation or the trading
    // gate: they are the raw REST surface, kept for tests and reserved as the
    // extension point if the live client is ever migrated onto this transport.
    // Production order egress goes through OrderGateway::submit() only.  So that
    // nobody can open this second door by accident, it is closed by default and
    // requires an explicit, reviewed arm_cold_egress(); a refused attempt is
    // counted, logged and reported as unambiguous (nothing reached the wire).
    void arm_cold_egress() noexcept {
        cold_egress_armed_.store(true, std::memory_order_release);
    }
    bool cold_egress_armed() const noexcept {
        return cold_egress_armed_.load(std::memory_order_acquire);
    }
    uint64_t cold_egress_refusals() const noexcept {
        return cold_egress_refusals_.load(std::memory_order_acquire);
    }

    // Raw authenticated POST.  Cancels and every read-only endpoint are NOT
    // behind the latch: refusing them would remove the safe direction.
    bool post_raw(const char* path, const char* body, size_t body_len,
                  CallResult& result, const RequestOptions& options = {}) {
        if (!cold_egress_armed()) {
            cold_egress_refusals_.fetch_add(1, std::memory_order_release);
            result = CallResult{};
            std::snprintf(result.detail, sizeof(result.detail),
                          "REFUSED: cold-egress latch closed for raw POST %s; "
                          "order egress must go through OrderGateway",
                          path ? path : "(null)");
            std::fprintf(stderr, "%s\n", result.detail);
            return false;
        }
        if (!credentials_ready()) return false;
        char url[K_MAX_URL];
        std::snprintf(url, sizeof(url), "%s%s", clob_host_, path);
        call("POST", url, body, body_len, true, options, result, path);
        return result.ok;
    }

    bool post_order(const char* body, size_t body_len, OrderPostResult& out,
                    CallResult& result, const RequestOptions& options = {}) {
        if (!post_raw(venue::K_PATH_POST_ORDER, body, body_len, result, options))
            return false;
        return parse_order_post(result.response.body, result.response.body_len, out);
    }

    // ── Helpers exposed for tests ───────────────────────────────────────────
    size_t build_l2_headers(char headers[K_MAX_HEADERS][K_MAX_HEADER],
                            const char* method, const char* path, const char* body,
                            size_t body_len) {
        if (!credentials_ready()) return 0;
        const uint64_t timestamp = now_unix_seconds();
        char timestamp_text[24];
        std::snprintf(timestamp_text, sizeof(timestamp_text), "%llu",
                      static_cast<unsigned long long>(timestamp));
        char message[K_MAX_PATH + K_MAX_BODY + 64];
        size_t message_len = 0;
        const size_t timestamp_len = std::strlen(timestamp_text);
        if (timestamp_len + std::strlen(method) + std::strlen(path) + body_len + 1 >
            sizeof(message))
            return 0;
        std::memcpy(message + message_len, timestamp_text, timestamp_len);
        message_len += timestamp_len;
        const size_t method_len = std::strlen(method);
        std::memcpy(message + message_len, method, method_len);
        message_len += method_len;
        const size_t path_len = std::strlen(path);
        std::memcpy(message + message_len, path, path_len);
        message_len += path_len;
        if (body && body_len) {
            std::memcpy(message + message_len, body, body_len);
            message_len += body_len;
        }
        uint8_t digest[32];
        hmac_.compute(reinterpret_cast<const uint8_t*>(message), message_len, digest);
        secure_zero(message, message_len);
        char signature[64];
        const size_t signature_len = base64url_encode(digest, 32, signature);
        signature[signature_len] = '\0';
        secure_zero(digest, sizeof(digest));

        size_t count = 0;
        std::snprintf(headers[count++], K_MAX_HEADER, "POLY_ADDRESS: %s",
                      credentials_.address);
        std::snprintf(headers[count++], K_MAX_HEADER, "POLY_SIGNATURE: %s", signature);
        std::snprintf(headers[count++], K_MAX_HEADER, "POLY_TIMESTAMP: %s",
                      timestamp_text);
        std::snprintf(headers[count++], K_MAX_HEADER, "POLY_API_KEY: %s",
                      credentials_.api_key);
        std::snprintf(headers[count++], K_MAX_HEADER, "POLY_PASSPHRASE: %s",
                      credentials_.api_passphrase);
        secure_zero(signature, sizeof(signature));
        return count;
    }

    static uint64_t now_unix_seconds() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec);
    }

    static void bytes_to_hex(const uint8_t* bytes, size_t len, char* out,
                             size_t cap) noexcept {
        if (cap < len * 2 + 3) { if (cap) out[0] = '\0'; return; }
        static const char* digits = "0123456789abcdef";
        out[0] = '0';
        out[1] = 'x';
        for (size_t i = 0; i < len; ++i) {
            out[2 + i * 2] = digits[bytes[i] >> 4];
            out[3 + i * 2] = digits[bytes[i] & 0x0FU];
        }
        out[2 + len * 2] = '\0';
    }

private:
    static void build_query(char* out, size_t cap, const char* market,
                            const char* asset_id, const char* cursor) noexcept {
        size_t written = 0;
        out[0] = '\0';
        auto append = [&](const char* key, const char* value) {
            if (!value || !*value) return;
            const int n = std::snprintf(out + written, cap - written, "%s%s=%s",
                                        written ? "&" : "", key, value);
            if (n < 0 || static_cast<size_t>(n) >= cap - written) return;
            written += static_cast<size_t>(n);
        };
        append("market", market);
        append("asset_id", asset_id);
        append("next_cursor", cursor);
    }

    void call(const char* method, const char* url, const char* body, size_t body_len,
              bool authenticate, const RequestOptions& options, CallResult& result,
              const char* sign_path = nullptr) {
        result = CallResult{};
        result.response.body = body_buffer_;
        const char* headers[K_MAX_HEADERS] = {nullptr};
        char header_storage[K_MAX_HEADERS][K_MAX_HEADER];
        char derived_path[K_MAX_PATH];
        size_t header_count = 0;
        if (authenticate) {
            const char* effective_path = sign_path;
            if (!effective_path) {
                if (!path_of(url, derived_path, sizeof(derived_path))) {
                    std::snprintf(result.detail, sizeof(result.detail),
                                  "url too long to sign");
                    return;
                }
                effective_path = derived_path;
            }
            header_count = build_l2_headers(header_storage, method, effective_path,
                                            body, body_len);
            if (header_count == 0) {
                std::snprintf(result.detail, sizeof(result.detail),
                              "l2 credentials unavailable");
                return;
            }
        }
        for (size_t i = 0; i < header_count; ++i) headers[i] = header_storage[i];
        transport_.request(method, url, body, body_len, headers, header_count, options,
                           body_buffer_, sizeof(body_buffer_), result.response);
        result.ambiguous = result.response.ambiguous;
        result.http_ok = result.response.transport_ok &&
                         result.response.code >= 200 && result.response.code < 300;
        result.ok = result.http_ok && !result.response.truncated &&
                    result.response.body_len != 0;
        if (!result.response.transport_ok && result.detail[0] == '\0')
            std::snprintf(result.detail, sizeof(result.detail), "%s",
                          result.response.error);
        if (result.response.truncated)
            std::snprintf(result.detail, sizeof(result.detail),
                          "response exceeded bounded buffer");
    }

    // Extracts the path component of an absolute URL, without the query string
    // (the L2 signature covers the bare path).
    static size_t path_of(const char* url, char* out, size_t cap) noexcept {
        if (!out || cap < 2) return 0;
        const char* scheme = std::strstr(url, "://");
        const char* start = scheme ? std::strchr(scheme + 3, '/') : nullptr;
        if (!start) start = "/";
        const char* end = std::strchr(start, '?');
        const size_t length = end ? static_cast<size_t>(end - start)
                                  : std::strlen(start);
        if (length + 1 > cap) return 0;
        std::memcpy(out, start, length);
        out[length] = '\0';
        return length;
    }

    HttpTransport& transport_;
    Credentials credentials_;
    char clob_host_[128]{};
    char gamma_host_[128]{};
    uint8_t secret_raw_[96]{};
    size_t secret_len_ = 0;
    HmacSha256 hmac_{};
    char body_buffer_[K_MAX_BODY]{};
    char heartbeat_path_[64]{};
    bool heartbeat_null_start_ = false;
    std::atomic<bool> cold_egress_armed_{false};
    std::atomic<uint64_t> cold_egress_refusals_{0};
};

}  // namespace clob

#endif  // CLOB_REST_CLIENT_HPP
