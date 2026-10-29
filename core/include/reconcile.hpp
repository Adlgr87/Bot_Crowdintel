#ifndef CROWDINTEL_RECONCILE_HPP
#define CROWDINTEL_RECONCILE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// reconcile.hpp — the position-diff decision, extracted from the live path.
//
// WHY THIS EXISTS
// The reconciliation rule lived inline in main_hot_path.cpp inside
// `#if defined(CROWDINTEL_LIVE)`, so it could only run against a real venue with
// real credentials: it had no test at all (its original test source was lost
// from the tree). It is the most financially dangerous function in the
// repository — it decides whether the bot adopts the venue's inventory and
// whether that disagreement is severe enough to latch the kill switch.
//
// The transport stays in main (an HTTP GET). The DECISION is here, pure and
// testable: given what we think we hold and what the venue says we hold, do we
// restate, and do we stop?
//
// THE RULE, unchanged from the live path:
//   drift = |local_yes − venue_yes| + |local_hedge − venue_hedge|
//   restate only when drift > 0        (an idle market must not spam restates)
//   kill    only when drift > max      (strictly greater: == is tolerated)
//   max == 0 disables the kill         (explicitly, not accidentally)
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>

namespace crowdintel {

struct ReconcileInput {
    uint64_t local_yes_shares = 0;    // 1e6
    uint64_t local_hedge_shares = 0;  // 1e6
    uint64_t venue_yes_shares = 0;    // 1e6, parsed from GET /data/positions
    uint64_t venue_hedge_shares = 0;  // 1e6
    uint64_t venue_yes_avg = 0;       // 1e6
    uint64_t venue_hedge_avg = 0;     // 1e6
    double max_drift_shares = 0.0;    // BOT_RECONCILE_MAX_DRIFT_SHARES
};

struct ReconcileOutcome {
    bool restate = false;        // enqueue a TrackerRestate at all
    uint64_t drift = 0;          // 1e6 shares
    bool drift_exceeded = false; // latch the kill switch
};

inline ReconcileOutcome reconcile_positions(const ReconcileInput& in) noexcept {
    ReconcileOutcome out{};
    const auto diff = [](uint64_t a, uint64_t b) {
        return a > b ? a - b : b - a;
    };
    out.drift = diff(in.local_yes_shares, in.venue_yes_shares) +
                diff(in.local_hedge_shares, in.venue_hedge_shares);

    const uint64_t max_drift =
        in.max_drift_shares > 0.0
            ? static_cast<uint64_t>(in.max_drift_shares * 1000000.0)
            : 0;
    // Strictly greater, and a zero maximum means "do not kill on drift".
    out.drift_exceeded = max_drift != 0 && out.drift > max_drift;
    out.restate = out.drift > 0;
    return out;
}

}  // namespace crowdintel

#endif  // CROWDINTEL_RECONCILE_HPP
