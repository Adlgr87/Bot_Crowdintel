#ifndef CROWDINTEL_MAKER_RUNNER_HPP
#define CROWDINTEL_MAKER_RUNNER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// maker_runner.hpp — executes a MakerDecision against a client.
//
// THE SEAM
// MakerLoop decides; this executes; the engine supplies the two side effects.
// Signing is deliberately NOT here: it is passed in as callables, so the same
// code drives a signed live order and a mock body without either knowing about
// the other.
//
// THE INVARIANT: every cancel happens BEFORE any placement on that side.
// A replace that places first can momentarily hold two rests on one side, which
// is precisely the exposure the inventory caps exist to bound. The executor
// enforces the order rather than trusting callers to remember it.
//
// FAILURE HANDLING: a failed cancel aborts the placement on that side. Placing
// a new quote while the old one may still be live is worse than not quoting —
// it doubles up exactly when the model already wanted to move.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>

namespace crowdintel {

struct MakerRunSummary {
    bool cancelled_bid = false;
    bool cancelled_ask = false;
    bool placed_bid = false;
    bool placed_ask = false;
    // Counted separately so a caller can tell "nothing to do" from "the venue
    // refused": a silent zero on both is the failure mode worth seeing.
    uint32_t errors = 0;
};

// `place(side, price, size)` -> true when the order was accepted.
//   side: 0 = bid, 1 = ask  (matches quote_registry / maker_quote)
// `cancel(id)` -> true when the venue acknowledged (or the id was already gone).
template <typename PlaceFn, typename CancelFn>
MakerRunSummary run_maker_decision(const MakerDecision& d, PlaceFn&& place,
                                   CancelFn&& cancel) {
    MakerRunSummary out{};

    // ── 1. Cancels first, both sides ───────────────────────────────────────
    bool bid_clear = true;
    bool ask_clear = true;

    if (d.cancel_bid_id != 0) {
        out.cancelled_bid = cancel(d.cancel_bid_id);
        bid_clear = out.cancelled_bid;
        if (!bid_clear) ++out.errors;
    }
    if (d.cancel_ask_id != 0) {
        out.cancelled_ask = cancel(d.cancel_ask_id);
        ask_clear = out.cancelled_ask;
        if (!ask_clear) ++out.errors;
    }

    // ── 2. Then placements, gated on the matching cancel having succeeded ──
    if (d.place_bid && bid_clear) {
        out.placed_bid = place(0, d.bid_price, d.bid_size);
        if (!out.placed_bid) ++out.errors;
    }
    if (d.place_ask && ask_clear) {
        out.placed_ask = place(1, d.ask_price, d.ask_size);
        if (!out.placed_ask) ++out.errors;
    }
    return out;
}

}  // namespace crowdintel

#endif  // CROWDINTEL_MAKER_RUNNER_HPP
