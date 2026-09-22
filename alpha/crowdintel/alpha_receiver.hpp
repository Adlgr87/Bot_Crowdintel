#ifndef ALPHA_RECEIVER_HPP
#define ALPHA_RECEIVER_HPP

#include <cstdint>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// AlphaSignal: POD message from the CrowdIntel alpha feed (cold path) to the
// execution engine (hot path). Trivially copyable so it crosses the SPSC ring
// buffer as a plain memcpy.
//
// Semantics (important — this is the engine's contract):
//   p_win       : posterior probability that the configured outcome resolves
//                 YES, in (0,1). The engine combines this with the LIVE book
//                 price to compute edge and Kelly size (statistical filters
//                 happen here in the cold path; economic filters, which need
//                 the live price, happen in the hot path).
//   confidence  : signal quality ∈ [0,1] (reject < BOT_MIN_CONFIDENCE).
//   q_value     : FDR q-value (reject > BOT_MAX_Q_VALUE).
// ─────────────────────────────────────────────────────────────────────────────

struct AlphaSignal {
    enum class Type : uint8_t {
        WHALE_TRADE = 0,
        FUNDING_CLUSTER = 1,
        MACHINE_DETECTION = 2,
        MARKET_EVENT = 3
    };

    Type     type;
    uint8_t  direction_hint;   // 0 = buy, 1 = sell, 2 = engine decides
    double   p_win;            // posterior P(YES)
    double   confidence;       // signal quality
    double   q_value;          // false discovery rate
    uint64_t timestamp_ns;     // signal arrival time (CLOCK_REALTIME ns)
    char     market_slug[16];  // debug label only (hot path uses the single
                               // configured market/token)
};
static_assert(sizeof(AlphaSignal) <= 64, "keep signals cache-line friendly");

#endif // ALPHA_RECEIVER_HPP
