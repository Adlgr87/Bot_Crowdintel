#ifndef VENUE_METADATA_HPP
#define VENUE_METADATA_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic venue metadata: endpoints, contracts, parsers and validation.
//
// Nothing the venue can change is hardcoded as a trading parameter.  Tick size,
// minimum order size, negative-risk flag, fee schedule, market status and the
// token/condition identity are all fetched, parsed, cross-checked and then
// published atomically to the hot path.  Any missing, malformed, unsupported or
// mutually inconsistent value fails closed.
//
// Verified sources (checked 2026-10-03 against the official documentation and
// the official SDKs — Polymarket/py-sdk @ b543c9db0c896a3727619ea174db7971f03b5b6a
// and Polymarket/clob-client @ 7df8257dc95f99edb257b53a7873e273a9b4a9b3,
// Polymarket/py-clob-client @ b076b04d61135657e25dccc1bbd6866a96bd8c6e):
//
//  REST (CLOB, base https://clob.polymarket.com)
//   GET /                      health ("ok")                       [py-clob-client client.py:197]
//   GET /time                  server time                         [py-clob-client client.py:204]
//   GET /book?token_id=        {market,asset_id,timestamp,bids,asks,
//                               min_order_size,tick_size,neg_risk,
//                               last_trade_price,hash}              [clob-client types.ts:316]
//   GET /tick-size?token_id=   {"minimum_tick_size":"0.01"}        [py-sdk market_data.py:52]
//   GET /neg-risk?token_id=    {"neg_risk":bool}                   [py-sdk market_data.py:64]
//   GET /clob-markets/{cid}    {"nr":bool,"mts":"0.01","t":[{"t":"<token>"}],
//                               "fd":{"r":rate,"e":exponent}}       [py-sdk market_data.py:100,225]
//   GET /markets-by-token/{t}  condition id for a token            [py-sdk market_data.py:78]
//   GET /markets/{cid}         full CLOB market object             [py-clob-client client.py:1027]
//   GET /fee-rate?token_id=    {"base_fee":<bps>}                  [py-clob-client client.py:450]
//   GET /data/order/{id}       single order (L2)                   [docs trading/manage-orders]
//   GET /data/orders           open orders, cursor paginated (L2)  [py-sdk account.py:62]
//   GET /data/trades           trade history, cursor paginated(L2) [py-sdk account.py:109]
//   GET /balance-allowance     {balance,allowances{spender:amt}}(L2)[py-sdk account.py:190,201]
//   POST /order                place one order (L2 + EIP-712)      [py-sdk orders/post.py:19]
//   POST /orders               batch (<=15) (L2)                   [py-sdk orders/post.py:23]
//   DELETE /order              cancel one {"orderID":...} (L2)     [py-sdk orders/cancel.py:12]
//   DELETE /orders             cancel many (<=3000) (L2)           [py-sdk orders/cancel.py:17]
//   DELETE /cancel-all         cancel everything (L2)              [py-sdk orders/cancel.py:28]
//   DELETE /cancel-market-orders {"market","asset_id"} (L2)        [py-sdk orders/cancel.py:31]
//   POST /v1/heartbeats        {"heartbeat_id":""} → new id (L2)   [py-clob-client client.py:713]
//
//  REST (Gamma, base https://gamma-api.polymarket.com)
//   GET /markets/slug/{slug}   market discovery/status             [docs market-data/market-details]
//   GET /markets?condition_ids=…&closed=false                      [docs getting-started/api]
//   Gamma fields: conditionId, active, closed, archived, acceptingOrders,
//   enableOrderBook, negRisk, restricted, clobTokenIds, outcomes,
//   orderMinSize, orderPriceMinTickSize, feesEnabled, feeSchedule
//   {rate,exponent,takerOnly,rebateRate}, secondsDelay              [py-sdk gamma/market.py:486]
//
//  WebSocket
//   wss://ws-subscriptions-clob.polymarket.com/ws/market           [py-sdk environments.py:100]
//   wss://ws-subscriptions-clob.polymarket.com/ws/user             [py-sdk environments.py:101]
//
//  Contracts (Polygon mainnet, chain id 137)                        [docs resources/contracts]
//   CTF Exchange (V2)          0xE111180000d2663C0091e4f400237545B87B996B
//   Neg Risk CTF Exchange      0xe2222d279d744050d28e00520010520000310F59
//   Conditional Tokens (CTF)   0x4D97DCd97eC945f40cF65F87097ACe5EA0476045
//   pUSD collateral            0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB
//   Neg Risk Adapter (v1, dep) 0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296
//   Combos Exchange (v3)       0xe3333700cA9d93003F00f0F71f8515005F6c00Aa
//   PositionManager (proxy)    0x006F54F7f9A22e0000CC2AB60031000000ae9fEF
//
// Constants are permitted here only because they are cross-checked at runtime:
// the chain id is verified with `eth_chainId` against POLYGON_RPC_URL and the
// contract set is re-validated by the preflight binary before live trading.
// ─────────────────────────────────────────────────────────────────────────────

#include <strings.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "asset_type.hpp"
#include "json_scan.hpp"
#include "order_state.hpp"

namespace venue {

// ── Chain and contracts ─────────────────────────────────────────────────────
inline constexpr uint64_t K_POLYGON_CHAIN_ID = 137;
inline constexpr uint64_t K_AMOY_CHAIN_ID = 80002;  // testnet, never used live

inline constexpr uint8_t K_COLLATERAL_PUSD[20] = {
    0xC0,0x11,0xa7,0xE1,0x2a,0x19,0xf7,0xB1,0xf6,0x70,
    0xd4,0x6F,0x03,0xB0,0x3f,0x33,0x42,0xE8,0x2D,0xFB};
inline constexpr uint8_t K_CONDITIONAL_TOKENS[20] = {
    0x4D,0x97,0xDC,0xd9,0x7e,0xC9,0x45,0xf4,0x0c,0xF6,
    0x5F,0x87,0x09,0x7A,0xCe,0x5E,0xA0,0x47,0x60,0x45};
inline constexpr uint8_t K_EXCHANGE_STANDARD[20] = {
    0xE1,0x11,0x18,0x00,0x00,0xd2,0x66,0x3C,0x00,0x91,
    0xe4,0xf4,0x00,0x23,0x75,0x45,0xB8,0x7B,0x99,0x6B};
inline constexpr uint8_t K_EXCHANGE_NEG_RISK[20] = {
    0xe2,0x22,0x2d,0x27,0x9d,0x74,0x40,0x50,0xd2,0x8e,
    0x00,0x52,0x00,0x10,0x52,0x00,0x00,0x31,0x0F,0x59};
inline constexpr uint8_t K_NEG_RISK_ADAPTER[20] = {
    0xd9,0x1E,0x80,0xcF,0x2E,0x7b,0xe2,0xe1,0x62,0xc6,
    0x51,0x3c,0xeD,0x06,0xf1,0xdD,0x0d,0xA3,0x52,0x96};
inline constexpr uint8_t K_EXCHANGE_V3[20] = {
    0xe3,0x33,0x37,0x00,0xcA,0x9d,0x93,0x00,0x3F,0x00,
    0xf0,0xF7,0x1f,0x85,0x15,0x00,0x5F,0x6c,0x00,0xAa};
inline constexpr uint8_t K_POSITION_MANAGER[20] = {
    0x00,0x6F,0x54,0xF7,0xf9,0xA2,0x2e,0x00,0x00,0xCC,
    0x2A,0xB6,0x00,0x31,0x00,0x00,0x00,0xae,0x9f,0xEF};

inline constexpr uint64_t K_COLLATERAL_DECIMALS = 6;  // pUSD, 1e6 base units
inline constexpr uint64_t K_OUTCOME_DECIMALS = 6;     // CTF positions, 1e6

// ── Endpoints (paths only; hosts come from configuration) ───────────────────
inline constexpr const char* K_PATH_HEALTH = "/";
inline constexpr const char* K_PATH_TIME = "/time";
inline constexpr const char* K_PATH_BOOK = "/book";
inline constexpr const char* K_PATH_BOOKS = "/books";
inline constexpr const char* K_PATH_TICK_SIZE = "/tick-size";
inline constexpr const char* K_PATH_NEG_RISK = "/neg-risk";
inline constexpr const char* K_PATH_FEE_RATE = "/fee-rate";
inline constexpr const char* K_PATH_CLOB_MARKETS = "/clob-markets/";
inline constexpr const char* K_PATH_MARKETS = "/markets";
inline constexpr const char* K_PATH_MARKET = "/markets/";
inline constexpr const char* K_PATH_MARKETS_BY_TOKEN = "/markets-by-token/";
inline constexpr const char* K_PATH_ORDER = "/data/order/";
inline constexpr const char* K_PATH_ORDERS = "/data/orders";
inline constexpr const char* K_PATH_TRADES = "/data/trades";
inline constexpr const char* K_PATH_BALANCE_ALLOWANCE = "/balance-allowance";
inline constexpr const char* K_PATH_POST_ORDER = "/order";
inline constexpr const char* K_PATH_POST_ORDERS = "/orders";
inline constexpr const char* K_PATH_CANCEL_ALL = "/cancel-all";
inline constexpr const char* K_PATH_CANCEL_MARKET = "/cancel-market-orders";
// Both spellings are documented: /v1/heartbeats by the official SDKs
// (py-clob-client endpoints.py:48, clob-client src/endpoints.ts:81) and
// /heartbeats by the official OpenAPI page
// (docs api-reference/trade/send-heartbeat).  The client tries the SDK spelling
// first and falls back once on 404.
inline constexpr const char* K_PATH_HEARTBEAT = "/v1/heartbeats";
inline constexpr const char* K_PATH_HEARTBEAT_OPENAPI = "/heartbeats";
inline constexpr const char* K_PATH_NOTIFICATIONS = "/notifications";
inline constexpr const char* K_PATH_ORDER_SCORING = "/order-scoring";

inline constexpr const char* K_DEFAULT_CLOB_HOST = "https://clob.polymarket.com";
inline constexpr const char* K_DEFAULT_GAMMA_HOST = "https://gamma-api.polymarket.com";
inline constexpr const char* K_DEFAULT_DATA_HOST = "https://data-api.polymarket.com";
inline constexpr const char* K_DEFAULT_MARKET_WS =
    "wss://ws-subscriptions-clob.polymarket.com/ws/market";
inline constexpr const char* K_DEFAULT_USER_WS =
    "wss://ws-subscriptions-clob.polymarket.com/ws/user";

// ── Tick sizes the venue supports ───────────────────────────────────────────
// py-sdk _ROUNDING_BY_TICK / clob-client TickSize, extended with the two
// additional values the unified SDK accepts (0.005, 0.0025).
inline constexpr uint64_t K_SUPPORTED_TICKS[6] = {100000, 10000, 5000, 2500, 1000, 100};

inline bool tick_supported(uint64_t tick_raw) noexcept {
    for (uint64_t supported : K_SUPPORTED_TICKS)
        if (supported == tick_raw) return true;
    return false;
}

// Amount precision (1e6 base units) per tick, matching the official rounding
// table: 0.1→3, 0.01→4, 0.005→5, 0.0025→6, 0.001→5, 0.0001→6 decimals.
inline uint64_t amount_quantum_for_tick(uint64_t tick_raw) noexcept {
    switch (tick_raw) {
        case 100000: return 1000;      // 0.001
        case 10000:  return 100;       // 0.0001
        case 5000:   return 10;        // 0.00001
        case 2500:   return 1;         // 0.000001
        case 1000:   return 10;        // 0.00001
        case 100:    return 1;         // 0.000001
        default:     return 0;         // unsupported → caller fails closed
    }
}

// ── Protocol-v2 position ids ────────────────────────────────────────────────
// py-sdk _internal/protocol.py: a token occupies the protocol-v2 namespace when
// (value & (((1<<64)-1) << 40)) == 0, i.e. bits 40..103 of the uint256 are
// zero.  Those assets settle through the Combos Exchange (v3) and the
// PositionManager rather than the CTF exchanges, so they must not be traded by
// a signer/allowance setup that targets the CTF exchange.
// value*10 + add decomposed into 64-bit low/high halves without relying on
// non-standard 128-bit integers (keeps -Wpedantic clean on every toolchain).
inline void mul10_add_u64(uint64_t value, uint64_t add, uint64_t& low,
                          uint64_t& high) noexcept {
    const uint64_t a_hi = value >> 32;
    const uint64_t a_lo = value & 0xFFFFFFFFULL;
    const uint64_t t = a_lo * 10 + add;   // < 2^36: cannot overflow
    const uint64_t u = a_hi * 10;         // < 2^36: cannot overflow
    const uint64_t mid = u + (t >> 32);   // < 2^37: cannot overflow
    low = (mid << 32) | (t & 0xFFFFFFFFULL);
    high = mid >> 32;
}

// Parses a decimal uint256 string into four base-2^64 limbs (little endian).
// Returns false when the text is not a plain decimal integer or exceeds 2^256.
inline bool parse_uint256_limbs(const char* text, size_t len,
                                uint64_t limbs[4]) noexcept {
    if (!json_scan::is_decimal_integer(text, len)) return false;
    limbs[0] = limbs[1] = limbs[2] = limbs[3] = 0;
    for (size_t i = 0; i < len; ++i) {
        uint64_t carry = static_cast<uint64_t>(text[i] - '0');
        for (int word = 0; word < 4; ++word) {
            uint64_t low = 0;
            uint64_t high = 0;
            mul10_add_u64(limbs[word], carry, low, high);
            limbs[word] = low;
            carry = high;
        }
        if (carry) return false;  // exceeds uint256
    }
    return true;
}

inline bool is_protocol_v2_position_id(const char* token_id) noexcept {
    if (!token_id) return false;
    uint64_t limbs[4] = {0, 0, 0, 0};
    if (!parse_uint256_limbs(token_id, std::strlen(token_id), limbs)) return false;
    const bool low_bits_clear = (limbs[0] >> 40) == 0;
    const bool next_bits_clear = (limbs[1] & ((1ULL << 40) - 1ULL)) == 0;
    return low_bits_clear && next_bits_clear;
}

// ── Metadata aggregate ──────────────────────────────────────────────────────
enum class MetadataSource : uint8_t {
    NONE = 0, BOOK = 1, TICK_SIZE = 2, NEG_RISK = 3, CLOB_MARKET = 4,
    FEE_RATE = 5, GAMMA = 6, USER_WS = 7, MARKET_WS = 8
};

inline const char* metadata_source_name(MetadataSource source) noexcept {
    switch (source) {
        case MetadataSource::NONE: return "none";
        case MetadataSource::BOOK: return "clob:/book";
        case MetadataSource::TICK_SIZE: return "clob:/tick-size";
        case MetadataSource::NEG_RISK: return "clob:/neg-risk";
        case MetadataSource::CLOB_MARKET: return "clob:/clob-markets";
        case MetadataSource::FEE_RATE: return "clob:/fee-rate";
        case MetadataSource::GAMMA: return "gamma:/markets";
        case MetadataSource::USER_WS: return "wss:user";
        case MetadataSource::MARKET_WS: return "wss:market";
    }
    return "unknown";
}

struct MarketMetadata {
    static constexpr size_t K_MAX_TOKENS = 4;

    char condition_id[70]{};
    char token_id[80]{};                 // the outcome token this bot trades
    char tokens[K_MAX_TOKENS][80]{};     // every token of the market
    size_t token_count = 0;

    uint64_t tick_raw = 0;
    uint64_t min_size_raw = 0;
    bool neg_risk = false;
    bool protocol_v2_position = false;

    // Fee schedule (docs: fee = shares × rate × price × (1 - price) for
    // exponent 1; `exponent` scales the price component).
    uint64_t fee_rate_micro = 0;      // rate × 1e6 (0.07 → 70000)
    uint64_t fee_exponent_micro = 0;  // exponent × 1e6 (1 → 1000000)
    uint64_t fee_rate_bps = 0;        // /fee-rate base_fee, kept for audit
    bool fees_enabled = false;
    bool fee_taker_only = true;
    uint64_t maker_rebate_micro = 0;

    // Market status (Gamma)
    bool active = false;
    bool closed = true;
    bool archived = true;
    bool accepting_orders = false;
    bool enable_order_book = false;
    bool restricted = true;
    bool status_known = false;
    int64_t seconds_delay = 0;

    // Book snapshot provenance
    char book_hash[70]{};
    uint64_t book_timestamp_ms = 0;
    uint64_t last_trade_price_raw = 0;

    // Which sources contributed, and when (wall clock ns).
    uint32_t sources_seen = 0;
    uint64_t observed_wall_ns = 0;

    void mark_source(MetadataSource source) noexcept {
        sources_seen |= 1U << static_cast<unsigned>(source);
    }
    bool saw(MetadataSource source) const noexcept {
        return (sources_seen & (1U << static_cast<unsigned>(source))) != 0;
    }
    void reset() noexcept { std::memset(this, 0, sizeof(*this)); closed = true; archived = true; restricted = true; fee_taker_only = true; }

    bool add_token(const char* token) noexcept {
        if (!token || !*token) return false;
        for (size_t i = 0; i < token_count; ++i)
            if (std::strcmp(tokens[i], token) == 0) return true;
        if (token_count >= K_MAX_TOKENS) return false;
        std::snprintf(tokens[token_count], sizeof(tokens[token_count]), "%s", token);
        ++token_count;
        return true;
    }
    bool has_token(const char* token) const noexcept {
        if (!token || !*token) return false;
        for (size_t i = 0; i < token_count; ++i)
            if (std::strcmp(tokens[i], token) == 0) return true;
        return false;
    }
};

// ── Validation report ───────────────────────────────────────────────────────
struct MetadataReport {
    static constexpr size_t K_MAX_REASONS = 10;
    bool ok = false;
    size_t reason_count = 0;
    char reasons[K_MAX_REASONS][64]{};

    void fail(const char* reason) noexcept {
        ok = false;
        if (reason_count < K_MAX_REASONS) {
            std::snprintf(reasons[reason_count], sizeof(reasons[reason_count]),
                          "%s", reason);
            ++reason_count;
        }
    }
    void reset() noexcept {
        ok = true;
        reason_count = 0;
        for (auto& reason : reasons) reason[0] = '\0';
    }
    void format(char* out, size_t cap) const noexcept {
        if (!out || cap == 0) return;
        size_t written = 0;
        out[0] = '\0';
        for (size_t i = 0; i < reason_count; ++i) {
            const size_t length = std::strlen(reasons[i]);
            if (written + length + 2 >= cap) break;
            if (written) { out[written++] = ';'; out[written] = '\0'; }
            std::memcpy(out + written, reasons[i], length);
            written += length;
            out[written] = '\0';
        }
    }
};

struct MetadataPolicy {
    bool require_gamma_status = true;   // live: refuse to trade without status
    bool allow_protocol_v2 = false;     // Combos/v3 exchange path
    bool allow_nonunit_fee_exponent = false;
    uint64_t max_taker_fee_micro = 70000;  // 0.07, the highest documented rate
};

// ── Parsers ─────────────────────────────────────────────────────────────────
// Every parser is pure: buffer in, typed values out, no I/O, no allocation.
// They return false for malformed input; partial recognition is reported
// through explicit output flags so the caller can fail closed on gaps.

// GET /book?token_id=…  →  {market,asset_id,timestamp,bids,asks,
//                           min_order_size,tick_size,neg_risk,last_trade_price,hash}
struct BookSnapshot {
    static constexpr size_t K_MAX_LEVELS = 100;
    char market[70]{};
    char asset_id[80]{};
    uint64_t timestamp_ms = 0;
    uint64_t bid_price[K_MAX_LEVELS]{};
    uint64_t bid_size[K_MAX_LEVELS]{};
    uint64_t ask_price[K_MAX_LEVELS]{};
    uint64_t ask_size[K_MAX_LEVELS]{};
    size_t bid_count = 0;
    size_t ask_count = 0;
    uint64_t min_order_size_raw = 0;
    uint64_t tick_raw = 0;
    bool neg_risk = false;
    uint64_t last_trade_price_raw = 0;
    char hash[70]{};
    bool min_order_size_valid = false;
    bool tick_valid = false;
    bool neg_risk_valid = false;
};

inline bool parse_levels(const char* data, size_t len, size_t start, size_t end,
                         uint64_t* prices, uint64_t* sizes, size_t cap,
                         size_t& count) noexcept {
    count = 0;
    const size_t total = json_scan::array_count(data, len, start, end);
    for (size_t i = 0; i < total && count < cap; ++i) {
        size_t es = 0;
        size_t ee = 0;
        if (!json_scan::array_element(data, len, start, end, i, es, ee)) return false;
        uint64_t price = 0;
        uint64_t size = 0;
        if (!json_scan::get_fixed(data + es, ee - es, "price", 1000000, price))
            return false;
        if (!json_scan::get_fixed(data + es, ee - es, "size", 1000000, size))
            return false;
        prices[count] = price;
        sizes[count] = size;
        ++count;
    }
    return true;
}

inline bool parse_book(const char* data, size_t len, BookSnapshot& out) noexcept {
    out = BookSnapshot{};
    if (!data || len == 0) return false;
    if (json_scan::count_key(data, len, "event_type") > 1) return false;
    if (!json_scan::get_string(data, len, "asset_id", out.asset_id,
                               sizeof(out.asset_id))) {
        if (!json_scan::get_string(data, len, "token_id", out.asset_id,
                                   sizeof(out.asset_id)))
            return false;
    }
    if (!json_scan::is_decimal_integer(out.asset_id, std::strlen(out.asset_id)))
        return false;
    (void)json_scan::get_string(data, len, "market", out.market, sizeof(out.market));
    (void)json_scan::get_u64(data, len, "timestamp", out.timestamp_ms);
    if (!out.timestamp_ms) {
        // /book returns the timestamp as a string on some deployments.
        char text[32]{};
        if (json_scan::get_string(data, len, "timestamp", text, sizeof(text)))
            (void)json_scan::parse_fixed(text, std::strlen(text), 1, out.timestamp_ms);
    }
    size_t start = 0;
    size_t end = 0;
    if (!json_scan::find_array(data, len, "bids", start, end)) return false;
    if (!parse_levels(data, len, start, end, out.bid_price, out.bid_size,
                      BookSnapshot::K_MAX_LEVELS, out.bid_count))
        return false;
    if (!json_scan::find_array(data, len, "asks", start, end)) return false;
    if (!parse_levels(data, len, start, end, out.ask_price, out.ask_size,
                      BookSnapshot::K_MAX_LEVELS, out.ask_count))
        return false;
    out.min_order_size_valid =
        json_scan::get_fixed(data, len, "min_order_size", 1000000,
                             out.min_order_size_raw);
    out.tick_valid = json_scan::get_fixed(data, len, "tick_size", 1000000, out.tick_raw);
    out.neg_risk_valid = json_scan::get_bool(data, len, "neg_risk", out.neg_risk);
    (void)json_scan::get_fixed(data, len, "last_trade_price", 1000000,
                               out.last_trade_price_raw);
    (void)json_scan::get_string(data, len, "hash", out.hash, sizeof(out.hash));
    return true;
}

// GET /tick-size?token_id=… → {"minimum_tick_size":"0.01"}
inline bool parse_tick_size(const char* data, size_t len, uint64_t& tick_raw) noexcept {
    if (!data || len == 0) return false;
    if (!json_scan::get_fixed(data, len, "minimum_tick_size", 1000000, tick_raw))
        return false;
    return tick_supported(tick_raw);
}

// GET /neg-risk?token_id=… → {"neg_risk":true}
inline bool parse_neg_risk(const char* data, size_t len, bool& neg_risk) noexcept {
    if (!data || len == 0) return false;
    return json_scan::get_bool(data, len, "neg_risk", neg_risk);
}

// GET /fee-rate?token_id=… → {"base_fee":<bps>}   (kept for audit/cross-check)
inline bool parse_fee_rate_bps(const char* data, size_t len, uint64_t& bps) noexcept {
    if (!data || len == 0) return false;
    return json_scan::get_u64(data, len, "base_fee", bps);
}

// GET /clob-markets/{condition_id} → {"nr":bool,"mts":"0.01","t":[{"t":"<token>"}],
//                                     "fd":{"r":rate,"e":exponent}}
inline bool parse_clob_market(const char* data, size_t len,
                              MarketMetadata& metadata) noexcept {
    if (!data || len == 0) return false;
    bool value = false;
    if (json_scan::get_bool(data, len, "nr", value)) metadata.neg_risk = value;
    uint64_t tick = 0;
    if (json_scan::get_fixed(data, len, "mts", 1000000, tick)) {
        if (!tick_supported(tick)) return false;
        metadata.tick_raw = tick;
    }
    size_t start = 0;
    size_t end = 0;
    if (json_scan::find_array(data, len, "t", start, end)) {
        const size_t total = json_scan::array_count(data, len, start, end);
        for (size_t i = 0; i < total; ++i) {
            size_t es = 0;
            size_t ee = 0;
            if (!json_scan::array_element(data, len, start, end, i, es, ee))
                return false;
            char token[80]{};
            if (!json_scan::get_string(data + es, ee - es, "t", token, sizeof(token)))
                return false;
            if (!json_scan::is_decimal_integer(token, std::strlen(token))) return false;
            if (!metadata.add_token(token)) return false;
        }
    }
    size_t fd_start = 0;
    size_t fd_end = 0;
    if (json_scan::find_object(data, len, "fd", fd_start, fd_end)) {
        uint64_t rate = 0;
        uint64_t exponent = 0;
        if (json_scan::get_fixed(data + fd_start, fd_end - fd_start, "r", 1000000, rate))
            metadata.fee_rate_micro = rate;
        if (json_scan::get_fixed(data + fd_start, fd_end - fd_start, "e", 1000000,
                                 exponent))
            metadata.fee_exponent_micro = exponent;
        metadata.fees_enabled = metadata.fee_rate_micro != 0;
    }
    metadata.mark_source(MetadataSource::CLOB_MARKET);
    return true;
}

// Gamma market object.  Accepts both the single-object response
// (GET /markets/slug/{slug}) and one element of GET /markets?….
inline bool parse_gamma_market(const char* data, size_t len,
                               MarketMetadata& metadata) noexcept {
    if (!data || len == 0) return false;
    char condition[70]{};
    if (!json_scan::get_string(data, len, "conditionId", condition, sizeof(condition)))
        return false;
    if (!json_scan::is_hex_bytes(condition, std::strlen(condition), 32)) return false;
    if (metadata.condition_id[0] &&
        std::strcmp(metadata.condition_id, condition) != 0)
        return false;  // different market than the one we resolved
    std::snprintf(metadata.condition_id, sizeof(metadata.condition_id), "%s",
                  condition);

    bool value = false;
    if (json_scan::get_bool(data, len, "active", value)) metadata.active = value;
    if (json_scan::get_bool(data, len, "closed", value)) metadata.closed = value;
    if (json_scan::get_bool(data, len, "archived", value)) metadata.archived = value;
    if (json_scan::get_bool(data, len, "acceptingOrders", value))
        metadata.accepting_orders = value;
    if (json_scan::get_bool(data, len, "enableOrderBook", value))
        metadata.enable_order_book = value;
    if (json_scan::get_bool(data, len, "restricted", value))
        metadata.restricted = value;
    else
        metadata.restricted = false;  // field is optional; absence is not a block
    if (json_scan::get_bool(data, len, "negRisk", value)) {
        if (metadata.saw(MetadataSource::CLOB_MARKET) ||
            metadata.saw(MetadataSource::NEG_RISK) ||
            metadata.saw(MetadataSource::BOOK)) {
            if (metadata.neg_risk != value) return false;  // disagreement
        }
        metadata.neg_risk = value;
    }
    metadata.status_known = true;
    int64_t seconds_delay = 0;
    if (json_scan::get_i64(data, len, "secondsDelay", seconds_delay))
        metadata.seconds_delay = seconds_delay;

    // clobTokenIds is a JSON-encoded array *string*: "[\"123\",\"456\"]".
    char encoded[512]{};
    if (json_scan::get_string(data, len, "clobTokenIds", encoded, sizeof(encoded))) {
        const size_t encoded_len = std::strlen(encoded);
        if (encoded_len < 2 || encoded[0] != '[') return false;
        size_t cursor = 1;
        while (cursor < encoded_len) {
            while (cursor < encoded_len &&
                   (encoded[cursor] == ' ' || encoded[cursor] == ','))
                ++cursor;
            if (cursor >= encoded_len || encoded[cursor] == ']') break;
            if (encoded[cursor] != '"') return false;
            ++cursor;
            const size_t begin = cursor;
            while (cursor < encoded_len && encoded[cursor] != '"') ++cursor;
            if (cursor >= encoded_len) return false;
            char token[80]{};
            const size_t token_len = cursor - begin;
            if (token_len == 0 || token_len >= sizeof(token)) return false;
            std::memcpy(token, encoded + begin, token_len);
            token[token_len] = '\0';
            if (!json_scan::is_decimal_integer(token, token_len)) return false;
            if (!metadata.add_token(token)) return false;
            ++cursor;
        }
    }

    uint64_t min_size = 0;
    if (json_scan::get_fixed(data, len, "orderMinSize", 1000000, min_size)) {
        if (min_size == 0) return false;
        if (metadata.min_size_raw && metadata.min_size_raw != min_size) return false;
        metadata.min_size_raw = min_size;
    }
    uint64_t tick = 0;
    if (json_scan::get_fixed(data, len, "orderPriceMinTickSize", 1000000, tick)) {
        if (!tick_supported(tick)) return false;
        if (metadata.tick_raw && metadata.tick_raw != tick) return false;
        metadata.tick_raw = tick;
    }
    if (json_scan::get_bool(data, len, "feesEnabled", value))
        metadata.fees_enabled = value;
    size_t fs_start = 0;
    size_t fs_end = 0;
    if (json_scan::find_object(data, len, "feeSchedule", fs_start, fs_end)) {
        uint64_t rate = 0;
        uint64_t exponent = 0;
        if (json_scan::get_fixed(data + fs_start, fs_end - fs_start, "rate", 1000000,
                                 rate)) {
            if (metadata.fee_rate_micro && metadata.fee_rate_micro != rate)
                return false;
            metadata.fee_rate_micro = rate;
        }
        if (json_scan::get_fixed(data + fs_start, fs_end - fs_start, "exponent",
                                 1000000, exponent)) {
            if (metadata.fee_exponent_micro && metadata.fee_exponent_micro != exponent)
                return false;
            metadata.fee_exponent_micro = exponent;
        }
        if (json_scan::get_bool(data + fs_start, fs_end - fs_start, "takerOnly", value))
            metadata.fee_taker_only = value;
        uint64_t rebate = 0;
        if (json_scan::get_fixed(data + fs_start, fs_end - fs_start, "rebateRate",
                                 1000000, rebate))
            metadata.maker_rebate_micro = rebate;
    }
    metadata.mark_source(MetadataSource::GAMMA);
    return true;
}

// GET /time → epoch seconds, returned either as a number or as a string.
inline bool parse_server_time(const char* data, size_t len, uint64_t& seconds) noexcept {
    if (!data || len == 0) return false;
    while (len && (json_scan::is_space(data[0]))) { ++data; --len; }
    while (len && (json_scan::is_space(data[len - 1]))) --len;
    if (len == 0) return false;
    if (data[0] == '"') {
        char text[32]{};
        if (len - 2 >= sizeof(text) || len < 2 || data[len - 1] != '"') return false;
        std::memcpy(text, data + 1, len - 2);
        text[len - 2] = '\0';
        return json_scan::parse_fixed(text, std::strlen(text), 1, seconds);
    }
    return json_scan::parse_fixed(data, len, 1, seconds);
}

// GET / → {"ok":true} or a bare "OK"/{} depending on deployment; treat any
// 2xx with a parseable body as healthy and let the caller record the text.
inline bool parse_health(const char* data, size_t len) noexcept {
    if (!data || len == 0) return false;
    bool ok = false;
    if (json_scan::get_bool(data, len, "ok", ok)) return ok;
    while (len && json_scan::is_space(data[0])) { ++data; --len; }
    return len >= 2 && data[0] == '{' && data[len - 1] == '}';
}

// GET /balance-allowance → {"balance":"<base units>",
//                           "allowances":{"<spender>":"<base units>"}}
// The legacy shape {"balance":"…","allowance":"…"} is still accepted.
struct BalanceAllowance {
    uint64_t balance = 0;
    bool balance_valid = false;
    uint64_t allowance = 0;           // for the requested spender
    bool allowance_valid = false;
    char spender[43]{};
    size_t spender_count = 0;
};

inline bool parse_balance_allowance(const char* data, size_t len,
                                    const char* wanted_spender,
                                    BalanceAllowance& out) noexcept {
    out = BalanceAllowance{};
    if (!data || len == 0) return false;
    char balance[40]{};
    if (json_scan::get_string(data, len, "balance", balance, sizeof(balance))) {
        if (!json_scan::parse_fixed(balance, std::strlen(balance), 1, out.balance))
            return false;
        out.balance_valid = true;
    } else {
        uint64_t value = 0;
        if (!json_scan::get_u64(data, len, "balance", value)) return false;
        out.balance = value;
        out.balance_valid = true;
    }
    size_t start = 0;
    size_t end = 0;
    if (json_scan::find_object(data, len, "allowances", start, end)) {
        // Walk the object's spender → amount pairs.
        size_t i = start + 1;
        while (i < end) {
            while (i < end && (json_scan::is_space(data[i]) || data[i] == ',')) ++i;
            if (i >= end || data[i] == '}') break;
            if (data[i] != '"') return false;
            const size_t after_key = json_scan::skip_string(data, len, i);
            if (!after_key || after_key > end) return false;
            char spender[43]{};
            const size_t key_len = after_key - i - 2;
            if (key_len >= sizeof(spender)) return false;
            std::memcpy(spender, data + i + 1, key_len);
            spender[key_len] = '\0';
            size_t j = after_key;
            while (j < end && json_scan::is_space(data[j])) ++j;
            if (j >= end || data[j] != ':') return false;
            ++j;
            while (j < end && json_scan::is_space(data[j])) ++j;
            const size_t value_end = json_scan::skip_value(data, len, j);
            if (!value_end || value_end > end) return false;
            char amount[40]{};
            size_t value_len = value_end - j;
            if (value_len >= 2 && data[j] == '"') {
                value_len -= 2;
                ++j;
            }
            if (value_len >= sizeof(amount)) return false;
            std::memcpy(amount, data + j, value_len);
            amount[value_len] = '\0';
            uint64_t value = 0;
            if (!json_scan::parse_fixed(amount, value_len, 1, value)) return false;
            ++out.spender_count;
            if (wanted_spender && *wanted_spender &&
                strcasecmp(spender, wanted_spender) == 0) {
                out.allowance = value;
                out.allowance_valid = true;
                std::snprintf(out.spender, sizeof(out.spender), "%s", spender);
            }
            i = value_end;
        }
        return true;
    }
    char allowance[40]{};
    if (json_scan::get_string(data, len, "allowance", allowance, sizeof(allowance))) {
        if (!json_scan::parse_fixed(allowance, std::strlen(allowance), 1, out.allowance))
            return false;
        out.allowance_valid = true;
        out.spender_count = 1;
        if (wanted_spender)
            std::snprintf(out.spender, sizeof(out.spender), "%s", wanted_spender);
        return true;
    }
    uint64_t value = 0;
    if (json_scan::get_u64(data, len, "allowance", value)) {
        out.allowance = value;
        out.allowance_valid = true;
        out.spender_count = 1;
        return true;
    }
    return false;
}

// ── Cross-source validation ─────────────────────────────────────────────────
inline void validate_metadata(const MarketMetadata& metadata,
                              const MetadataPolicy& policy,
                              MetadataReport& report) noexcept {
    report.reset();
    if (!json_scan::is_hex_bytes(metadata.condition_id,
                                 std::strlen(metadata.condition_id), 32))
        report.fail("condition_id_missing_or_malformed");
    if (!json_scan::is_decimal_integer(metadata.token_id,
                                       std::strlen(metadata.token_id)))
        report.fail("token_id_missing_or_malformed");
    if (metadata.token_count && !metadata.has_token(metadata.token_id))
        report.fail("token_not_in_market");
    if (!tick_supported(metadata.tick_raw)) report.fail("tick_size_unsupported");
    if (metadata.min_size_raw == 0) report.fail("min_order_size_missing");
    if (!metadata.saw(MetadataSource::NEG_RISK) &&
        !metadata.saw(MetadataSource::CLOB_MARKET) &&
        !metadata.saw(MetadataSource::BOOK) &&
        !metadata.saw(MetadataSource::GAMMA))
        report.fail("neg_risk_unknown");
    if (policy.require_gamma_status) {
        if (!metadata.status_known) report.fail("market_status_unknown");
        if (!metadata.active) report.fail("market_not_active");
        if (metadata.closed) report.fail("market_closed");
        if (metadata.archived) report.fail("market_archived");
        if (!metadata.accepting_orders) report.fail("market_not_accepting_orders");
        if (!metadata.enable_order_book) report.fail("order_book_disabled");
        if (metadata.restricted) report.fail("market_restricted");
    }
    if (metadata.fee_rate_micro > policy.max_taker_fee_micro)
        report.fail("taker_fee_above_policy");
    if (metadata.fees_enabled && metadata.fee_exponent_micro != 0 &&
        metadata.fee_exponent_micro != 1000000 && !policy.allow_nonunit_fee_exponent)
        report.fail("fee_exponent_unverified");
    if (is_protocol_v2_position_id(metadata.token_id)) {
        if (!policy.allow_protocol_v2) report.fail("protocol_v2_position_unsupported");
    }
    report.ok = report.reason_count == 0;
}

// ── Hot-path publication ────────────────────────────────────────────────────
// The decision loop must read one coherent metadata generation without taking
// a lock.  Same publication protocol as OrderBookL2: an odd sequence means "a
// write is in progress", so readers retry.
class MarketRuntime {
public:
    struct View {
        uint64_t tick_raw = 0;
        uint64_t min_size_raw = 0;
        uint64_t amount_quantum = 0;
        uint64_t fee_rate_micro = 0;
        bool neg_risk = false;
        bool valid = false;
        uint32_t generation = 0;
        uint64_t observed_wall_ns = 0;   // when the venue data was observed
    };

    // Publishes a validated snapshot.  Returns false when the metadata is not
    // usable, leaving the previous (still valid) generation untouched.
    bool publish(const MarketMetadata& metadata, const MetadataPolicy& policy,
                 MetadataReport& report) noexcept {
        validate_metadata(metadata, policy, report);
        if (!report.ok) {
            invalid_reason_generation_.fetch_add(1, std::memory_order_release);
            return false;
        }
        const uint64_t quantum = amount_quantum_for_tick(metadata.tick_raw);
        if (quantum == 0) {
            report.fail("amount_quantum_unsupported");
            report.ok = false;
            return false;
        }
        seq_.fetch_add(1, std::memory_order_seq_cst);  // odd: write in progress
        tick_raw_.store(metadata.tick_raw, std::memory_order_seq_cst);
        min_size_raw_.store(metadata.min_size_raw, std::memory_order_seq_cst);
        amount_quantum_.store(quantum, std::memory_order_seq_cst);
        fee_rate_micro_.store(metadata.fee_rate_micro, std::memory_order_seq_cst);
        neg_risk_.store(metadata.neg_risk, std::memory_order_seq_cst);
        // The observation timestamp is stored before valid_ so that a reader
        // that sees a valid generation also sees its age.
        observed_wall_ns_.store(metadata.observed_wall_ns, std::memory_order_seq_cst);
        valid_.store(true, std::memory_order_seq_cst);
        generation_.fetch_add(1, std::memory_order_seq_cst);
        seq_.fetch_add(1, std::memory_order_seq_cst);  // even: coherent
        std::snprintf(condition_id_, sizeof(condition_id_), "%s",
                      metadata.condition_id);
        std::snprintf(token_id_, sizeof(token_id_), "%s", metadata.token_id);
        return true;
    }

    // Invalidates immediately (disconnect, tick-size change, reconciliation
    // divergence).  The hot path then sees valid == false and stops trading.
    void invalidate() noexcept {
        seq_.fetch_add(1, std::memory_order_seq_cst);
        valid_.store(false, std::memory_order_seq_cst);
        observed_wall_ns_.store(0, std::memory_order_seq_cst);  // age unknown
        seq_.fetch_add(1, std::memory_order_seq_cst);
        invalid_reason_generation_.fetch_add(1, std::memory_order_release);
    }

    bool read(View& out) const noexcept {
        const uint32_t s1 = seq_.load(std::memory_order_seq_cst);
        if (s1 & 1U) return false;
        out.tick_raw = tick_raw_.load(std::memory_order_seq_cst);
        out.min_size_raw = min_size_raw_.load(std::memory_order_seq_cst);
        out.amount_quantum = amount_quantum_.load(std::memory_order_seq_cst);
        out.fee_rate_micro = fee_rate_micro_.load(std::memory_order_seq_cst);
        out.neg_risk = neg_risk_.load(std::memory_order_seq_cst);
        out.valid = valid_.load(std::memory_order_seq_cst);
        out.generation = generation_.load(std::memory_order_seq_cst);
        out.observed_wall_ns = observed_wall_ns_.load(std::memory_order_seq_cst);
        const uint32_t s2 = seq_.load(std::memory_order_seq_cst);
        return s1 == s2 && !(s2 & 1U);
    }

    uint32_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }
    // Wall-clock ns at which the published generation was observed, 0 when
    // nothing is published or the view was invalidated.  Single atomic load:
    // written only inside the publication window, so a reader either sees the
    // previous (older, i.e. more conservative) timestamp or the new one, and
    // never a timestamp for a generation that is not published.
    uint64_t observed_wall_ns() const noexcept {
        return observed_wall_ns_.load(std::memory_order_acquire);
    }
    bool valid() const noexcept { return valid_.load(std::memory_order_acquire); }
    const char* token_id() const noexcept { return token_id_; }
    const char* condition_id() const noexcept { return condition_id_; }

private:
    mutable std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> tick_raw_{0};
    std::atomic<uint64_t> min_size_raw_{0};
    std::atomic<uint64_t> amount_quantum_{0};
    std::atomic<uint64_t> fee_rate_micro_{0};
    std::atomic<bool> neg_risk_{false};
    std::atomic<bool> valid_{false};
    std::atomic<uint32_t> generation_{0};
    std::atomic<uint64_t> observed_wall_ns_{0};
    std::atomic<uint64_t> invalid_reason_generation_{0};
    char condition_id_[70]{};
    char token_id_[80]{};
};

// ── Bounded lifetime of venue-derived parameters ────────────────────────────
// Tick size, minimum order size, fee rate and the negative-risk flag all come
// from the venue and can change under us, so a snapshot may not drive order
// construction indefinitely.  Every unknown is fail-closed:
//   * never observed (0) or a zero budget -> not fresh
//   * wall clock moved backwards          -> not fresh (a negative age is
//                                            nonsense, never "very fresh")
//   * age above the budget                -> not fresh
inline bool metadata_is_fresh(uint64_t observed_wall_ns, uint64_t max_age_ms,
                              uint64_t now_wall_ns) noexcept {
    if (observed_wall_ns == 0 || max_age_ms == 0) return false;
    if (now_wall_ns < observed_wall_ns) return false;
    return (now_wall_ns - observed_wall_ns) / 1000000ULL <= max_age_ms;
}

// Age in milliseconds, for logging.  UINT64_MAX means "cannot be established"
// (never observed, or the clock moved backwards) and must be treated as stale.
inline uint64_t metadata_age_ms(uint64_t observed_wall_ns,
                                uint64_t now_wall_ns) noexcept {
    if (observed_wall_ns == 0 || now_wall_ns < observed_wall_ns) return UINT64_MAX;
    return (now_wall_ns - observed_wall_ns) / 1000000ULL;
}

}  // namespace venue

#endif  // VENUE_METADATA_HPP
