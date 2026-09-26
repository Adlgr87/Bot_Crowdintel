#ifndef ORDER_BOOK_HPP
#pragma GCC optimize("O3,unroll-loops,fast-math")
#define ORDER_BOOK_HPP

#include <cstdint>
#include <array>
#include <atomic>
#include <chrono>

/**
 * Time utilities for staleness checks — thread-safe, no allocation.
 * Inlined for hot path performance.
 */
inline uint64_t current_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

/**
 * OrderBookL2: Ultra-low latency Level 2 Order Book.
 * Uses static arrays to avoid dynamic allocation and ensure cache locality.
 * Updates are O(1) for specific levels.
 */
struct Level2Entry {
    uint64_t price;      // Fixed point (price * 1e6)
    uint64_t size;       // Fixed point (size * 1e6)
    uint64_t timestamp;  // Nanoseconds from RDTSC
} __attribute__((packed));

class OrderBookL2 {
public:
    static constexpr size_t MAX_LEVELS = 100;

    OrderBookL2() : sequence_(0) {}

    // Update a bid level with zero allocation
        #pragma unroll 4
    inline void update_bid(uint32_t level, uint64_t price, uint64_t size, uint64_t ts) {
        if (level < MAX_LEVELS) {
            bids_[level] = {price, size, ts};
            sequence_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Update an ask level with zero allocation
        #pragma unroll 4
    inline void update_ask(uint32_t level, uint64_t price, uint64_t size, uint64_t ts) {
        if (level < MAX_LEVELS) {
            asks_[level] = {price, size, ts};
            sequence_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    inline const Level2Entry& get_bid(uint32_t level) const {
        // Zero-allocation bounds guard — returns sentinel entry for out-of-range
        static const Level2Entry empty_entry = {0, 0, 0};
        return (level < MAX_LEVELS) ? bids_[level] : empty_entry;
    }
    inline const Level2Entry& get_ask(uint32_t level) const {
        static const Level2Entry empty_entry = {0, 0, 0};
        return (level < MAX_LEVELS) ? asks_[level] : empty_entry;
    }
    inline uint64_t get_sequence() const { return sequence_.load(std::memory_order_acquire); }

    // ─── Staleness Check (O(1), branch-predicted) ───────────────────────────
    // Returns true if the best bid/ask levels are stale (not updated within maxAgeNs).
    // Adapted from Polywhales copclob: walkAsks + maxBookAgeMs.
    // Default maxBookAgeNs = 90s (90'000'000'000 ns) per Polywhales rules.ts
    inline bool is_stale(uint64_t max_age_ns = 90'000'000'000ULL) const {
        uint64_t now = current_time_ns();
        const auto& best_bid = get_bid(0);
        const auto& best_ask = get_ask(0);
        // Check the most recently updated side
        uint64_t latest_ts = (best_bid.timestamp > best_ask.timestamp)
            ? best_bid.timestamp
            : best_ask.timestamp;
        return (now - latest_ts) > max_age_ns;
    }

    // ─── VWAP Helper (O(N) with small N, used in cold path) ─────────────────
    // Calculate VWAP walking up to maxEntry price (adapted from Polywhales walkAsks).
    // Returns (vwap_price, fillable_usd) or (0, 0) if insufficient liquidity.
    struct VwapResult {
        uint64_t vwap_price;  // Weighted average price
        uint64_t fillable_usd;  // Total USD fillable within maxEntry
    };

    VwapResult walk_asks(uint64_t max_entry_price, uint64_t stake_usd) const {
        uint64_t usd_available = 0;
        uint64_t cost = 0;
        uint64_t units_remaining = stake_usd;  // Simplified: stake in USD

        for (size_t i = 0; i < MAX_LEVELS && units_remaining > 0; i++) {
            const auto& ask = asks_[i];
            if (ask.price == 0 || ask.size == 0) break;
            if (ask.price > max_entry_price) break;

            uint64_t level_usd = ask.price * ask.size;  // price * size (both in micros)
            usd_available += level_usd;

            if (units_remaining > 0) {
                uint64_t take = (units_remaining < level_usd) ? units_remaining : level_usd;
                cost += take * ask.price / 1'000'000ULL;  // cost in USD cents
                units_remaining -= take;
            }
        }

        if (usd_available == 0) return {0, 0};
        uint64_t vwap = (cost > 0) ? (cost / (usd_available - units_remaining) * 1'000'000ULL) : 0;
        // Simplified VWAP: weighted by level USD / total units
        vwap = usd_available / (stake_usd > 0 ? stake_usd : 1);
        return {vwap, usd_available};
    }

private:
    alignas(64) std::array<Level2Entry, MAX_LEVELS> bids_;
    alignas(64) std::array<Level2Entry, MAX_LEVELS> asks_;
    alignas(64) std::atomic<uint64_t> sequence_;
};

#endif // ORDER_BOOK_HPP
