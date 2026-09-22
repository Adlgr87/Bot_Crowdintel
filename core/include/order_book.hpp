#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

// ─────────────────────────────────────────────────────────────────────────────
// OrderBookL2: fixed-capacity, zero-allocation Level-2 book.
//
// - Prices/sizes are uint64 fixed-point (×1e6). No floats in the hot path.
// - bids[0] is the best bid (descending), asks[0] the best ask (ascending).
// - Producer = WS listener thread (single writer). Consumer = engine thread.
//   A seqlock counter lets the consumer detect torn reads:
//     acquire-load seq (even) → read → re-load seq; retry if changed.
// - No packed structs (packed + misaligned 8-byte fields = split cache lines),
//   no pragmas that alter FP semantics (there are no floats here).
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct Level2Entry {
    uint64_t price;   // fixed-point ×1e6
    uint64_t size;    // fixed-point ×1e6 (0 = empty level)
};

class OrderBookL2 {
public:
    static constexpr size_t MAX_LEVELS = 100;

    // ── Producer API (WS listener thread) ────────────────────────────────────
    // Replace the full side (snapshot or post-sort delta application).
    // Levels must arrive pre-sorted: bids descending, asks ascending.
    void set_bids(const Level2Entry* levels, size_t n) {
        write_begin();
        if (n > MAX_LEVELS) n = MAX_LEVELS;
        for (size_t i = 0; i < n; ++i) bids_[i] = levels[i];
        for (size_t i = n; i < MAX_LEVELS; ++i) bids_[i] = {0, 0};
        write_end();
    }
    void set_asks(const Level2Entry* levels, size_t n) {
        write_begin();
        if (n > MAX_LEVELS) n = MAX_LEVELS;
        for (size_t i = 0; i < n; ++i) asks_[i] = levels[i];
        for (size_t i = n; i < MAX_LEVELS; ++i) asks_[i] = {0, 0};
        write_end();
    }

    // ── Consumer API (engine thread) — seqlock-guarded snapshot ─────────────
    struct Top {
        Level2Entry bid;
        Level2Entry ask;
        uint64_t    sequence;
    };

    // Returns false if the producer updated the book mid-read (retry).
    inline bool read_top(Top& out) const {
        const uint64_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1) return false;                    // writer active
        out.bid = bids_[0];
        out.ask = asks_[0];
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t s2 = seq_.load(std::memory_order_relaxed);
        if (s1 != s2) return false;                  // torn read
        out.sequence = s1;
        return true;
    }

    // Unguarded accessors for cold-path/benchmark use.
    inline Level2Entry get_bid(size_t level) const {
        return (level < MAX_LEVELS) ? bids_[level] : Level2Entry{0, 0};
    }
    inline Level2Entry get_ask(size_t level) const {
        return (level < MAX_LEVELS) ? asks_[level] : Level2Entry{0, 0};
    }
    inline uint64_t sequence() const { return seq_.load(std::memory_order_acquire); }

private:
    inline void write_begin() { seq_.fetch_add(1, std::memory_order_relaxed); }  // → odd
    inline void write_end()   {
        std::atomic_thread_fence(std::memory_order_release);
        seq_.fetch_add(1, std::memory_order_release);                               // → even
    }

    alignas(64) std::array<Level2Entry, MAX_LEVELS> bids_{};   // descending
    alignas(64) std::array<Level2Entry, MAX_LEVELS> asks_{};   // ascending
    alignas(64) std::atomic<uint64_t> seq_{0};                  // seqlock
};

#endif // ORDER_BOOK_HPP
