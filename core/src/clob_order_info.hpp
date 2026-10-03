#ifndef CLOB_ORDER_INFO_HPP
#define CLOB_ORDER_INFO_HPP

// Phase 5 — bounded parsers for the authenticated CLOB order/trade shapes.
//
// Source of truth for every field below: the official OpenAPI reference
// (docs.polymarket.com, "Get user orders", "Get single order by ID",
// "Get trades") and the official client's endpoint constants
// (github.com/Polymarket/clob-client/src/endpoints.ts). The shapes are:
//
//   GET /data/orders        -> {limit, next_cursor, count, data:[OpenOrder]}
//   GET /data/order/{id}    -> OpenOrder
//   GET /data/trades        -> {limit, next_cursor, count, data:[Trade]}
//   GET /auth/ban-status/closed-only -> {closed_only}
//
// Rules (same doctrine as json_field/user_ws_client):
//   * a duplicate semantic key is ambiguous and rejects the whole object;
//   * every field the venue marks required is required here;
//   * sizes/prices arrive as DECIMAL STRINGS already normalised by the venue
//     ("Already normalized; do not divide by 1e6"), so they are parsed as
//     exact 1e-6 decimals and never reinterpreted as raw 6-decimal integers;
//   * an unknown status/enum value is preserved as kUnknown and the caller
//     treats it as "not proven", never as a default;
//   * no allocation, no I/O, no clocks.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/json_field.hpp"

namespace clob_info {

using json_field::Span;

// ── enums ────────────────────────────────────────────────────────────────────

enum class VenueOrderStatus : uint8_t {
    kUnknown = 0,
    kLive = 1,
    kInvalid = 2,
    kCanceledMarketResolved = 3,
    kCanceled = 4,
    kMatched = 5,
};

inline const char* venue_order_status_name(VenueOrderStatus status) noexcept {
    switch (status) {
        case VenueOrderStatus::kLive: return "LIVE";
        case VenueOrderStatus::kInvalid: return "INVALID";
        case VenueOrderStatus::kCanceledMarketResolved:
            return "CANCELED_MARKET_RESOLVED";
        case VenueOrderStatus::kCanceled: return "CANCELED";
        case VenueOrderStatus::kMatched: return "MATCHED";
        case VenueOrderStatus::kUnknown: break;
    }
    return "UNKNOWN";
}

// Exact match, case sensitive: the venue documents upper-case enum values and
// a lower-case variant would be an undocumented value, not a synonym.
inline VenueOrderStatus parse_venue_order_status(const char* text,
                                                 size_t length) noexcept {
    if (!text) return VenueOrderStatus::kUnknown;
    struct Entry {
        const char* text;
        VenueOrderStatus status;
    };
    static constexpr Entry kEntries[] = {
        {"LIVE", VenueOrderStatus::kLive},
        {"INVALID", VenueOrderStatus::kInvalid},
        {"CANCELED_MARKET_RESOLVED", VenueOrderStatus::kCanceledMarketResolved},
        {"CANCELED", VenueOrderStatus::kCanceled},
        {"MATCHED", VenueOrderStatus::kMatched},
    };
    for (const Entry& entry : kEntries) {
        if (std::strlen(entry.text) == length &&
            std::memcmp(entry.text, text, length) == 0)
            return entry.status;
    }
    return VenueOrderStatus::kUnknown;
}

enum class VenueSide : uint8_t { kUnknown = 0, kBuy = 1, kSell = 2 };

inline VenueSide parse_venue_side(const char* text, size_t length) noexcept {
    if (!text) return VenueSide::kUnknown;
    if (length == 3 && std::memcmp(text, "BUY", 3) == 0) return VenueSide::kBuy;
    if (length == 4 && std::memcmp(text, "SELL", 4) == 0) return VenueSide::kSell;
    return VenueSide::kUnknown;
}

enum class TradeStatus : uint8_t {
    kUnknown = 0,
    kMatched = 1,
    kMatchedNotBroadcasted = 2,
    kMined = 3,
    kConfirmed = 4,
    kRetrying = 5,
    kFailed = 6,
};

inline TradeStatus parse_trade_status(const char* text, size_t length) noexcept {
    if (!text) return TradeStatus::kUnknown;
    struct Entry {
        const char* text;
        TradeStatus status;
    };
    static constexpr Entry kEntries[] = {
        {"TRADE_STATUS_MATCHED", TradeStatus::kMatched},
        {"TRADE_STATUS_MATCHED_NOT_BROADCASTED",
         TradeStatus::kMatchedNotBroadcasted},
        {"TRADE_STATUS_MINED", TradeStatus::kMined},
        {"TRADE_STATUS_CONFIRMED", TradeStatus::kConfirmed},
        {"TRADE_STATUS_RETRYING", TradeStatus::kRetrying},
        {"TRADE_STATUS_FAILED", TradeStatus::kFailed},
    };
    for (const Entry& entry : kEntries) {
        if (std::strlen(entry.text) == length &&
            std::memcmp(entry.text, text, length) == 0)
            return entry.status;
    }
    return TradeStatus::kUnknown;
}

// ── bounded field helpers ────────────────────────────────────────────────────

// Every required string field: unique key, JSON string, fits the buffer.
inline bool require_string(const char* json, size_t length, const char* key,
                           char* out, size_t cap) noexcept {
    if (!json_field::string(json, length, key, out, cap)) return false;
    return out[0] != '\0';
}

inline bool require_fixed6(const char* json, size_t length, const char* key,
                           uint64_t& out) noexcept {
    return json_field::fixed6(json, length, key, out);
}

inline bool parse_u64_text(const char* text, size_t length,
                           uint64_t& out) noexcept {
    if (!text || length == 0 || length > 19) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    out = value;
    return true;
}

// Integer token only: used for fields the schema types as integers.
inline bool require_u64(const char* json, size_t length, const char* key,
                        uint64_t& out) noexcept {
    Span span;
    if (!json_field::unique(json, length, key, span)) return false;
    if (json_field::span_is_string(span)) return false;
    return parse_u64_text(span.begin, span.size(), out);
}

// Integer or numeric string: the same field is documented as an integer in the
// OpenAPI schema and as a string in the published wire examples, so both are
// accepted instead of blocking on a documentation discrepancy.
inline bool require_u64_any(const char* json, size_t length, const char* key,
                            uint64_t& out) noexcept {
    Span span;
    if (!json_field::unique(json, length, key, span)) return false;
    if (!json_field::span_is_string(span))
        return parse_u64_text(span.begin, span.size(), out);
    char buffer[24];
    if (!json_field::unescape(span, buffer, sizeof(buffer))) return false;
    return parse_u64_text(buffer, std::strlen(buffer), out);
}

// Optional diagnostic field: absent or unparsable leaves the output empty
// instead of failing the whole object, because no policy depends on it.
inline void optional_string(const char* json, size_t length, const char* key,
                            char* out, size_t cap) noexcept {
    out[0] = '\0';
    (void)json_field::string(json, length, key, out, cap);
}

// Optional field whose name differs between the API reference and the
// published wire examples (market/condition_id, asset_id/token_id,
// expiration/expires_at). The alias is only consulted when the primary name is
// absent, and duplicates of either are rejected by json_field.
inline void optional_string_alias(const char* json, size_t length,
                                  const char* key, const char* alias,
                                  char* out, size_t cap) noexcept {
    optional_string(json, length, key, out, cap);
    if (out[0] == '\0') optional_string(json, length, alias, out, cap);
}

// ── array walking ────────────────────────────────────────────────────────────

// Visits every element of the top-level array `key`, in order. Element spans
// are exactly the element's own bytes, so the same depth-1 lookups can be
// applied to each one. Rejects trailing commas and malformed elements.
template <typename Visit, typename Context>
inline bool for_each_array_element(const char* json, size_t length,
                                   const char* key, Context& context,
                                   Visit visit) noexcept {
    Span array;
    if (!json_field::unique(json, length, key, array)) return false;
    if (!array.valid() || array.size() < 2 || *array.begin != '[' ||
        array.end[-1] != ']')
        return false;
    const char* p = array.begin + 1;
    const char* end = array.end - 1;
    bool need_value = true;  // true = a value is required here
    bool any = false;        // an element has been consumed already
    while (true) {
        while (p < end && json_field::is_space(*p)) ++p;
        if (p == end) return !need_value || !any;  // empty or completed
        if (!need_value) {
            if (*p != ',') return false;
            ++p;
            need_value = true;
            continue;
        }
        const char* start = p;
        if (!json_field::skip_value(p, end)) return false;
        if (!visit(Span{start, p}, context)) return false;
        need_value = false;
        any = true;
    }
}

// Page envelope. The venue's own pages disagree on the array key: the OpenAPI
// example for GET /data/orders is {"items": [...], "has_more": ...,
// "next_cursor": ...} while the api-reference pages for GET /data/trades show
// {"limit": ..., "count": ..., "data": [...]}. Both spellings are accepted;
// a payload carrying both at once is ambiguous and therefore malformed.
template <typename Visit, typename Context>
inline bool for_each_page_element(const char* json, size_t length,
                                  Context& context, Visit visit) noexcept {
    Span data;
    Span items;
    const bool has_data = json_field::unique(json, length, "data", data);
    const bool has_items = json_field::unique(json, length, "items", items);
    if (has_data == has_items) return false;  // neither, or both
    return for_each_array_element(json, length, has_data ? "data" : "items",
                                  context, visit);
}

// Visits each element of a top-level array of strings.
template <typename Visit, typename Context>
inline bool for_each_string_element(const char* json, size_t length,
                                    const char* key, Context& context,
                                    Visit visit) noexcept {
    struct Local {
        Visit& visit;
        Context& context;
    } local{visit, context};
    return for_each_array_element(
        json, length, key, local,
        [](const Span& span, Local& l) {
            return l.visit(span, l.context);
        });
}

// ── venue order ──────────────────────────────────────────────────────────────

inline constexpr size_t kVenueIdChars = 80;
inline constexpr size_t kMarketIdChars = 67;
inline constexpr size_t kAssetIdChars = 96;
inline constexpr size_t kOwnerChars = 64;
inline constexpr size_t kAddressChars = 43;
inline constexpr size_t kSmallFieldChars = 32;

struct VenueOrder {
    char id[kVenueIdChars]{};
    char market[kMarketIdChars]{};
    char asset_id[kAssetIdChars]{};
    char owner[kOwnerChars]{};
    char maker_address[kAddressChars]{};
    char order_type[8]{};
    char expiration[kSmallFieldChars]{};
    char outcome[kSmallFieldChars]{};
    VenueOrderStatus status = VenueOrderStatus::kUnknown;
    VenueSide side = VenueSide::kUnknown;
    uint64_t original_size_f6 = 0;
    uint64_t size_matched_f6 = 0;
    uint64_t price_f6 = 0;
    uint64_t created_at = 0;
    bool has_associate_trades = false;
};

// Parses one OpenOrder object. `json` must point at the object's first byte.
// Returns false (fail closed) on any missing/duplicated/malformed field.
inline bool parse_open_order(const char* json, size_t length,
                             VenueOrder& out) noexcept {
    if (!json || length == 0) return false;
    const char* begin = json;
    const char* end = json + length;
    while (begin < end && json_field::is_space(*begin)) ++begin;
    if (begin == end || *begin != '{') return false;
    const char* parsed_end = begin;
    if (!json_field::skip_value(parsed_end, end)) return false;
    while (parsed_end < end && json_field::is_space(*parsed_end)) ++parsed_end;
    if (parsed_end != end) return false;

    VenueOrder order;
    // Policy-critical fields: identity, ownership, market and sizes.
    if (!require_string(json, length, "id", order.id, sizeof(order.id)))
        return false;
    if (!require_string(json, length, "owner", order.owner,
                        sizeof(order.owner)))
        return false;
    optional_string_alias(json, length, "market", "condition_id", order.market,
                          sizeof(order.market));
    if (order.market[0] == '\0') return false;
    if (!require_fixed6(json, length, "original_size", order.original_size_f6))
        return false;
    if (!require_fixed6(json, length, "size_matched", order.size_matched_f6))
        return false;
    char text[kSmallFieldChars];
    if (!require_string(json, length, "status", text, sizeof(text)))
        return false;
    order.status = parse_venue_order_status(text, std::strlen(text));

    // Everything else is diagnostic or selector data: parsed when present,
    // tolerated when absent or in its older spelling, never a reason to
    // declare a page malformed.
    optional_string_alias(json, length, "asset_id", "token_id", order.asset_id,
                          sizeof(order.asset_id));
    optional_string(json, length, "maker_address", order.maker_address,
                    sizeof(order.maker_address));
    optional_string(json, length, "order_type", order.order_type,
                    sizeof(order.order_type));
    optional_string_alias(json, length, "expiration", "expires_at",
                          order.expiration, sizeof(order.expiration));
    optional_string(json, length, "outcome", order.outcome,
                    sizeof(order.outcome));
    optional_string(json, length, "side", text, sizeof(text));
    order.side = parse_venue_side(text, std::strlen(text));
    (void)json_field::fixed6(json, length, "price", order.price_f6);
    (void)require_u64_any(json, length, "created_at", order.created_at);
    Span ignored;
    bool duplicate = false;
    order.has_associate_trades =
        json_field::unique(json, length, "associate_trades", ignored,
                           &duplicate) ||
        duplicate;
    out = order;
    return true;
}

// ── envelopes ────────────────────────────────────────────────────────────────

inline constexpr size_t kCursorChars = 64;

// Reads the pagination cursor of a page. Returns false only when the field is
// present with a type that cannot be a cursor (a number, an object, a
// duplicate); `has_cursor` is false for an absent, `null` or empty cursor,
// which every published example uses for "this is the last page".
inline bool page_cursor(const char* json, size_t length, char* out, size_t cap,
                        bool& has_cursor) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    has_cursor = false;
    Span cursor;
    bool duplicate = false;
    if (!json_field::unique(json, length, "next_cursor", cursor, &duplicate))
        return !duplicate;  // absent is legal: older envelope
    const size_t n = cursor.size();
    if (n == 4 && std::memcmp(cursor.begin, "null", 4) == 0) return true;
    if (!json_field::span_is_string(cursor)) return false;
    char buffer[kCursorChars];
    if (!json_field::unescape(cursor, buffer, sizeof(buffer))) return false;
    if (buffer[0] == '\0') return true;
    if (std::strlen(buffer) + 1 > cap) return false;
    std::snprintf(out, cap, "%s", buffer);
    has_cursor = true;
    return true;
}

// Tri-state read of the envelope's `has_more`: `present` separates "absent"
// (the older envelope) from a literal `false`. A non-boolean value is an
// error, never a silent false.
inline bool has_more_field(const char* json, size_t length, bool& present,
                           bool& value) noexcept {
    present = false;
    value = false;
    Span span;
    bool duplicate = false;
    if (!json_field::unique(json, length, "has_more", span, &duplicate))
        return !duplicate;
    present = true;
    const size_t n = span.size();
    if (n == 4 && std::memcmp(span.begin, "true", 4) == 0) {
        value = true;
        return true;
    }
    if (n == 5 && std::memcmp(span.begin, "false", 5) == 0) return true;
    return false;
}

inline bool parse_closed_only(const char* json, size_t length,
                              bool& out) noexcept {
    return json_field::boolean(json, length, "closed_only", out);
}

// The error envelope is {error: string, code?: string, retry_after_seconds?}.
inline bool parse_error_message(const char* json, size_t length, char* out,
                                size_t cap) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    return json_field::string(json, length, "error", out, cap) &&
           out[0] != '\0';
}

// ── trades ───────────────────────────────────────────────────────────────────

inline constexpr size_t kTradeIdChars = 80;
inline constexpr size_t kMaxMakerOrders = 16;

struct VenueMakerOrder {
    char order_id[kVenueIdChars]{};
    char owner[kOwnerChars]{};
};

struct VenueTrade {
    char id[kTradeIdChars]{};
    char taker_order_id[kVenueIdChars]{};
    char market[kMarketIdChars]{};
    char asset_id[kAssetIdChars]{};
    char owner[kOwnerChars]{};
    TradeStatus status = TradeStatus::kUnknown;
    VenueSide side = VenueSide::kUnknown;
    // The REST trade schema documents `size` only as "Trade size"; its
    // published example is "100000000" for what the WebSocket stream reports
    // as 100 shares, i.e. raw 1e-6 units. Because the units are NOT documented
    // (unlike the order's size_matched, which explicitly says "do not divide
    // by 1e6"), the value is kept verbatim and never used in fill arithmetic:
    // fills come from the order's own cumulative size_matched.
    char size_text[kSmallFieldChars]{};
    uint64_t price_f6 = 0;
    uint64_t match_time = 0;
    // Attribution data: every referenced order id, capped. `truncated` means
    // the venue returned more makers than can be checked, which the reconciler
    // treats as "cannot attribute" rather than silently ignoring the rest.
    VenueMakerOrder makers[kMaxMakerOrders]{};
    uint32_t maker_orders = 0;         // declared elements on the wire
    uint32_t maker_orders_kept = 0;    // elements parsed into `makers`
    uint32_t maker_orders_ours = 0;    // kept elements owned by our API key
    bool maker_orders_truncated = false;
};

// Parses one Trade object of /data/trades. `owner`/`trader_side`/`maker_orders`
// are optional in the schema; the required subset is enforced strictly.
inline bool parse_trade(const char* json, size_t length, VenueTrade& out,
                        const char* our_owner) noexcept {
    VenueTrade trade;
    if (!require_string(json, length, "id", trade.id, sizeof(trade.id)))
        return false;
    if (!require_string(json, length, "taker_order_id", trade.taker_order_id,
                        sizeof(trade.taker_order_id)))
        return false;
    if (!require_string(json, length, "market", trade.market,
                        sizeof(trade.market)))
        return false;
    if (!require_string(json, length, "asset_id", trade.asset_id,
                        sizeof(trade.asset_id)))
        return false;
    char text[kSmallFieldChars];
    if (!require_string(json, length, "status", text, sizeof(text)))
        return false;
    trade.status = parse_trade_status(text, std::strlen(text));
    if (!require_string(json, length, "side", text, sizeof(text))) return false;
    trade.side = parse_venue_side(text, std::strlen(text));
    if (!require_string(json, length, "size", trade.size_text,
                        sizeof(trade.size_text)))
        return false;
    if (!require_fixed6(json, length, "price", trade.price_f6)) return false;
    // Documented as a string, but an integer works too (same discrepancy as
    // the order's created_at).
    (void)require_u64_any(json, length, "match_time", trade.match_time);
    optional_string(json, length, "owner", trade.owner, sizeof(trade.owner));

    struct Counter {
        const char* our_owner;
        VenueTrade* trade;
    } counter{our_owner, &trade};
    Span makers;
    if (json_field::unique(json, length, "maker_orders", makers)) {
        if (!for_each_array_element(
                json, length, "maker_orders", counter,
                [](const Span& element, Counter& c) {
                    if (c.trade->maker_orders >= kMaxMakerOrders) {
                        c.trade->maker_orders_truncated = true;
                        ++c.trade->maker_orders;
                        return true;
                    }
                    VenueMakerOrder maker;
                    if (!require_string(element.begin, element.size(),
                                        "order_id", maker.order_id,
                                        sizeof(maker.order_id)))
                        return false;
                    (void)json_field::string(element.begin, element.size(),
                                             "owner", maker.owner,
                                             sizeof(maker.owner));
                    c.trade->makers[c.trade->maker_orders_kept++] = maker;
                    ++c.trade->maker_orders;
                    if (c.our_owner && c.our_owner[0] && maker.owner[0] &&
                        std::strcmp(maker.owner, c.our_owner) == 0)
                        ++c.trade->maker_orders_ours;
                    return true;
                }))
            return false;
    }
    out = trade;
    return true;
}

// ── id comparison ────────────────────────────────────────────────────────────

// Venue ids are hex hashes ("0x…"): compare case-insensitively so a checksum
// variation never looks like a different order. Lengths must match exactly.
inline bool venue_id_equal(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    const size_t la = std::strlen(a);
    const size_t lb = std::strlen(b);
    if (la == 0 || la != lb) return false;
    for (size_t i = 0; i < la; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

}  // namespace clob_info

#endif  // CLOB_ORDER_INFO_HPP
