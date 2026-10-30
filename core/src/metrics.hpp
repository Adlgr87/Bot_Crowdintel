#ifndef CROWDINTEL_METRICS_HPP
#define CROWDINTEL_METRICS_HPP

// ─────────────────────────────────────────────────────────────────────────────
// metrics.hpp — business instrumentation for the hot path (Fase 3).
//
// WHY THIS EXISTS
// The repository tracks latency budgets and acceptance gates in prose, but
// production emitted no counters: there was no way to tell "no edge" from
// "no book" from "risk rejected".  Without that split, tuning is guesswork and
// every capital decision is an opinion.
//
// DESIGN CONSTRAINTS (hot path):
//   * one relaxed fetch_add per event, no allocation, no locks, no I/O;
//   * one writer per counter (the tick thread), many readers (the health
//     thread), which is why relaxed ordering is sufficient: a reader may see
//     a stale count for at most one scrape interval, never a torn value;
//   * cache-line aligned so the health thread's reads cannot false-share with
//     the tick thread's writes.
//
// This header deliberately depends on nothing but <atomic>/<cstdint> so that
// execution_engine.hpp can include it without a cycle.  Callers index the
// histogram with static_cast<size_t>(TickResult), and the engine static_asserts
// that the enum still fits.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace crowdintel {

// Room for every TickResult plus headroom.  The engine static_asserts against
// TickResult::COUNT so this can never silently truncate.
inline constexpr size_t METRICS_RESULT_SLOTS = 32;

// Motor C: why the maker did or did not quote this tick. The taker path got this
// in Fase 3 and it is what turned "the bot does nothing" into a labelled reason;
// the sleeve that actually earns was left with SUBMITTED / NO_SIGNAL only.
enum class MakerEvent : uint8_t {
    Quoted = 0,        // both sides placed
    Kept,              // resting quotes still valid
    Withdrew,          // quotes pulled (unquotable book / kill)
    NoBook,            // book stale, empty or crossed
    BandTooNarrow,     // reward band cannot cover cushion + edge
    SpreadBelowCost,   // the market is tighter than our cost
    SizeTooSmall,      // below rewardsMinSize or min_size_shares
    InventoryFull,     // skew stopped both sides
    RiskRefused,       // RiskManager denied the BUY
    VolumeCapReached,  // daily BUY volume would be exceeded
    VenueRefused,      // the venue rejected the wire
    SignFailed,        // signing or body construction failed
    COUNT
};
inline constexpr size_t MAKER_EVENT_SLOTS =
    static_cast<size_t>(MakerEvent::COUNT);

inline const char* maker_event_name(MakerEvent e) noexcept {
    switch (e) {
        case MakerEvent::Quoted:          return "quoted";
        case MakerEvent::Kept:            return "kept";
        case MakerEvent::Withdrew:        return "withdrew";
        case MakerEvent::NoBook:          return "no_book";
        case MakerEvent::BandTooNarrow:   return "band_too_narrow";
        case MakerEvent::SpreadBelowCost: return "spread_below_cost";
        case MakerEvent::SizeTooSmall:    return "size_too_small";
        case MakerEvent::InventoryFull:   return "inventory_full";
        case MakerEvent::RiskRefused:     return "risk_refused";
        case MakerEvent::VolumeCapReached:return "volume_cap";
        case MakerEvent::VenueRefused:    return "venue_refused";
        case MakerEvent::SignFailed:      return "sign_failed";
        case MakerEvent::COUNT:           break;
    }
    return "unknown";
}

// Plain-old-data view used for rendering.  No atomics cross this boundary.
struct MetricsSnapshot {
    uint64_t decisions;
    uint64_t orders_requested;
    uint64_t fills;
    uint64_t reject_reason[METRICS_RESULT_SLOTS];
    uint64_t edge_samples;
    double edge_mean;          // mean net edge at submit (probability units)
    uint64_t slip_samples;
    double slip_mean_bps;      // mean adverse fill slippage in basis points
    uint64_t brier_samples;
    double brier;              // mean squared error of p_win vs realised label
};

class Metrics {
public:
    Metrics() noexcept { clear(); }
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // ── Hot path ────────────────────────────────────────────────────────────
    // Called once per tick with the engine's verdict.  This single line is
    // what turns "the bot does nothing" into a labelled reason.
    void on_decision(uint32_t result_code) noexcept {
        decisions_.fetch_add(1, std::memory_order_relaxed);
        if (result_code < METRICS_RESULT_SLOTS)
            results_[result_code].fetch_add(1, std::memory_order_relaxed);
        else
            results_[METRICS_RESULT_SLOTS - 1].fetch_add(
                1, std::memory_order_relaxed);  // fold overflow into the last slot
    }

    // Motor C: one relaxed increment per maker decision.
    void on_maker(MakerEvent e) noexcept {
        const size_t i = static_cast<size_t>(e);
        if (i < MAKER_EVENT_SLOTS)
            maker_[i].fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t maker(MakerEvent e) const noexcept {
        const size_t i = static_cast<size_t>(e);
        return i < MAKER_EVENT_SLOTS
                   ? maker_[i].load(std::memory_order_relaxed)
                   : 0;
    }

    // Motor C: the conditional adverse move, in 1e6 price units accumulated.
    // The maker engine pushes the tracker's totals each tick; the gauge is the
    // mean in cents, which is the number the canary exists to produce.
    void set_markout(int64_t sum_e6, uint64_t n) noexcept {
        markout_sum_e6_.store(sum_e6, std::memory_order_relaxed);
        markout_n_.store(n, std::memory_order_relaxed);
    }
    int64_t markout_sum_e6() const noexcept {
        return markout_sum_e6_.load(std::memory_order_relaxed);
    }
    uint64_t markout_samples() const noexcept {
        return markout_n_.load(std::memory_order_relaxed);
    }

    void on_order_requested() noexcept {
        orders_requested_.fetch_add(1, std::memory_order_relaxed);
    }

    // `net_edge` is the fee-aware edge the engine acted on, in probability
    // units (0.011 == 1.1 cents per contract).
    void on_order_submitted(double net_edge) noexcept {
        orders_requested_.fetch_add(1, std::memory_order_relaxed);
        edge_sum_e9_.fetch_add(to_e9(net_edge), std::memory_order_relaxed);
        edge_n_.fetch_add(1, std::memory_order_relaxed);
    }

    // `slippage_bps` is (fill_price - decision_price) in basis points, already
    // signed against the position so that positive always means "worse".
    void on_fill(double slippage_bps) noexcept {
        fills_.fetch_add(1, std::memory_order_relaxed);
        slip_sum_e6_.fetch_add(to_e6(slippage_bps), std::memory_order_relaxed);
        slip_n_.fetch_add(1, std::memory_order_relaxed);
    }

    // Calibration: accumulate (p_win - label)^2 directly, where `label` is 1.0
    // for a winner and 0.0 for a loser.  Storing the sum keeps the score an
    // O(1) read; no history buffer is needed to report Brier.
    void on_outcome(double p_win, double label) noexcept {
        const double error = p_win - label;
        brier_sum_e6_.fetch_add(to_e6(error * error), std::memory_order_relaxed);
        brier_n_.fetch_add(1, std::memory_order_relaxed);
    }

    // ── Cold path ───────────────────────────────────────────────────────────
    MetricsSnapshot snapshot() const noexcept {
        MetricsSnapshot out{};
        out.decisions = decisions_.load(std::memory_order_relaxed);
        out.orders_requested = orders_requested_.load(std::memory_order_relaxed);
        out.fills = fills_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < METRICS_RESULT_SLOTS; ++i)
            out.reject_reason[i] = results_[i].load(std::memory_order_relaxed);

        const uint64_t edge_n = edge_n_.load(std::memory_order_relaxed);
        out.edge_samples = edge_n;
        out.edge_mean = edge_n
            ? static_cast<double>(edge_sum_e9_.load(std::memory_order_relaxed)) *
                  1e-9 / static_cast<double>(edge_n)
            : 0.0;

        const uint64_t slip_n = slip_n_.load(std::memory_order_relaxed);
        out.slip_samples = slip_n;
        out.slip_mean_bps = slip_n
            ? static_cast<double>(slip_sum_e6_.load(std::memory_order_relaxed)) *
                  1e-6 / static_cast<double>(slip_n)
            : 0.0;

        const uint64_t brier_n = brier_n_.load(std::memory_order_relaxed);
        out.brier_samples = brier_n;
        out.brier = brier_n
            ? static_cast<double>(brier_sum_e6_.load(std::memory_order_relaxed)) *
                  1e-6 / static_cast<double>(brier_n)
            : 0.0;
        return out;
    }

    void clear() noexcept {
        decisions_.store(0, std::memory_order_relaxed);
        orders_requested_.store(0, std::memory_order_relaxed);
        fills_.store(0, std::memory_order_relaxed);
        edge_sum_e9_.store(0, std::memory_order_relaxed);
        edge_n_.store(0, std::memory_order_relaxed);
        slip_sum_e6_.store(0, std::memory_order_relaxed);
        slip_n_.store(0, std::memory_order_relaxed);
        brier_sum_e6_.store(0, std::memory_order_relaxed);
        brier_n_.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < METRICS_RESULT_SLOTS; ++i)
            results_[i].store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < MAKER_EVENT_SLOTS; ++i)
            maker_[i].store(0, std::memory_order_relaxed);
        markout_sum_e6_.store(0, std::memory_order_relaxed);
        markout_n_.store(0, std::memory_order_relaxed);
    }

private:
    // Fixed-point accumulators keep the hot path integer-only (no FP rounding
    // drift between writers) and make the sums exactly reproducible.
    static int64_t to_e9(double value) noexcept {
        const double scaled = value * 1e9;
        if (!(scaled > -9.2e18 && scaled < 9.2e18)) return 0;  // NaN/overflow guard
        return static_cast<int64_t>(scaled);
    }
    static int64_t to_e6(double value) noexcept {
        const double scaled = value * 1e6;
        if (!(scaled > -9.2e18 && scaled < 9.2e18)) return 0;
        return static_cast<int64_t>(scaled);
    }

    alignas(64) std::atomic<uint64_t> maker_[MAKER_EVENT_SLOTS]{};
    alignas(64) std::atomic<int64_t> markout_sum_e6_{0};
    alignas(64) std::atomic<uint64_t> markout_n_{0};
    alignas(64) std::atomic<uint64_t> decisions_{0};
    alignas(64) std::atomic<uint64_t> results_[METRICS_RESULT_SLOTS]{};
    alignas(64) std::atomic<uint64_t> orders_requested_{0};
    alignas(64) std::atomic<uint64_t> fills_{0};
    alignas(64) std::atomic<int64_t> edge_sum_e9_{0};
    alignas(64) std::atomic<uint64_t> edge_n_{0};
    alignas(64) std::atomic<int64_t> slip_sum_e6_{0};
    alignas(64) std::atomic<uint64_t> slip_n_{0};
    alignas(64) std::atomic<int64_t> brier_sum_e6_{0};
    alignas(64) std::atomic<uint64_t> brier_n_{0};
};

}  // namespace crowdintel

#endif  // CROWDINTEL_METRICS_HPP
