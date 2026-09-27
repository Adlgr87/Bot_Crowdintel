#ifndef ORDER_BOOK_HPP
#pragma GCC optimize("O3,unroll-loops,fast-math")
#define ORDER_BOOK_HPP
#include <cstdint>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
inline uint64_t current_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Level2Entry { uint64_t price, size, timestamp; } __attribute__((packed));
class OrderBookL2 {
public:
    static constexpr size_t MAX_LEVELS = 100;
private:
    struct BookBuffer {
        std::array<Level2Entry, MAX_LEVELS> bids, asks;
        uint64_t timestamp, sequence;
    };
    alignas(64) std::array<BookBuffer, 2> buffers_;
    alignas(64) std::atomic<uint32_t> active_;
    alignas(64) std::atomic<uint64_t> sequence_;
public:
    OrderBookL2() : active_(0), sequence_(0) {
        for (auto& buf : buffers_) {
            for (size_t i = 0; i < MAX_LEVELS; i++) { buf.bids[i] = {0,0,0}; buf.asks[i] = {0,0,0}; }
            buf.timestamp = 0; buf.sequence = 0;
        }
        sequence_.store(0, std::memory_order_relaxed);
    }
    void set_book(const Level2Entry* bids, size_t nb, const Level2Entry* asks, size_t na, uint64_t ts_ns = 0) {
        const uint32_t next = 1u - active_.load(std::memory_order_acquire);
        auto& buf = buffers_[next];
        size_t i = 0;
        for (; i < nb && i < MAX_LEVELS; i++) { buf.bids[i] = bids[i]; buf.bids[i].timestamp = ts_ns; }
        for (; i < MAX_LEVELS; i++) { buf.bids[i] = {0,0,0}; }
        i = 0;
        for (; i < na && i < MAX_LEVELS; i++) { buf.asks[i] = asks[i]; buf.asks[i].timestamp = ts_ns; }
        for (; i < MAX_LEVELS; i++) { buf.asks[i] = {0,0,0}; }
        buf.timestamp = ts_ns; buf.sequence = sequence_.load(std::memory_order_relaxed) + 1;
        sequence_.store(buf.sequence, std::memory_order_relaxed);
        active_.store(next, std::memory_order_release);
    }
    void update_bid(uint32_t level, uint64_t price, uint64_t size, uint64_t ts) {
        if (level < MAX_LEVELS) {
            const uint32_t next = 1u - active_.load(std::memory_order_acquire);
            auto& buf = buffers_[next];
            buf.bids[level] = {price,size,ts};
            buf.sequence = sequence_.load(std::memory_order_relaxed) + 1;
            sequence_.store(buf.sequence, std::memory_order_relaxed);
            active_.store(next, std::memory_order_release);
        }
    }
    void update_ask(uint32_t level, uint64_t price, uint64_t size, uint64_t ts) {
        if (level < MAX_LEVELS) {
            const uint32_t next = 1u - active_.load(std::memory_order_acquire);
            auto& buf = buffers_[next];
            buf.asks[level] = {price,size,ts};
            buf.sequence = sequence_.load(std::memory_order_relaxed) + 1;
            sequence_.store(buf.sequence, std::memory_order_relaxed);
            active_.store(next, std::memory_order_release);
        }
    }
    inline const Level2Entry& get_bid(uint32_t level) const {
        const auto& buf = buffers_[active_.load(std::memory_order_acquire)];
        static const Level2Entry empty_entry = {0,0,0};
        return (level < MAX_LEVELS) ? buf.bids[level] : empty_entry;
    }
    inline const Level2Entry& get_ask(uint32_t level) const {
        const auto& buf = buffers_[active_.load(std::memory_order_acquire)];
        static const Level2Entry empty_entry = {0,0,0};
        return (level < MAX_LEVELS) ? buf.asks[level] : empty_entry;
    }
    inline uint64_t get_sequence() const { return buffers_[active_.load(std::memory_order_acquire)].sequence; }
    inline bool is_stale(uint64_t max_age_ns = 90'000'000'000ULL) const {
        const auto& buf = buffers_[active_.load(std::memory_order_acquire)];
        if (buf.timestamp == 0) return true;
        return (current_time_ns() - buf.timestamp) > max_age_ns;
    }
    struct VwapResult { uint64_t vwap_price, fillable_usd; };
    VwapResult walk_asks(uint64_t max_entry_price, uint64_t stake_usd) const {
        const auto& buf = buffers_[active_.load(std::memory_order_acquire)];
        uint64_t usd_available = 0, units_remaining = stake_usd;
        for (size_t i = 0; i < MAX_LEVELS && units_remaining > 0; i++) {
            const auto& ask = buf.asks[i];
            if (ask.price == 0 || ask.size == 0) break;
            if (ask.price > max_entry_price) break;
            usd_available += ask.price * ask.size;
            if (units_remaining > 0) {
                uint64_t take = (units_remaining < ask.price * ask.size) ? units_remaining : ask.price * ask.size;
                units_remaining -= take;
            }
        }
        if (usd_available == 0) return {0,0};
        uint64_t vwap = usd_available / (stake_usd > 0 ? stake_usd : 1);
        return {vwap, usd_available};
    }
};
#endif
