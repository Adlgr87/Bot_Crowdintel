#ifndef ORDER_BOOK_HPP
#pragma GCC optimize("O3,unroll-loops,fast-math")
#define ORDER_BOOK_HPP

#include <cstdint>
#include <array>
#include <atomic>

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

    inline const Level2Entry& get_bid(uint32_t level) const { return bids_[level]; }
    inline const Level2Entry& get_ask(uint32_t level) const { return asks_[level]; }
    inline uint64_t get_sequence() const { return sequence_.load(std::memory_order_acquire); }

private:
    alignas(64) std::array<Level2Entry, MAX_LEVELS> bids_;
    alignas(64) std::array<Level2Entry, MAX_LEVELS> asks_;
    alignas(64) std::atomic<uint64_t> sequence_;
};

#endif // ORDER_BOOK_HPP
