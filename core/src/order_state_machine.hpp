#ifndef ORDER_STATE_MACHINE_HPP
#define ORDER_STATE_MACHINE_HPP

// Phase 2 — explicit order lifecycle with a mandatory UNKNOWN state.
//
// Rationale: any order whose venue outcome is not proven by venue evidence is
// UNKNOWN. UNKNOWN blocks trading (fail closed). Local assumptions (timeout ==
// rejected, queued == live) are forbidden: leaving UNKNOWN requires evidence
// from the venue (acknowledgement, private-channel event, or REST state).
//
// The header is pure logic: no I/O, no allocation, no clocks. The ledger
// (order_ledger.hpp) persists transitions; the execution engine asks this
// table for the next state and refuses to trade while any order is
// unreconciled.

#include <cstdint>
#include <cstring>

namespace cledger {

enum class OrderState : uint8_t {
    kUnknown = 0,        // no venue proof: blocks trading
    kPendingSubmit = 1,  // intent journaled, egress not acknowledged yet
    kSubmitted = 2,      // venue returned an order id; no fill evidence yet
    kLive = 3,           // venue channel reports the order resting
    kPartiallyFilled = 4,
    kFilled = 5,         // terminal
    kCanceled = 6,       // terminal
    kExpired = 7,        // terminal
    kRejected = 8,       // terminal (venue refused the order)
    kCount = 9,
};

// How the caller knows what it knows. Venue evidence ranks above local intent.
enum class Evidence : uint8_t {
    kNone = 0,         // local assumption only (never enough to leave UNKNOWN)
    kLocalIntent = 1,  // we journaled the intent ourselves
    kVenueAck = 2,     // venue answered the submit request (id or rejection)
    kVenueChannel = 3, // private user channel event (order or trade)
    kVenueRest = 4,    // authenticated REST reconciliation
};

enum class LedgerEvent : uint8_t {
    kIntentRecorded = 0,  // local: intent durable before egress
    kSubmitAck = 1,       // venue accepted the order and returned an id
    kSubmitAmbiguous = 2, // timeout / transport failure: outcome unknown
    kSubmitRejected = 3,  // venue explicitly refused (4xx with body)
    kOrderLive = 4,       // channel: order status LIVE
    kOrderCanceled = 5,   // channel/REST: canceled
    kOrderExpired = 6,    // channel/REST: expired (GTD)
    kTradeUpdate = 7,     // channel: trade matched/mined/confirmed
    kTradeFailed = 8,     // channel: trade FAILED/RETRYING -> back to UNKNOWN
    kReconcilePresent = 9,// REST: order exists, with cumulative fill
    kReconcileAbsent = 10,// REST: confirmed absent after open-orders+trades check
    // The venue cancels every order of this account when heartbeats stop
    // arriving; whatever the journal believed is no longer provable.
    kHeartbeatLost = 11,
    kCount = 12,
};

enum class TransitionResult : uint8_t {
    kApplied = 0,
    kUnchanged = 1,      // idempotent repeat (channel replays)
    kNeedsReconcile = 2, // not enough evidence: order stays/returns UNKNOWN
    kIllegal = 3,        // contradicts a terminal state: quarantine + block
};

inline const char* order_state_name(OrderState state) noexcept {
    switch (state) {
        case OrderState::kUnknown: return "UNKNOWN";
        case OrderState::kPendingSubmit: return "PENDING_SUBMIT";
        case OrderState::kSubmitted: return "SUBMITTED";
        case OrderState::kLive: return "LIVE";
        case OrderState::kPartiallyFilled: return "PARTIALLY_FILLED";
        case OrderState::kFilled: return "FILLED";
        case OrderState::kCanceled: return "CANCELED";
        case OrderState::kExpired: return "EXPIRED";
        case OrderState::kRejected: return "REJECTED";
        case OrderState::kCount: return "INVALID";
    }
    return "INVALID";
}

inline const char* evidence_name(Evidence evidence) noexcept {
    switch (evidence) {
        case Evidence::kNone: return "none";
        case Evidence::kLocalIntent: return "local_intent";
        case Evidence::kVenueAck: return "venue_ack";
        case Evidence::kVenueChannel: return "venue_channel";
        case Evidence::kVenueRest: return "venue_rest";
    }
    return "invalid";
}

inline const char* ledger_event_name(LedgerEvent event) noexcept {
    switch (event) {
        case LedgerEvent::kIntentRecorded: return "INTENT";
        case LedgerEvent::kSubmitAck: return "SUBMIT_ACK";
        case LedgerEvent::kSubmitAmbiguous: return "SUBMIT_AMBIGUOUS";
        case LedgerEvent::kSubmitRejected: return "SUBMIT_REJECTED";
        case LedgerEvent::kOrderLive: return "ORDER_LIVE";
        case LedgerEvent::kOrderCanceled: return "ORDER_CANCELED";
        case LedgerEvent::kOrderExpired: return "ORDER_EXPIRED";
        case LedgerEvent::kTradeUpdate: return "TRADE_UPDATE";
        case LedgerEvent::kTradeFailed: return "TRADE_FAILED";
        case LedgerEvent::kReconcilePresent: return "RECONCILE_PRESENT";
        case LedgerEvent::kReconcileAbsent: return "RECONCILE_ABSENT";
        case LedgerEvent::kHeartbeatLost: return "HEARTBEAT_LOST";
        case LedgerEvent::kCount: return "INVALID";
    }
    return "INVALID";
}

inline const char* transition_result_name(TransitionResult result) noexcept {
    switch (result) {
        case TransitionResult::kApplied: return "applied";
        case TransitionResult::kUnchanged: return "unchanged";
        case TransitionResult::kNeedsReconcile: return "needs_reconcile";
        case TransitionResult::kIllegal: return "illegal";
    }
    return "invalid";
}

inline bool is_terminal(OrderState state) noexcept {
    return state == OrderState::kFilled || state == OrderState::kCanceled ||
           state == OrderState::kExpired || state == OrderState::kRejected;
}

// Minimum evidence required to publish each state. UNKNOWN and PENDING_SUBMIT
// are the only states reachable without venue proof.
inline Evidence required_evidence(OrderState state) noexcept {
    switch (state) {
        case OrderState::kUnknown:
        case OrderState::kPendingSubmit: return Evidence::kLocalIntent;
        case OrderState::kSubmitted:
        case OrderState::kRejected: return Evidence::kVenueAck;
        case OrderState::kLive:
        case OrderState::kPartiallyFilled:
        case OrderState::kFilled:
        case OrderState::kCanceled:
        case OrderState::kExpired: return Evidence::kVenueChannel;
        case OrderState::kCount: break;
    }
    return Evidence::kVenueRest;
}

struct Transition {
    TransitionResult result = TransitionResult::kNeedsReconcile;
    OrderState next = OrderState::kUnknown;
};

// Pure transition table.
//
//   * UNKNOWN is the only state reachable from a failed/ambiguous submit and
//     the state every unproven order sits in; it never leaves UNKNOWN without
//     venue evidence.
//   * Terminal states are immutable: a contradictory venue event returns
//     kIllegal so the caller quarantines and blocks instead of trading on a
//     divergent view of the account.
//   * `filled_fixed6` and `shares_fixed6` are cumulative (fixed 1e-6 shares);
//     a trade update with filled >= shares is FILLED, otherwise
//     PARTIALLY_FILLED.
inline Transition transition(OrderState current, LedgerEvent event,
                             Evidence evidence, uint64_t filled_fixed6 = 0,
                             uint64_t shares_fixed6 = 0) noexcept {
    OrderState desired = current;
    switch (event) {
        case LedgerEvent::kIntentRecorded:
            desired = OrderState::kPendingSubmit;
            break;
        case LedgerEvent::kSubmitAck:
            desired = OrderState::kSubmitted;
            break;
        case LedgerEvent::kSubmitAmbiguous:
            desired = OrderState::kUnknown;
            break;
        case LedgerEvent::kSubmitRejected:
            desired = OrderState::kRejected;
            break;
        case LedgerEvent::kOrderLive:
            desired = OrderState::kLive;
            break;
        case LedgerEvent::kOrderCanceled:
            desired = OrderState::kCanceled;
            break;
        case LedgerEvent::kOrderExpired:
            desired = OrderState::kExpired;
            break;
        case LedgerEvent::kTradeUpdate:
            if (shares_fixed6 == 0)
                return {TransitionResult::kNeedsReconcile, current};
            desired = filled_fixed6 >= shares_fixed6
                          ? OrderState::kFilled
                          : OrderState::kPartiallyFilled;
            break;
        case LedgerEvent::kTradeFailed:
        case LedgerEvent::kHeartbeatLost:
            // Neither a failed trade nor a lost heartbeat proves anything
            // about the order itself: the outcome is unknown until the venue
            // is asked again.
            desired = OrderState::kUnknown;
            break;
        case LedgerEvent::kReconcilePresent:
            if (shares_fixed6 == 0)
                desired = OrderState::kLive;
            else if (filled_fixed6 >= shares_fixed6)
                desired = OrderState::kFilled;
            else if (filled_fixed6 > 0)
                desired = OrderState::kPartiallyFilled;
            else
                desired = OrderState::kLive;
            break;
        case LedgerEvent::kReconcileAbsent:
            desired = OrderState::kRejected;
            break;
        case LedgerEvent::kCount:
            return {TransitionResult::kIllegal, current};
    }

    if (is_terminal(current)) {
        return desired == current ? Transition{TransitionResult::kUnchanged, current}
                                  : Transition{TransitionResult::kIllegal, current};
    }
    if (event == LedgerEvent::kSubmitAmbiguous ||
        event == LedgerEvent::kTradeFailed ||
        event == LedgerEvent::kHeartbeatLost)
        return desired == current ? Transition{TransitionResult::kUnchanged, current}
                                  : Transition{TransitionResult::kApplied, desired};

    // Confirmed absence is only proof when it comes from an authenticated
    // venue query; a channel event or a local assumption cannot declare an
    // order nonexistent.
    if (event == LedgerEvent::kReconcileAbsent &&
        evidence < Evidence::kVenueRest)
        return {TransitionResult::kNeedsReconcile, OrderState::kUnknown};
    // Leaving UNKNOWN for any state above PENDING_SUBMIT requires proof.
    if (current == OrderState::kUnknown && is_terminal(desired) &&
        evidence < required_evidence(desired))
        return {TransitionResult::kNeedsReconcile, OrderState::kUnknown};
    if (evidence < required_evidence(desired))
        return {TransitionResult::kNeedsReconcile, OrderState::kUnknown};
    return desired == current ? Transition{TransitionResult::kUnchanged, current}
                              : Transition{TransitionResult::kApplied, desired};
}

// True when the order still needs venue proof before trading may resume.
inline bool blocks_trading(OrderState state) noexcept {
    return !is_terminal(state);
}

}  // namespace cledger

#endif  // ORDER_STATE_MACHINE_HPP
