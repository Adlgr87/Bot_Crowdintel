#ifndef ORDER_STATE_HPP
#define ORDER_STATE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Order state machine (fail-closed).
//
// The venue's own vocabulary is *not* our state machine.  Polymarket reports
// order statuses (LIVE, MATCHED, DELAYED, UNMATCHED, CANCELED) and order event
// types (PLACEMENT, UPDATE, CANCELLATION) plus trade statuses
// (TRADE_STATUS_*).  Those observations are mapped onto an explicit local
// lifecycle that additionally contains the mandatory UNKNOWN state:
//
//   LOCAL_CREATED → SIGNED → SUBMITTING → {LIVE | PARTIALLY_FILLED | MATCHED
//                                          | REJECTED | UNKNOWN | FAILED}
//
// UNKNOWN means "we sent something and cannot prove what the venue did with
// it".  It is entered on ambiguous transport results and on divergent
// observations, and it blocks new orders for that account/market until an
// authoritative read (GET /data/order/{id}, GET /data/orders, GET /data/trades,
// balances) resolves it.  If it cannot be resolved, trading stays disabled.
//
// Terminal states: MATCHED, CANCELLED, EXPIRED, REJECTED, FAILED.  No
// transition leaves a terminal state; attempts are rejected and recorded.
//
// Sources for the venue vocabulary (verified 2026-10-03):
//   * https://docs.polymarket.com/trading/realtime-order-updates
//     (order: type PLACEMENT|UPDATE|CANCELLATION,
//      status LIVE|MATCHED|DELAYED|UNMATCHED|CANCELED;
//      trade: status TRADE_STATUS_MATCHED|_MATCHED_NOT_BROADCASTED|_MINED|
//                    _CONFIRMED|_RETRYING|_FAILED)
//   * Polymarket/py-sdk src/polymarket/models/clob/user_events.py
//     (_OrderEventType, _OrderStatus, TradeStatus)
//   * Polymarket/py-sdk src/polymarket/models/clob/order_response.py
//     (OrderPostStatus = live|matched|delayed)
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstring>

enum class OrderState : uint8_t {
    NONE = 0,          // no local record
    LOCAL_CREATED,     // intent built locally, not signed
    SIGNED,            // EIP-712 signature produced, not handed to transport
    SUBMITTING,        // handed to transport; outcome not yet known
    UNKNOWN,           // ambiguous outcome — blocks new orders until resolved
    LIVE,              // venue accepted and resting (or delayed)
    PARTIALLY_FILLED,  // venue accepted, some shares matched
    MATCHED,           // terminal: fully matched
    CANCELLED,         // terminal: cancelled by us, by the venue, or by heartbeat expiry
    EXPIRED,           // terminal: GTD expiry
    REJECTED,          // terminal: venue rejected the order
    FAILED,            // terminal: local/transport failure, never reached the venue
    SUPERSEDED,        // terminal: local submission ticket replaced by the venue record
    COUNT
};

inline const char* order_state_name(OrderState state) noexcept {
    switch (state) {
        case OrderState::NONE: return "NONE";
        case OrderState::LOCAL_CREATED: return "LOCAL_CREATED";
        case OrderState::SIGNED: return "SIGNED";
        case OrderState::SUBMITTING: return "SUBMITTING";
        case OrderState::UNKNOWN: return "UNKNOWN";
        case OrderState::LIVE: return "LIVE";
        case OrderState::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderState::MATCHED: return "MATCHED";
        case OrderState::CANCELLED: return "CANCELLED";
        case OrderState::EXPIRED: return "EXPIRED";
        case OrderState::REJECTED: return "REJECTED";
        case OrderState::FAILED: return "FAILED";
        case OrderState::SUPERSEDED: return "SUPERSEDED";
        case OrderState::COUNT: break;
    }
    return "INVALID";
}

inline bool order_state_is_terminal(OrderState state) noexcept {
    return state == OrderState::MATCHED || state == OrderState::CANCELLED ||
           state == OrderState::EXPIRED || state == OrderState::REJECTED ||
           state == OrderState::FAILED || state == OrderState::SUPERSEDED;
}

// A state that still represents open venue exposure.  UNKNOWN counts as open:
// it may be a resting order we cannot see, so inventory/exposure accounting
// must stay conservative.
inline bool order_state_is_open(OrderState state) noexcept {
    return state == OrderState::SUBMITTING || state == OrderState::UNKNOWN ||
           state == OrderState::LIVE || state == OrderState::PARTIALLY_FILLED;
}

inline bool order_state_blocks_new_orders(OrderState state) noexcept {
    return state == OrderState::UNKNOWN || state == OrderState::SUBMITTING;
}

namespace order_state {

// Explicit transition table.  Rows are indexed by the current state; a bit is
// set when the target state may be entered.  Everything else is illegal.
inline constexpr uint32_t bit(OrderState state) noexcept {
    return state == OrderState::NONE || state == OrderState::COUNT
               ? 0U : (1U << static_cast<unsigned>(state));
}

inline constexpr uint32_t mask(OrderState a,
                               OrderState b = OrderState::NONE,
                               OrderState c = OrderState::NONE,
                               OrderState d = OrderState::NONE,
                               OrderState e = OrderState::NONE,
                               OrderState f = OrderState::NONE,
                               OrderState g = OrderState::NONE,
                               OrderState h = OrderState::NONE,
                               OrderState i = OrderState::NONE) noexcept {
    return bit(a) | bit(b) | bit(c) | bit(d) | bit(e) | bit(f) | bit(g) | bit(h) |
           bit(i);
}

// Self-transitions are allowed for the non-terminal, observably-repeatable
// states (a second UPDATE, another heartbeat of the same LIVE state, etc.).
inline constexpr uint32_t K_ALLOWED[static_cast<size_t>(OrderState::COUNT)] = {
    /* NONE            */ mask(OrderState::LOCAL_CREATED, OrderState::SIGNED,
                               OrderState::SUBMITTING, OrderState::LIVE,
                               OrderState::PARTIALLY_FILLED, OrderState::MATCHED,
                               OrderState::CANCELLED),
    /* LOCAL_CREATED   */ mask(OrderState::SIGNED, OrderState::FAILED,
                               OrderState::CANCELLED, OrderState::REJECTED,
                               OrderState::LOCAL_CREATED),
    /* SIGNED          */ mask(OrderState::SUBMITTING, OrderState::FAILED,
                               OrderState::CANCELLED, OrderState::SIGNED),
    /* SUBMITTING      */ mask(OrderState::LIVE, OrderState::PARTIALLY_FILLED,
                               OrderState::MATCHED, OrderState::REJECTED,
                               OrderState::UNKNOWN, OrderState::FAILED,
                               OrderState::SUPERSEDED),
    /* UNKNOWN         */ mask(OrderState::LIVE, OrderState::PARTIALLY_FILLED,
                               OrderState::MATCHED, OrderState::CANCELLED,
                               OrderState::EXPIRED, OrderState::REJECTED,
                               OrderState::FAILED, OrderState::SUPERSEDED),
    /* LIVE            */ mask(OrderState::PARTIALLY_FILLED, OrderState::MATCHED,
                               OrderState::CANCELLED, OrderState::EXPIRED,
                               OrderState::UNKNOWN, OrderState::LIVE,
                               // Only an authoritative "no such order" read
                               // (GET /data/order/{id} → 404) may take an order
                               // we believed was resting to FAILED: the venue
                               // never had it, so there is no exposure.
                               OrderState::FAILED),
    /* PARTIALLY_FILLED*/ mask(OrderState::MATCHED, OrderState::CANCELLED,
                               OrderState::EXPIRED, OrderState::UNKNOWN,
                               OrderState::PARTIALLY_FILLED),
    /* MATCHED         */ mask(OrderState::MATCHED),
    /* CANCELLED       */ mask(OrderState::CANCELLED),
    /* EXPIRED         */ mask(OrderState::EXPIRED),
    /* REJECTED        */ mask(OrderState::REJECTED),
    /* FAILED          */ mask(OrderState::FAILED),
    /* SUPERSEDED      */ mask(OrderState::SUPERSEDED),
};

inline bool transition_allowed(OrderState from, OrderState to) noexcept {
    const unsigned f = static_cast<unsigned>(from);
    const unsigned t = static_cast<unsigned>(to);
    if (f >= static_cast<unsigned>(OrderState::COUNT) ||
        t >= static_cast<unsigned>(OrderState::COUNT) ||
        t == static_cast<unsigned>(OrderState::NONE) ||
        t == static_cast<unsigned>(OrderState::COUNT))
        return false;
    // An observation may repeat the current state (idempotent re-delivery).
    // That is a no-op, not a lifecycle change, and is always accepted so a
    // duplicated WebSocket message cannot poison the record.
    if (from == to) return true;
    return (K_ALLOWED[f] & (1U << t)) != 0U;
}

}  // namespace order_state

// ── Venue vocabulary → local state ──────────────────────────────────────────
namespace venue_status {

// Order statuses reported by GET /data/order/{id}, GET /data/orders and the
// user WebSocket `status` field.  Unknown strings fail closed to UNKNOWN.
inline OrderState from_order_status(const char* status, bool fully_matched,
                                    bool& recognized) noexcept {
    recognized = true;
    if (!status || !*status) { recognized = false; return OrderState::UNKNOWN; }
    if (std::strcmp(status, "LIVE") == 0 || std::strcmp(status, "live") == 0 ||
        std::strcmp(status, "DELAYED") == 0 ||
        std::strcmp(status, "delayed") == 0)
        return OrderState::LIVE;
    if (std::strcmp(status, "MATCHED") == 0 ||
        std::strcmp(status, "matched") == 0)
        return fully_matched ? OrderState::MATCHED : OrderState::PARTIALLY_FILLED;
    if (std::strcmp(status, "CANCELED") == 0 ||
        std::strcmp(status, "CANCELLED") == 0 ||
        std::strcmp(status, "canceled") == 0)
        return OrderState::CANCELLED;
    if (std::strcmp(status, "UNMATCHED") == 0 ||
        std::strcmp(status, "unmatched") == 0)
        // FAK/FOK killed without (further) match: the venue will not work it.
        return fully_matched ? OrderState::MATCHED : OrderState::CANCELLED;
    recognized = false;
    return OrderState::UNKNOWN;
}

// `orderEventType` / `type` on the user channel.
enum class OrderEventKind : uint8_t { UNKNOWN = 0, PLACEMENT, UPDATE, CANCELLATION };

inline OrderEventKind order_event_kind(const char* value) noexcept {
    if (!value) return OrderEventKind::UNKNOWN;
    if (std::strcmp(value, "PLACEMENT") == 0) return OrderEventKind::PLACEMENT;
    if (std::strcmp(value, "UPDATE") == 0) return OrderEventKind::UPDATE;
    if (std::strcmp(value, "CANCELLATION") == 0 ||
        std::strcmp(value, "CANCELLATION_ALL") == 0)
        return OrderEventKind::CANCELLATION;
    return OrderEventKind::UNKNOWN;
}

// Trade settlement statuses.  Only CONFIRMED and FAILED are terminal
// (docs: "Confirmation and permanent failure are terminal; a trade being
// retried may later be mined and confirmed").
enum class TradeKind : uint8_t {
    UNKNOWN = 0,
    MATCHED_NOT_BROADCASTED,
    MATCHED,
    MINED,
    CONFIRMED,
    RETRYING,
    FAILED
};

inline TradeKind trade_kind(const char* value) noexcept {
    if (!value) return TradeKind::UNKNOWN;
    if (std::strcmp(value, "TRADE_STATUS_MATCHED_NOT_BROADCASTED") == 0 ||
        std::strcmp(value, "MATCHED_NOT_BROADCASTED") == 0)
        return TradeKind::MATCHED_NOT_BROADCASTED;
    if (std::strcmp(value, "TRADE_STATUS_MATCHED") == 0 ||
        std::strcmp(value, "MATCHED") == 0)
        return TradeKind::MATCHED;
    if (std::strcmp(value, "TRADE_STATUS_MINED") == 0 ||
        std::strcmp(value, "MINED") == 0) return TradeKind::MINED;
    if (std::strcmp(value, "TRADE_STATUS_CONFIRMED") == 0 ||
        std::strcmp(value, "CONFIRMED") == 0) return TradeKind::CONFIRMED;
    if (std::strcmp(value, "TRADE_STATUS_RETRYING") == 0 ||
        std::strcmp(value, "RETRYING") == 0) return TradeKind::RETRYING;
    if (std::strcmp(value, "TRADE_STATUS_FAILED") == 0 ||
        std::strcmp(value, "FAILED") == 0) return TradeKind::FAILED;
    return TradeKind::UNKNOWN;
}

inline bool trade_is_terminal(TradeKind kind) noexcept {
    return kind == TradeKind::CONFIRMED || kind == TradeKind::FAILED;
}

// A fill counts towards inventory/exposure as soon as the venue reports a
// match; FAILED settlement is surfaced separately so reconciliation can
// re-derive the truth from balances instead of trusting this flag.
inline bool trade_counts_as_fill(TradeKind kind) noexcept {
    return kind == TradeKind::MATCHED_NOT_BROADCASTED ||
           kind == TradeKind::MATCHED || kind == TradeKind::MINED ||
           kind == TradeKind::CONFIRMED || kind == TradeKind::RETRYING;
}

// POST /order semantic result → local state.  `taking_amount`/`making_amount`
// come from the response body; `requested_size` is what we asked for.
inline OrderState from_post_response(const char* status, uint64_t filled_size,
                                     uint64_t requested_size,
                                     bool& recognized) noexcept {
    recognized = true;
    if (!status || !*status) { recognized = false; return OrderState::UNKNOWN; }
    const bool full = requested_size != 0 && filled_size >= requested_size;
    if (std::strcmp(status, "live") == 0 || std::strcmp(status, "LIVE") == 0)
        return full ? OrderState::MATCHED
                    : (filled_size ? OrderState::PARTIALLY_FILLED : OrderState::LIVE);
    if (std::strcmp(status, "delayed") == 0 ||
        std::strcmp(status, "DELAYED") == 0)
        // Accepted but not yet worked; must be confirmed by an authoritative read.
        return OrderState::LIVE;
    if (std::strcmp(status, "matched") == 0 ||
        std::strcmp(status, "MATCHED") == 0)
        return full ? OrderState::MATCHED : OrderState::PARTIALLY_FILLED;
    if (std::strcmp(status, "unmatched") == 0 ||
        std::strcmp(status, "UNMATCHED") == 0)
        return filled_size ? OrderState::PARTIALLY_FILLED : OrderState::CANCELLED;
    recognized = false;
    return OrderState::UNKNOWN;
}

}  // namespace venue_status

#endif  // ORDER_STATE_HPP
