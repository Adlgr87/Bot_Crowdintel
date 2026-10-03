#ifndef USER_EVENT_HPP
#define USER_EVENT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// CLOB *user* channel: wire parsing and event application.
//
// Two independent layers, both free of I/O so they are testable with fixtures:
//   1. parse_user_message()  — bytes → typed events
//   2. UserEventApplier      — typed events → ledger events + state transitions
// The socket itself lives in user_ws_client.hpp and only moves bytes.
//
// Wire format (verified 2026-10-03):
//  * Raw CLOB user channel frames are flat objects discriminated by
//    `event_type` ∈ {"order","trade"}:
//      order → {event_type,id,owner,market,asset_id,side,order_owner,
//               original_size,size_matched,price,associate_trades,outcome,
//               type:PLACEMENT|UPDATE|CANCELLATION,created_at,expiration,
//               order_type,status,maker_address,timestamp}
//      trade → {event_type,type:TRADE,id,taker_order_id,market,asset_id,side,
//               size,fee_rate_bps,price,status:MATCHED|…,match_time,
//               last_update,outcome,owner,trade_owner,maker_address,
//               transaction_hash,bucket_index,maker_orders[],trader_side,
//               timestamp}
//    Source: https://docs.polymarket.com/trading/realtime-order-updates
//    and Polymarket/py-sdk src/polymarket/models/clob/user_events.py
//    (_normalize_to_envelope lifts exactly this flat shape).
//  * The unified SDK envelope {topic:"user",type:"order"|"trade",payload:{…}}
//    is also accepted, because py-sdk accepts both and a deployment may sit
//    behind the SDK's normaliser.
//  * Subscription frame (sent by us, not parsed here):
//      {"auth":{"apiKey":…,"secret":…,"passphrase":…},
//       "markets":["<condition_id>"],"type":"user"}
//    Omitting `markets` subscribes to the whole account.  Hot updates use
//    {"operation":"subscribe"|"unsubscribe","markets":[…]}.
//    Source: docs (above) + py-sdk streams/clob/user_protocol.py:35-56.
//  * `timestamp` is epoch **milliseconds**; `created_at`, `match_time`,
//    `last_update` and `expiration` are epoch **seconds**.
//  * Trade statuses matched_not_broadcasted / matched / mined / retrying are
//    non-terminal; confirmed and failed are terminal.
//  * The stream does NOT replay what was missed while disconnected: after a
//    reconnect the account state must be re-read over REST
//    (GET /data/orders, GET /data/trades) before trusting the local view.
//    That requirement is implemented in reconciliation.hpp.
// ─────────────────────────────────────────────────────────────────────────────

#include <strings.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/event_ledger.hpp"
#include "../include/json_scan.hpp"
#include "../include/order_state.hpp"
#include "../include/venue_metadata.hpp"

namespace user_ws {

// Side vocabulary shared by every CLOB payload ("BUY"/"SELL", uppercased by the
// venue; lowercase tolerated because it has been observed in fixtures).
inline bool parse_side(const char* text, uint8_t& side) noexcept {
    if (!text) return false;
    if (std::strcmp(text, "BUY") == 0 || std::strcmp(text, "buy") == 0 ||
        std::strcmp(text, "B") == 0) { side = 0; return true; }
    if (std::strcmp(text, "SELL") == 0 || std::strcmp(text, "sell") == 0 ||
        std::strcmp(text, "S") == 0) { side = 1; return true; }
    return false;
}

inline constexpr size_t K_MAX_EVENTS_PER_MESSAGE = 8;
inline constexpr size_t K_MAX_MAKER_ORDERS = 8;

enum class MessageKind : uint8_t {
    NONE = 0,
    ORDER,
    TRADE,
    HEARTBEAT_ACK,  // {} or PONG
    ERROR_FRAME,
    UNRECOGNIZED
};

inline const char* message_kind_name(MessageKind kind) noexcept {
    switch (kind) {
        case MessageKind::NONE: return "none";
        case MessageKind::ORDER: return "order";
        case MessageKind::TRADE: return "trade";
        case MessageKind::HEARTBEAT_ACK: return "heartbeat_ack";
        case MessageKind::ERROR_FRAME: return "error";
        case MessageKind::UNRECOGNIZED: return "unrecognized";
    }
    return "invalid";
}

struct MakerOrderWire {
    char order_id[80]{};
    char owner[80]{};
    char maker_address[43]{};
    char asset_id[80]{};
    char outcome[24]{};
    uint64_t matched_amount_raw = 0;
    uint64_t price_raw = 0;
    uint64_t fee_rate_bps = 0;
    uint8_t side = 0;
    int8_t outcome_index = -1;
};

struct OrderEventWire {
    char id[80]{};
    char market[70]{};          // condition id
    char asset_id[80]{};        // token id
    char owner[80]{};
    char order_owner[80]{};
    char maker_address[43]{};
    char outcome[24]{};
    char order_type[8]{};
    char status[24]{};
    char update_type[16]{};     // PLACEMENT | UPDATE | CANCELLATION
    uint8_t side = 0;
    bool side_valid = false;
    uint64_t price_raw = 0;
    uint64_t original_size_raw = 0;
    uint64_t size_matched_raw = 0;
    uint64_t created_at_s = 0;
    uint64_t expiration_s = 0;
    uint64_t timestamp_ms = 0;
    bool valid = false;
};

struct TradeEventWire {
    char id[80]{};
    char taker_order_id[80]{};
    char market[70]{};
    char asset_id[80]{};
    char owner[80]{};
    char trade_owner[80]{};
    char maker_address[43]{};
    char outcome[24]{};
    char transaction_hash[70]{};
    char status[40]{};
    char trader_side[8]{};
    uint8_t side = 0;
    bool side_valid = false;
    uint64_t size_raw = 0;
    uint64_t price_raw = 0;
    uint64_t fee_rate_bps = 0;
    uint64_t match_time_s = 0;
    uint64_t last_update_s = 0;
    uint64_t timestamp_ms = 0;
    int64_t bucket_index = -1;
    MakerOrderWire maker_orders[K_MAX_MAKER_ORDERS]{};
    size_t maker_order_count = 0;
    venue_status::TradeKind kind = venue_status::TradeKind::UNKNOWN;
    bool status_recognized = false;
    bool valid = false;
};

struct UserMessage {
    MessageKind kind = MessageKind::NONE;
    OrderEventWire orders[K_MAX_EVENTS_PER_MESSAGE]{};
    TradeEventWire trades[K_MAX_EVENTS_PER_MESSAGE]{};
    size_t order_count = 0;
    size_t trade_count = 0;
    char error_text[128]{};
};

// ── Parsing helpers ─────────────────────────────────────────────────────────
// Locates the object that carries the event fields: either the payload of an
// SDK envelope or the flat frame itself.
inline bool locate_payload(const char* data, size_t len, const char*& payload,
                           size_t& payload_len, char* type_out,
                           size_t type_cap) noexcept {
    payload = data;
    payload_len = len;
    type_out[0] = '\0';
    size_t start = 0;
    size_t end = 0;
    const bool has_payload_object = json_scan::find_object(data, len, "payload", start, end);
    char topic[16]{};
    const bool has_topic = json_scan::get_string(data, len, "topic", topic, sizeof(topic));
    char type[16]{};
    const bool has_type = json_scan::get_string(data, len, "type", type, sizeof(type));
    if (has_payload_object && has_topic && has_type) {
        payload = data + start;
        payload_len = end - start;
        std::snprintf(type_out, type_cap, "%s", type);
        return true;
    }
    // Flat wire shape: `event_type` discriminates, `type` carries the
    // PLACEMENT/UPDATE/CANCELLATION or TRADE detail.
    char event_type[16]{};
    if (!json_scan::get_string(data, len, "event_type", event_type, sizeof(event_type)))
        return false;
    if (json_scan::count_key(data, len, "event_type") > 1) return false;
    std::snprintf(type_out, type_cap, "%s", event_type);
    return true;
}

inline bool parse_order_payload(const char* data, size_t len,
                                OrderEventWire& out) noexcept {
    out = OrderEventWire{};
    if (!json_scan::get_string(data, len, "id", out.id, sizeof(out.id))) return false;
    if (!out.id[0]) return false;
    (void)json_scan::get_string(data, len, "market", out.market, sizeof(out.market));
    if (!json_scan::get_string(data, len, "asset_id", out.asset_id,
                               sizeof(out.asset_id))) {
        if (!json_scan::get_string(data, len, "token_id", out.asset_id,
                                   sizeof(out.asset_id)))
            return false;
    }
    if (!json_scan::is_decimal_integer(out.asset_id, std::strlen(out.asset_id)))
        return false;
    if (out.market[0] &&
        !json_scan::is_hex_bytes(out.market, std::strlen(out.market), 32))
        return false;
    (void)json_scan::get_string(data, len, "owner", out.owner, sizeof(out.owner));
    (void)json_scan::get_string(data, len, "order_owner", out.order_owner,
                                sizeof(out.order_owner));
    (void)json_scan::get_string(data, len, "maker_address", out.maker_address,
                                sizeof(out.maker_address));
    (void)json_scan::get_string(data, len, "outcome", out.outcome, sizeof(out.outcome));
    (void)json_scan::get_string(data, len, "order_type", out.order_type,
                                sizeof(out.order_type));
    (void)json_scan::get_string(data, len, "status", out.status, sizeof(out.status));
    (void)json_scan::get_string(data, len, "type", out.update_type,
                                sizeof(out.update_type));
    char side[8]{};
    if (json_scan::get_string(data, len, "side", side, sizeof(side)))
        out.side_valid = parse_side(side, out.side);
    if (!out.side_valid) return false;
    if (!json_scan::get_fixed(data, len, "price", 1000000, out.price_raw)) return false;
    if (!json_scan::get_fixed(data, len, "original_size", 1000000,
                              out.original_size_raw))
        return false;
    if (!json_scan::get_fixed(data, len, "size_matched", 1000000,
                              out.size_matched_raw))
        out.size_matched_raw = 0;
    if (out.size_matched_raw > out.original_size_raw) return false;
    (void)json_scan::get_u64(data, len, "created_at", out.created_at_s);
    if (!out.created_at_s) {
        char text[24]{};
        if (json_scan::get_string(data, len, "created_at", text, sizeof(text)))
            (void)json_scan::parse_fixed(text, std::strlen(text), 1, out.created_at_s);
    }
    if (!json_scan::get_u64(data, len, "expiration", out.expiration_s)) {
        char text[24]{};
        if (json_scan::get_string(data, len, "expiration", text, sizeof(text)))
            (void)json_scan::parse_fixed(text, std::strlen(text), 1, out.expiration_s);
    }
    if (!json_scan::get_u64(data, len, "timestamp", out.timestamp_ms)) {
        char text[32]{};
        if (json_scan::get_string(data, len, "timestamp", text, sizeof(text)))
            (void)json_scan::parse_fixed(text, std::strlen(text), 1, out.timestamp_ms);
    }
    out.valid = true;
    return true;
}

inline bool parse_trade_payload(const char* data, size_t len,
                                TradeEventWire& out) noexcept {
    out = TradeEventWire{};
    if (!json_scan::get_string(data, len, "id", out.id, sizeof(out.id))) return false;
    if (!out.id[0]) return false;
    (void)json_scan::get_string(data, len, "taker_order_id", out.taker_order_id,
                                sizeof(out.taker_order_id));
    (void)json_scan::get_string(data, len, "market", out.market, sizeof(out.market));
    if (!json_scan::get_string(data, len, "asset_id", out.asset_id,
                               sizeof(out.asset_id))) {
        if (!json_scan::get_string(data, len, "token_id", out.asset_id,
                                   sizeof(out.asset_id)))
            return false;
    }
    if (!json_scan::is_decimal_integer(out.asset_id, std::strlen(out.asset_id)))
        return false;
    (void)json_scan::get_string(data, len, "owner", out.owner, sizeof(out.owner));
    (void)json_scan::get_string(data, len, "trade_owner", out.trade_owner,
                                sizeof(out.trade_owner));
    (void)json_scan::get_string(data, len, "maker_address", out.maker_address,
                                sizeof(out.maker_address));
    (void)json_scan::get_string(data, len, "outcome", out.outcome, sizeof(out.outcome));
    (void)json_scan::get_string(data, len, "transaction_hash", out.transaction_hash,
                                sizeof(out.transaction_hash));
    (void)json_scan::get_string(data, len, "trader_side", out.trader_side,
                                sizeof(out.trader_side));
    if (!json_scan::get_string(data, len, "status", out.status, sizeof(out.status)))
        return false;
    out.kind = venue_status::trade_kind(out.status);
    out.status_recognized = out.kind != venue_status::TradeKind::UNKNOWN;
    char side[8]{};
    if (!json_scan::get_string(data, len, "side", side, sizeof(side))) return false;
    if (!parse_side(side, out.side)) return false;
    out.side_valid = true;
    if (!json_scan::get_fixed(data, len, "size", 1000000, out.size_raw)) return false;
    if (!json_scan::get_fixed(data, len, "price", 1000000, out.price_raw)) return false;
    (void)json_scan::get_u64(data, len, "fee_rate_bps", out.fee_rate_bps);
    (void)json_scan::get_u64(data, len, "match_time", out.match_time_s);
    (void)json_scan::get_u64(data, len, "last_update", out.last_update_s);
    (void)json_scan::get_u64(data, len, "timestamp", out.timestamp_ms);
    int64_t bucket = 0;
    if (json_scan::get_i64(data, len, "bucket_index", bucket)) out.bucket_index = bucket;

    size_t start = 0;
    size_t end = 0;
    if (json_scan::find_array(data, len, "maker_orders", start, end)) {
        const size_t total = json_scan::array_count(data, len, start, end);
        for (size_t i = 0; i < total && out.maker_order_count < K_MAX_MAKER_ORDERS; ++i) {
            size_t es = 0;
            size_t ee = 0;
            if (!json_scan::array_element(data, len, start, end, i, es, ee)) break;
            MakerOrderWire& maker = out.maker_orders[out.maker_order_count];
            if (!json_scan::get_string(data + es, ee - es, "order_id", maker.order_id,
                                       sizeof(maker.order_id)))
                break;
            (void)json_scan::get_string(data + es, ee - es, "owner", maker.owner,
                                        sizeof(maker.owner));
            (void)json_scan::get_string(data + es, ee - es, "maker_address",
                                        maker.maker_address, sizeof(maker.maker_address));
            (void)json_scan::get_string(data + es, ee - es, "asset_id", maker.asset_id,
                                        sizeof(maker.asset_id));
            (void)json_scan::get_string(data + es, ee - es, "outcome", maker.outcome,
                                        sizeof(maker.outcome));
            (void)json_scan::get_fixed(data + es, ee - es, "matched_amount", 1000000,
                                       maker.matched_amount_raw);
            (void)json_scan::get_fixed(data + es, ee - es, "price", 1000000,
                                       maker.price_raw);
            (void)json_scan::get_u64(data + es, ee - es, "fee_rate_bps",
                                     maker.fee_rate_bps);
            char maker_side[8]{};
            if (json_scan::get_string(data + es, ee - es, "side", maker_side,
                                      sizeof(maker_side)))
                (void)parse_side(maker_side, maker.side);
            int64_t index = -1;
            if (json_scan::get_i64(data + es, ee - es, "outcome_index", index))
                maker.outcome_index = static_cast<int8_t>(index);
            ++out.maker_order_count;
        }
    }
    out.valid = true;
    return true;
}

// Parses one frame.  A frame may be a single object or an array of events.
inline bool parse_user_message(const char* data, size_t len,
                               UserMessage& out) noexcept {
    out = UserMessage{};
    if (!data || len == 0) return false;
    size_t begin = 0;
    while (begin < len && json_scan::is_space(data[begin])) ++begin;
    size_t end = len;
    while (end > begin && json_scan::is_space(data[end - 1])) --end;
    if (begin >= end) return false;

    // Application-level heartbeat: the server answers PING with PONG and may
    // send an empty object as a keep-alive.
    if (end - begin == 4 && std::memcmp(data + begin, "PONG", 4) == 0) {
        out.kind = MessageKind::HEARTBEAT_ACK;
        return true;
    }
    if (end - begin == 2 && data[begin] == '{' && data[begin + 1] == '}') {
        out.kind = MessageKind::HEARTBEAT_ACK;
        return true;
    }

    if (data[begin] == '[') {
        const size_t after = json_scan::skip_value(data, end, begin);
        if (!after) return false;
        const size_t total = json_scan::array_count(data, end, begin, after);
        for (size_t i = 0; i < total; ++i) {
            size_t es = 0;
            size_t ee = 0;
            if (!json_scan::array_element(data, end, begin, after, i, es, ee))
                return false;
            UserMessage element{};
            if (!parse_user_message(data + es, ee - es, element)) return false;
            if (element.kind == MessageKind::HEARTBEAT_ACK) continue;
            if (element.order_count + out.order_count > K_MAX_EVENTS_PER_MESSAGE ||
                element.trade_count + out.trade_count > K_MAX_EVENTS_PER_MESSAGE)
                return false;
            for (size_t k = 0; k < element.order_count; ++k)
                out.orders[out.order_count++] = element.orders[k];
            for (size_t k = 0; k < element.trade_count; ++k)
                out.trades[out.trade_count++] = element.trades[k];
            if (out.kind == MessageKind::NONE) out.kind = element.kind;
        }
        return out.order_count != 0 || out.trade_count != 0;
    }

    if (data[begin] != '{') {
        out.kind = MessageKind::UNRECOGNIZED;
        return false;
    }
    const char* frame = data + begin;
    const size_t frame_len = end - begin;
    const char* payload = frame;
    size_t payload_len = frame_len;
    char type[16]{};
    if (!locate_payload(frame, frame_len, payload, payload_len, type, sizeof(type))) {
        // An object without event_type/type is not an event we can act on.
        char message[64]{};
        if (json_scan::get_string(payload, payload_len, "message", message,
                                  sizeof(message)) ||
            json_scan::get_string(payload, payload_len, "error", message,
                                  sizeof(message))) {
            out.kind = MessageKind::ERROR_FRAME;
            std::snprintf(out.error_text, sizeof(out.error_text), "%s", message);
            return true;
        }
        out.kind = MessageKind::UNRECOGNIZED;
        return false;
    }
    if (std::strcmp(type, "order") == 0) {
        if (!parse_order_payload(payload, payload_len, out.orders[0])) return false;
        out.order_count = 1;
        out.kind = MessageKind::ORDER;
        return true;
    }
    if (std::strcmp(type, "trade") == 0) {
        if (!parse_trade_payload(payload, payload_len, out.trades[0])) return false;
        out.trade_count = 1;
        out.kind = MessageKind::TRADE;
        return true;
    }
    out.kind = MessageKind::UNRECOGNIZED;
    return false;
}

// ── Application to the ledger ───────────────────────────────────────────────
struct ApplierContext {
    char api_owner[80]{};        // our CLOB API key (the `owner` field)
    char maker_address[43]{};    // our signer/proxy address
    char condition_id[70]{};     // market we are subscribed to
    char token_id[80]{};         // token we trade
};

struct ApplyOutcome {
    bool applied = false;
    bool duplicate = false;
    bool adopted_external = false;   // order we never sent appeared
    bool needs_rest_confirmation = false;
    bool divergence = false;         // local and venue views disagree
    bool heartbeat_cancellation = false;
    bool illegal_transition = false;
    bool ledger_failure = false;     // durability problem → fail closed
    char order_id[80]{};
    char trade_id[80]{};
    char reason[96]{};
    OrderState new_state = OrderState::NONE;
};

class UserEventApplier {
public:
    explicit UserEventApplier(ledger::EventLedger& ledger) : ledger_(ledger) {}

    // Applies one order event.  Returns false when the ledger could not durably
    // record the observation; the caller must then stop trading.
    bool apply_order(const OrderEventWire& wire, const ApplierContext& context,
                     ApplyOutcome& outcome) {
        outcome = ApplyOutcome{};
        // The whole read-modify-write sequence is atomic with respect to the
        // other ledger writers (gateway, supervisor).  No I/O happens here.
        auto lock = ledger_.guard();
        if (!wire.valid) return false;
        std::snprintf(outcome.order_id, sizeof(outcome.order_id), "%s", wire.id);

        // Market scoping: an event for another market is not ours to apply.
        if (context.condition_id[0] && wire.market[0] &&
            strcasecmp(wire.market, context.condition_id) != 0) {
            std::snprintf(outcome.reason, sizeof(outcome.reason),
                          "event_for_other_market");
            outcome.divergence = true;
            return true;  // recorded as an observation, not applied to state
        }

        const ledger::OrderRecord* existing = ledger_.orders().find(wire.id);
        const bool known = existing != nullptr && existing->order_id[0] != '\0';
        const venue_status::OrderEventKind kind =
            venue_status::order_event_kind(wire.update_type);
        if (kind == venue_status::OrderEventKind::UNKNOWN) {
            std::snprintf(outcome.reason, sizeof(outcome.reason),
                          "unrecognized_order_event_type");
            outcome.needs_rest_confirmation = true;
            return record_state_event(wire.id, outcome.reason, wire.timestamp_ms,
                                      outcome);
        }

        bool status_recognized = false;
        const bool fully_matched = wire.original_size_raw != 0 &&
                                   wire.size_matched_raw >= wire.original_size_raw;
        OrderState target = venue_status::from_order_status(
            wire.status[0] ? wire.status : nullptr, fully_matched, status_recognized);
        if (!status_recognized) {
            // Fall back to the event type when the status string is unknown.
            switch (kind) {
                case venue_status::OrderEventKind::PLACEMENT:
                    target = OrderState::LIVE;
                    break;
                case venue_status::OrderEventKind::UPDATE:
                    target = fully_matched ? OrderState::MATCHED
                                           : OrderState::PARTIALLY_FILLED;
                    break;
                case venue_status::OrderEventKind::CANCELLATION:
                    target = OrderState::CANCELLED;
                    break;
                case venue_status::OrderEventKind::UNKNOWN:
                default:
                    target = OrderState::UNKNOWN;
                    break;
            }
            std::snprintf(outcome.reason, sizeof(outcome.reason),
                          "unrecognized_status_string");
            outcome.needs_rest_confirmation = true;
        }
        if (wire.expiration_s != 0 && target == OrderState::LIVE) {
            // GTD orders that are past expiry cannot still be working.
            const uint64_t now_s = static_cast<uint64_t>(ledger::now_wall_ns() / 1000000000ULL);
            if (wire.expiration_s <= now_s) target = OrderState::EXPIRED;
        }

        if (!known) {
            // An order we have no local record of.  Either our POST result was
            // lost (ambiguous transport) or another session placed it.  Adopt
            // it, mark it externally observed and require a REST confirmation
            // before it may be treated as ours.
            outcome.adopted_external = !is_ours(wire, context);
            outcome.needs_rest_confirmation = true;
            std::snprintf(outcome.reason, sizeof(outcome.reason),
                          outcome.adopted_external ? "external_order_observed"
                                                   : "unacked_local_order");
            ledger::Event event{};
            // VENUE_ORDER_ADOPTED marks the record as externally observed; our
            // own unacknowledged order is recorded as a plain state change so
            // it is never mistaken for someone else's exposure.
            event.type = outcome.adopted_external
                             ? ledger::EventType::VENUE_ORDER_ADOPTED
                             : ledger::EventType::ORDER_STATE;
            event.source = ledger::Source::USER_WS;
            event.wall_ns = ledger::now_wall_ns();
            event.venue_ts_ms = wire.timestamp_ms;
            add_order_fields(event, wire);
            event.add_u8(ledger::F_STATE, static_cast<uint8_t>(target));
            event.add_str(ledger::F_STATUS_TEXT, wire.status);
            event.add_str(ledger::F_REASON, outcome.reason);
            // The key domain is the *observation* (an order event on the user
            // channel), not the ledger record type: the same observation must
            // collapse onto one key whether it is recorded as an adoption or as
            // a state change, otherwise a redelivery would be applied twice.
            ledger::compute_event_key(ledger::EventType::ORDER_STATE, event.source,
                                      wire.id, "", wire.timestamp_ms,
                                      dedup_salt(wire), event.key);
            if (!commit(event, outcome)) return false;
            outcome.new_state = target;
            outcome.applied = true;
            return true;
        }

        // Cancellations we did not request are a divergence: the venue may have
        // killed the order because the heartbeat chain lapsed.
        if (kind == venue_status::OrderEventKind::CANCELLATION) {
            const bool we_cancelled = existing->state == OrderState::CANCELLED;
            if (!we_cancelled && ledger_.heartbeat().invalidated) {
                outcome.heartbeat_cancellation = true;
                std::snprintf(outcome.reason, sizeof(outcome.reason),
                              "cancellation_after_heartbeat_invalid");
            } else if (!we_cancelled && existing->state != OrderState::UNKNOWN) {
                outcome.divergence = true;
                std::snprintf(outcome.reason, sizeof(outcome.reason),
                              "unrequested_cancellation");
            }
        }

        ledger::Event event{};
        event.type = ledger::EventType::ORDER_STATE;
        event.source = ledger::Source::USER_WS;
        event.wall_ns = ledger::now_wall_ns();
        event.venue_ts_ms = wire.timestamp_ms;
        add_order_fields(event, wire);
        event.add_u8(ledger::F_STATE, static_cast<uint8_t>(target));
        event.add_str(ledger::F_STATUS_TEXT, wire.status);
        if (outcome.reason[0]) event.add_str(ledger::F_REASON, outcome.reason);
        const uint64_t before = ledger_.illegal_transitions();
        ledger::compute_event_key(ledger::EventType::ORDER_STATE, event.source,
                                  wire.id, "", wire.timestamp_ms,
                                  dedup_salt(wire), event.key);
        if (!commit(event, outcome)) return false;
        outcome.illegal_transition = ledger_.illegal_transitions() != before;
        const ledger::OrderRecord* after = ledger_.orders().find(wire.id);
        if (after) outcome.new_state = after->state;
        outcome.applied = true;
        // A non-terminal observation always needs an authoritative read before
        // the account is declared READY.
        if (!order_state_is_terminal(target)) outcome.needs_rest_confirmation = true;
        return true;
    }

    // Applies one trade event: records the fill and moves the order's matched
    // size.  Terminal statuses (CONFIRMED/FAILED) settle the observation;
    // non-terminal ones flag it for REST confirmation.
    bool apply_trade(const TradeEventWire& wire, const ApplierContext& context,
                     ApplyOutcome& outcome) {
        outcome = ApplyOutcome{};
        auto lock = ledger_.guard();
        if (!wire.valid) return false;
        std::snprintf(outcome.trade_id, sizeof(outcome.trade_id), "%s", wire.id);
        if (context.condition_id[0] && wire.market[0] &&
            strcasecmp(wire.market, context.condition_id) != 0) {
            std::snprintf(outcome.reason, sizeof(outcome.reason),
                          "event_for_other_market");
            outcome.divergence = true;
            return true;
        }
        // Our own order id: for a taker fill it is `taker_order_id`; when we
        // rested, the maker_orders array carries it.
        const char* order_id = wire.taker_order_id[0] ? wire.taker_order_id : "";
        for (size_t i = 0; i < wire.maker_order_count; ++i) {
            if (is_ours_address(wire.maker_orders[i].maker_address, context) ||
                (context.api_owner[0] &&
                 std::strcmp(wire.maker_orders[i].owner, context.api_owner) == 0)) {
                order_id = wire.maker_orders[i].order_id;
                break;
            }
        }
        if (is_ours_address(wire.maker_address, context) ||
            (context.api_owner[0] &&
             (std::strcmp(wire.owner, context.api_owner) == 0 ||
              std::strcmp(wire.trade_owner, context.api_owner) == 0))) {
            if (!order_id[0]) order_id = wire.taker_order_id;
        }

        ledger::Event event{};
        event.type = ledger::EventType::FILL;
        event.source = ledger::Source::USER_WS;
        event.wall_ns = ledger::now_wall_ns();
        event.venue_ts_ms = wire.timestamp_ms ? wire.timestamp_ms
                                              : wire.match_time_s * 1000ULL;
        event.add_str(ledger::F_TRADE_ID, wire.id);
        if (order_id[0]) event.add_str(ledger::F_ORDER_ID, order_id);
        event.add_str(ledger::F_TOKEN_ID, wire.asset_id);
        if (wire.market[0]) event.add_str(ledger::F_CONDITION_ID, wire.market);
        event.add_u8(ledger::F_SIDE, wire.side);
        event.add_u8(ledger::F_RESULT, static_cast<uint8_t>(wire.kind));
        event.add_u64(ledger::F_PRICE_RAW, wire.price_raw);
        event.add_u64(ledger::F_SIZE_RAW, wire.size_raw);
        event.add_u64(ledger::F_FEE_BPS, wire.fee_rate_bps);
        event.add_str(ledger::F_STATUS_TEXT, wire.status);
        if (!wire.status_recognized) {
            std::snprintf(outcome.reason, sizeof(outcome.reason),
                          "unrecognized_trade_status");
            event.add_str(ledger::F_REASON, outcome.reason);
            outcome.needs_rest_confirmation = true;
        }
        ledger::compute_event_key(ledger::EventType::FILL, event.source, order_id,
                                  wire.id, event.venue_ts_ms, 0, event.key);
        if (!commit(event, outcome)) return false;
        outcome.applied = true;
        std::snprintf(outcome.order_id, sizeof(outcome.order_id), "%s", order_id);

        if (!venue_status::trade_is_terminal(wire.kind))
            outcome.needs_rest_confirmation = true;
        if (wire.kind == venue_status::TradeKind::FAILED) {
            // A failed settlement means our inventory view may be wrong.
            outcome.divergence = true;
            std::snprintf(outcome.reason, sizeof(outcome.reason),
                          "trade_settlement_failed");
        }
        if (order_id[0]) {
            const ledger::OrderRecord* order = ledger_.orders().find(order_id);
            if (!order) {
                // A fill for an order we never saw: adopt it as unknown and
                // require reconciliation.
                outcome.adopted_external = true;
                outcome.needs_rest_confirmation = true;
                ledger::Event adopt{};
                adopt.type = ledger::EventType::VENUE_ORDER_ADOPTED;
                adopt.source = ledger::Source::USER_WS;
                adopt.wall_ns = ledger::now_wall_ns();
                adopt.venue_ts_ms = event.venue_ts_ms;
                adopt.add_str(ledger::F_ORDER_ID, order_id);
                adopt.add_str(ledger::F_TOKEN_ID, wire.asset_id);
                if (wire.market[0]) adopt.add_str(ledger::F_CONDITION_ID, wire.market);
                adopt.add_u8(ledger::F_SIDE, wire.side);
                adopt.add_u64(ledger::F_PRICE_RAW, wire.price_raw);
                adopt.add_u64(ledger::F_SIZE_RAW, wire.size_raw);
                adopt.add_u64(ledger::F_MATCHED_RAW, wire.size_raw);
                adopt.add_u8(ledger::F_STATE,
                             static_cast<uint8_t>(OrderState::PARTIALLY_FILLED));
                adopt.add_str(ledger::F_REASON, "fill_without_order_record");
                ledger::compute_event_key(ledger::EventType::ORDER_STATE,
                                          adopt.source, order_id, "",
                                          event.venue_ts_ms, 1, adopt.key);
                if (!commit(adopt, outcome)) return false;
            } else {
                // Keep the order's matched size monotonic.
                const uint64_t matched = order->matched_size;
                if (matched == 0) {
                    ledger::Event update{};
                    update.type = ledger::EventType::ORDER_STATE;
                    update.source = ledger::Source::USER_WS;
                    update.wall_ns = ledger::now_wall_ns();
                    update.venue_ts_ms = event.venue_ts_ms;
                    update.add_str(ledger::F_ORDER_ID, order_id);
                    update.add_str(ledger::F_TOKEN_ID, wire.asset_id);
                    update.add_u64(ledger::F_MATCHED_RAW, wire.size_raw);
                    ledger::compute_event_key(ledger::EventType::ORDER_STATE,
                                              update.source, order_id, wire.id,
                                              event.venue_ts_ms, 2, update.key);
                    if (!commit(update, outcome)) return false;
                }
            }
        }
        return true;
    }

private:
    static bool is_ours_address(const char* address,
                                const ApplierContext& context) noexcept {
        return context.maker_address[0] && address && address[0] &&
               strcasecmp(address, context.maker_address) == 0;
    }

    static bool is_ours(const OrderEventWire& wire,
                        const ApplierContext& context) noexcept {
        if (context.api_owner[0] && wire.owner[0] &&
            std::strcmp(wire.owner, context.api_owner) == 0)
            return true;
        if (context.api_owner[0] && wire.order_owner[0] &&
            std::strcmp(wire.order_owner, context.api_owner) == 0)
            return true;
        return is_ours_address(wire.maker_address, context);
    }

    // Salt keeps repeated observations of the same order distinguishable when
    // the venue reuses a timestamp, while remaining independent of arrival
    // order (status + update type + matched size identify the observation).
    static uint64_t dedup_salt(const OrderEventWire& wire) noexcept {
        uint64_t salt = wire.size_matched_raw;
        salt = salt * 31 + static_cast<uint64_t>(wire.update_type[0]);
        salt = salt * 31 + static_cast<uint64_t>(wire.status[0]);
        return salt;
    }

    static void add_order_fields(ledger::Event& event, const OrderEventWire& wire) {
        event.add_str(ledger::F_ORDER_ID, wire.id);
        if (wire.asset_id[0]) event.add_str(ledger::F_TOKEN_ID, wire.asset_id);
        if (wire.market[0]) event.add_str(ledger::F_CONDITION_ID, wire.market);
        event.add_u8(ledger::F_SIDE, wire.side);
        event.add_u64(ledger::F_PRICE_RAW, wire.price_raw);
        event.add_u64(ledger::F_SIZE_RAW, wire.original_size_raw);
        event.add_u64(ledger::F_MATCHED_RAW, wire.size_matched_raw);
        if (wire.order_type[0]) event.add_str(ledger::F_ORDER_TYPE, wire.order_type);
    }

    // Caller holds ledger_.guard().
    bool commit(ledger::Event& event, ApplyOutcome& outcome) {
        const uint64_t duplicates_before = ledger_.duplicates_ignored();
        char error[128]{};
        if (!ledger_.commit_locked(event, error, sizeof(error))) {
            outcome.ledger_failure = true;
            if (!outcome.reason[0])
                std::snprintf(outcome.reason, sizeof(outcome.reason), "%.95s", error);
            return false;
        }
        outcome.duplicate = ledger_.duplicates_ignored() != duplicates_before;
        return true;
    }

    bool record_state_event(const char* order_id, const char* reason,
                            uint64_t venue_ts_ms, ApplyOutcome& outcome) {
        ledger::Event event{};
        event.type = ledger::EventType::STATE_EVENT;
        event.source = ledger::Source::USER_WS;
        event.wall_ns = ledger::now_wall_ns();
        event.venue_ts_ms = venue_ts_ms;
        if (order_id && order_id[0]) event.add_str(ledger::F_ORDER_ID, order_id);
        event.add_str(ledger::F_REASON, reason);
        ledger::compute_event_key(event.type, event.source, order_id, "", venue_ts_ms,
                                  ledger::fnv1a(reason, std::strlen(reason)), event.key);
        return commit(event, outcome);
    }

    ledger::EventLedger& ledger_;
};

}  // namespace user_ws

#endif  // USER_EVENT_HPP
