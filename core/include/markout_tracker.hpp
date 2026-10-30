#ifndef CROWDINTEL_MARKOUT_TRACKER_HPP
#define CROWDINTEL_MARKOUT_TRACKER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// markout_tracker.hpp — the conditional adverse move, measured on real fills.
//
// THE NUMBER THIS PRODUCES
// docs/EXPERIMENT_MOTOR_C.md's +0.392c net per fill uses UNCONDITIONAL one-minute
// volatility (0.092c median). Adverse selection is conditional on being filled,
// and a fill happens exactly when the price moved against the quote. So the
// figure that decides the sleeve is: after we get filled, how far does the mid
// move against us? That is a mark-out, and it can only come from real fills.
//
// SIGN CONVENTION (easy to get backwards; pinned by tests):
//   BUY  at P, mid later < P  -> the price fell, we lost  -> adverse POSITIVE
//   SELL at P, mid later > P  -> the price rose, we lost  -> adverse POSITIVE
//   therefore  adverse = sign * (P - mid_later),  sign = +1 BUY, -1 SELL
// A negative mean means fills were, on average, FAVOURABLE — which would itself
// be suspicious and worth investigating before trusting it.
//
// Hot path: a fill is one bounded ring write; observe() settles samples whose
// horizon elapsed. No allocation, no clock reads (the caller passes `now`).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>

namespace crowdintel {

inline constexpr size_t MARKOUT_CAPACITY = 128;
inline constexpr uint8_t MARKOUT_SIDE_BUY = 0;
inline constexpr uint8_t MARKOUT_SIDE_SELL = 1;

class MarkoutTracker {
public:
    // Startup-only. There is no hot reconfig of the horizon in this process, so
    // clearing pending samples here is deliberate; calling this mid-run would
    // drop the very samples this class exists to measure.
    void configure(uint64_t horizon_ns) noexcept {
        horizon_ns_ = horizon_ns ? horizon_ns : 60'000'000'000ULL;
        clear();
    }

    void clear() noexcept {
        for (size_t i = 0; i < MARKOUT_CAPACITY; ++i) samples_[i] = Sample{};
        count_ = 0;
        qualified_ = 0;
        sum_adverse_e6_ = 0;
        dropped_ = 0;
    }

    // Record a fill. Returns false when the sample is malformed or the ring is
    // full; a full ring increments dropped() so the loss is visible, not silent.
    bool record(uint8_t side, uint64_t price, uint64_t size,
                uint64_t ts_ns) noexcept {
        // `size` is validated (a zero-size fill is malformed) but not stored:
        // the mean is unweighted by design until the canary shows a size spread
        // that justifies weighting.
        if (price == 0 || price >= 1000000ULL || size == 0) return false;
        if (side != MARKOUT_SIDE_BUY && side != MARKOUT_SIDE_SELL) return false;
        for (size_t i = 0; i < MARKOUT_CAPACITY; ++i) {
            if (samples_[i].pending) continue;
            samples_[i].pending = true;
            samples_[i].side = side;
            samples_[i].price = price;
            samples_[i].ts_ns = ts_ns;
            ++count_;
            return true;
        }
        ++dropped_;
        return false;
    }

    // Settle every sample older than the horizon against `mid`.
    // Returns how many were settled on this call.
    size_t observe(uint64_t mid, uint64_t now_ns) noexcept {
        if (mid == 0 || mid >= 1000000ULL) return 0;
        size_t settled = 0;
        for (size_t i = 0; i < MARKOUT_CAPACITY; ++i) {
            if (!samples_[i].pending) continue;
            if (now_ns < samples_[i].ts_ns ||
                now_ns - samples_[i].ts_ns < horizon_ns_)
                continue;
            const int64_t p = static_cast<int64_t>(samples_[i].price);
            const int64_t m = static_cast<int64_t>(mid);
            const int64_t diff = samples_[i].side == MARKOUT_SIDE_BUY ? p - m
                                                                     : m - p;
            sum_adverse_e6_ += diff;
            ++qualified_;
            samples_[i] = Sample{};
            if (count_) --count_;
            ++settled;
        }
        return settled;
    }

    uint64_t settled() const noexcept { return qualified_; }
    uint64_t pending() const noexcept { return count_; }
    uint64_t dropped() const noexcept { return dropped_; }

    // Exact int64 accumulator in 1e6 price units (microdollars). Exposed so the
    // metrics gauge reads the exact sum instead of a double round-trip.
    int64_t sum_adverse_e6() const noexcept { return sum_adverse_e6_; }

    // Mean adverse move in CENTS (1e6 price units / 10000 = one cent).
    double mean_adverse_cents() const noexcept {
        if (qualified_ == 0) return 0.0;
        return static_cast<double>(sum_adverse_e6_) /
               static_cast<double>(qualified_) / 10000.0;
    }

    uint64_t horizon_ns() const noexcept { return horizon_ns_; }

private:
    struct Sample {
        uint64_t price = 0;
        uint64_t ts_ns = 0;
        uint8_t side = 0;
        bool pending = false;
    };
    Sample samples_[MARKOUT_CAPACITY]{};
    size_t count_ = 0;
    uint64_t qualified_ = 0;
    uint64_t dropped_ = 0;
    int64_t sum_adverse_e6_ = 0;
    uint64_t horizon_ns_ = 60'000'000'000ULL;
};

}  // namespace crowdintel

#endif  // CROWDINTEL_MARKOUT_TRACKER_HPP
