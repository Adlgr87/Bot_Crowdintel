#ifndef ORDER_BOOK_HPP
#pragma GCC optimize("O3,unroll-loops,fast-math")
#define ORDER_BOOK_HPP

#include <cstdint>
#include <array>
#include <atomic>
<<<<<<< Updated upstream
=======
#include <chrono>
#include <cstring>

/**
 * Time utilities for staleness checks — thread-safe, no allocation.
 * Inlined for hot path performance.
 */
inline uint64_t current_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}
>>>>>>> Stashed changes

/**
 * Level2Entry: A single Level 2 price level entry.
 * Packed struct for cache efficiency — hot path reads these via SIMD-friendly
 * contiguous arrays.
 */
struct Level2Entry {
    uint64_t price;      // Fixed point (price * 1e6)
    uint64_t size;       // Fixed point (size * 1e6)
    uint64_t timestamp;  // Nanoseconds from steady_clock (for staleness)
} __attribute__((packed));

/**
 * OrderBookL2: Ultra-low latency Level 2 Order Book.
 *
 * P0 fix (from DEEP_AUDIT_2026-09-26.es.md):
 *   - Rewrote to double-buffer with atomic index swap (seqlock-style).
 *   - Added set_book() for atomic joint publication of bids + asks.
 *   - Each buffer update writes to inactive buffer, then swaps active_ index
 *     with release semantics. Consumer reads active_ with acquire semantics.
 *
 * Uses static arrays to avoid dynamic allocation and ensure cache locality.
 * Updates are O(1) for specific levels.
 */
class OrderBookL2 {
public:
    static constexpr size_t MAX_LEVELS = 100;

private:
    /** Double-buffered book snapshot for atomic publication. */
    struct BookBuffer {
        std::array<Level2Entry, MAX_LEVELS> bids;
        std::array<Level2Entry, MAX_LEVELS> asks;
        uint64_t timestamp;  // Last publish timestamp (nanoseconds)
        uint64_t sequence;   // Monotonically increasing update counter
    };

    alignas(64) std::array<BookBuffer, 2> buffers_;
    alignas(64) std::atomic<uint32_t> active_;      // 0 or 1: index into buffers_
    alignas(64) std::atomic<uint64_t> sequence_;  // Global sequence counter

public:
    OrderBookL2() : active_(0), sequence_(0) {
        // Initialize both buffers with sentinel entries
        for (auto& buf : buffers_) {
            for (size_t i = 0; i < MAX_LEVELS; i++) {
                buf.bids[i] = {0, 0, 0};
                buf.asks[i] = {0, 0, 0};
            }
            buf.timestamp = 0;
            buf.sequence = 0;
        }
        sequence_.store(0, std::memory_order_relaxed);
    }

    /**
     * set_book() — Atomically publish a complete L2 snapshot.
     *
     * P0 fix: Double-buffer publication with atomic index swap.
     * Writes to the inactive buffer (copying bids and asks atomically),
     * sets timestamps, then swaps active_ with release semantics.
     * Consumer (hot path) reads active_ with acquire semantics, ensuring
     * it never sees a partially-published snapshot.
     *
     * This implements the double-buffer pattern: producer writes to
     * inactive buffer while consumer reads from active buffer, then
     * atomically swap. No locks needed.
     *
     * @param bids  Array of bid levels (sorted descending by price)
     * @param nb    Number of bid levels to copy
     * @param asks  Array of ask levels (sorted ascending by price)
     * @param na    Number of ask levels to copy
     * @param ts_ns Timestamp for this snapshot (nanoseconds)
     */
    void set_book(const Level2Entry* bids, size_t nb,
                  const Level2Entry* asks, size_t na,
                  uint64_t ts_ns = 0) {
        // Write to inactive buffer (no reader can see this until we swap)
        const uint32_t next = 1u - active_.load(std::memory_order_acquire);
        auto& buf = buffers_[next];

        // Copy bids (up to MAX_LEVELS), update timestamp for staleness tracking
        size_t i = 0;
        for (; i < nb && i < MAX_LEVELS; i++) {
            buf.bids[i] = bids[i];
            buf.bids[i].timestamp = ts_ns;
        }
        for (; i < MAX_LEVELS; i++) {
            buf.bids[i] = {0, 0, 0};
        }

        // Copy asks (up to MAX_LEVELS), update timestamp for staleness tracking
        i = 0;
        for (; i < na && i < MAX_LEVELS; i++) {
            buf.asks[i] = asks[i];
            buf.asks[i].timestamp = ts_ns;
        }
        for (; i < MAX_LEVELS; i++) {
            buf.asks[i] = {0, 0, 0};
        }

        buf.timestamp = ts_ns;
        buf.sequence = sequence_.load(std::memory_order_relaxed) + 1;
        sequence_.store(buf.sequence, std::memory_order_relaxed);

        // Atomic publish — consumer reads active_ with acquire, then reads buffer
        active_.store(next, std::memory_order_release);
    }

    /**
     * Update a bid level with zero allocation (double-buffer + swap).
     * P0 fix: Now uses double-buffer write + atomic swap.
     */
    void update_bid(uint32_t level, uint64_t price, uint64_t size, uint64_t ts) {
        if (level < MAX_LEVELS) {
            const uint32_t next = 1u - active_.load(std::memory_order_acquire);
            auto& buf = buffers_[next];
            buf.bids[level] = {price, size, ts};
            buf.bids[level].timestamp = ts;
            buf.sequence = sequence_.load(std::memory_order_relaxed) + 1;
            sequence_.store(buf.sequence, std::memory_order_relaxed);
            active_.store(next, std::memory_order_release);
        }
    }

    /**
     * Update an ask level with zero allocation (double-buffer + swap).
     * P0 fix: Now uses double-buffer write + atomic swap.
     */
    void update_ask(uint32_t level, uint64_t price, uint64_t size, uint64_t ts) {
        if (level < MAX_LEVELS) {
            const uint32_t next = 1u - active_.load(std::memory_order_acquire);
            auto& buf = buffers_[next];
            buf.asks[level] = {price, size, ts};
            buf.asks[level].timestamp = ts;
            buf.sequence = sequence_.load(std::memory_order_relaxed) + 1;
            sequence_.store(buf.sequence, std::memory_order_relaxed);
            active_.store(next, std::memory_order_release);
        }
    }

    // ─── Read from active double-buffer (lock-free, O(1)) ─────────────────────
    inline const Level2Entry& get_bid(uint32_t level) const {
        const auto& buf = buffers_[active_.load(std::memory_order_acquire)];
        static const Level2Entry empty_entry = {0, 0, 0};
        return (level < MAX_LEVELS) ? buf.bids[level] : empty_entry;
    }

    inline const Level2Entry& get_ask(uint32_t level) const {
        const auto& buf = buffers_[active_.load(std::memory_order_acquire)];
        static const Level2Entry empty_entry = {0, 0, 0};
        return (level < MAX_LEVELS) ? buf.asks[level] : empty_entry;
    }

<<<<<<< Updated upstream
private:
    alignas(64) std::array<Level2Entry, MAX_LEVELS> bids_;
    alignas(64) std::array<Level2Entry, MAX_LEVELS> asks_;
    alignas(64) std::atomic<uint64_t> sequence_;
=======
    inline uint64_t get_sequence() const {
        return buffers_[active_.load(std::memory_order_acquire)].sequence;
    }

    /**
     * is_stale() — O(1) staleness check.
     * Returns true if the latest book timestamp is older than max_age_ns.
     * A book with no published data (timestamp=0) is always stale.
     */
    inline bool is_stale(uint64_t max_age_ns = 90'000'000'000ULL) const {
        const auto& buf = buffers_[active_.load(std::memory_order_acquire)];
        if (buf.timestamp == 0) return true;
        uint64_t now = current_time_ns();
        return (now - buf.timestamp) > max_age_ns;
    }

    /**
     * Walk asks to compute VWAP up to max_entry_price for a given stake.
     * O(N) with small N, used in cold path (risk evaluation).
     */
    struct VwapResult {
        uint64_t vwap_price;   // Weighted average price
        uint64_t fillable_usd; // Total USD fillable within maxEntry
    };

    VwapResult walk_asks(uint64_t max_entry_price, uint64_t stake_usd) const {
        const auto& buf = buffers_[active_.load(std::memory_order_acquire)];
        uint64_t usd_available = 0;
        uint64_t cost = 0;
        uint64_t units_remaining = stake_usd;

        for (size_t i = 0; i < MAX_LEVELS && units_remaining > 0; i++) {
            const auto& ask = buf.asks[i];
            if (ask.price == 0 || ask.size == 0) break;
            if (ask.price > max_entry_price) break;

            uint64_t level_usd = ask.price * ask.size;
            usd_available += level_usd;

            if (units_remaining > 0) {
                uint64_t take = (units_remaining < level_usd) ? units_remaining : level_usd;
                cost += take * ask.price / 1'000'000ULL;
                units_remaining -= take;
            }
        }

        if (usd_available == 0) return {0, 0};
        uint64_t vwap = usd_available / (stake_usd > 0 ? stake_usd : 1);
        return {vwap, usd_available};
    }
>>>>>>> Stashed changes
};

#endif // ORDER_BOOK_HPP
