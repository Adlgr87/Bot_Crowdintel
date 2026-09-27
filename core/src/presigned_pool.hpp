#ifndef PRESIGNED_POOL_HPP
#define PRESIGNED_POOL_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>

#include "eip712_signer.hpp"
#include "order_book.hpp"

/**
 * PresignedOrderPool: Lock-free pool of pre-signed orders with validity tracking.
 *
 * P0 fixes applied (from DEEP_AUDIT_2026-09-26.es.md):
 *   - Double-buffer publication with atomic index swap (seqlock-style)
 *   - One-shot acquire semantics (slot marked consumed after use)
 *   - Atomic pool_hit / pool_miss / inline_fallback counters with hit_rate()
 *   - No mutex in hot path (acquire/rebuild are lock-free)
 *
 * When the order book moves more than max_price_deviation_bps, orders in the pool
 * are marked INVALID and should be cancelled/replaced (T3-5).
 *
 * Architecture:
 *   - Producer (cold path): add_order() / rebuild() writes to inactive buffer,
 *     then atomically swaps the active_ index.
 *   - Consumer (hot path): acquire() reads through active_ index with acquire
 *     semantics, iterates slots, and marks consumed atomically.
 *
 * CRITICAL: PresignedOrderPool is NOT trivially copyable due to atomics in Slot.
 * For the hot path SPSC_RingBuffer, AlphaSignal is used instead (trivially copyable).
 */

struct PresignedOrder {
    uint64_t nonce;
    uint64_t salt;
    std::array<uint8_t, 65> signature;
    std::string payload;             // EIP-712 struct data (JSON)
    uint64_t price;              // Price at which this order was signed (* 1e6)
    uint64_t size;               // Size (* 1e6)
    uint8_t side;                // 0 = buy, 1 = sell
    std::chrono::steady_clock::time_point created_at;
    bool valid;
    std::string client_order_id;    // Maps to exchange order ID on fill
};

/**
 * Slot in the PresignedBuffer — wraps a PresignedOrder with atomic metadata
 * for lock-free access by hot path acquire().
 */
struct PresignedSlot {
    PresignedOrder order;
    std::atomic<bool> consumed;  // P0.3: One-shot — true if already acquired this epoch
    std::atomic<bool> valid;     // P0.3: True if order is still valid (not price-stale)
    std::atomic<bool> has_data;  // P0.3: True if this slot holds a presigned order

    PresignedSlot() : consumed(true), valid(false), has_data(false) {}
    // Not copyable — atomics require explicit handling.
    PresignedSlot(const PresignedSlot&) = delete;
    PresignedSlot& operator=(const PresignedSlot&) = delete;
    // Movable for std::array swap (but not needed in practice)
};

/**
 * Double-buffered book snapshot for the presigned order pool.
 * Written by producer (cold path), read by consumer (hot path).
 * Published atomically via PresignedOrderPool::active_ index swap.
 */
struct PresignedBuffer {
    static constexpr size_t SLOT_COUNT = 256;

    std::array<PresignedSlot, SLOT_COUNT> slots;
    size_t count;             // Number of slots with data (written by producer)
    uint64_t epoch;           // Publication counter for debugging/tracing

    PresignedBuffer() : count(0), epoch(0) {}
};

class PresignedOrderPool {
public:
    explicit PresignedOrderPool(int max_price_deviation_bps = 500)
        : max_price_deviation_bps_(max_price_deviation_bps),
          active_(0),
          pool_hits_(0),
          pool_misses_(0),
          inline_fallbacks_(0) {}

    /**
     * rebuild() — Atomically replace all presigned orders.
     * P0 fix: Double-buffer publication — writes to inactive buffer,
     * increments epoch, then swaps active_ index atomically.
     * Consumer (acquire) reads active_ with acquire semantics, ensuring
     * it never sees a partially-written buffer.
     *
     * Cold path — called infrequently during book rebuild.
     */
    void rebuild(const std::vector<PresignedOrder>& orders) {
        const uint32_t next = 1u - active_.load(std::memory_order_relaxed);
        auto& buf = buffers_[next];

        buf.count = 0;
        for (size_t i = 0; i < SLOT_COUNT && i < orders.size(); i++) {
            const auto& o = orders[i];
            buf.slots[i].order = o;
            buf.slots[i].has_data.store(true, std::memory_order_release);
            buf.slots[i].consumed.store(false, std::memory_order_release);
            buf.slots[i].valid.store(o.valid, std::memory_order_release);
            buf.count++;
        }
        buf.epoch++;

        // Atomic publish — consumer reads active_ with acquire, then buffer
        active_.store(next, std::memory_order_release);
    }

    /**
     * add_order() — Add a single order to the pool.
     * P0 fix: Copies existing orders from active buffer to inactive buffer,
     * appends new order, and publishes atomically.
     *
     * Cold path — for incremental order additions.
     */
    void add_order(const std::string& client_order_id,
                   const PresignedOrder& order) {
        uint32_t cur_active = active_.load(std::memory_order_relaxed);
        uint32_t next = 1u - cur_active;
        auto& buf = buffers_[next];
        const auto& cur_buf = buffers_[cur_active];

        // Copy existing orders from active buffer to inactive buffer
        buf.count = cur_buf.count;
        for (size_t i = 0; i < buf.count && i < SLOT_COUNT; i++) {
            buf.slots[i].order = cur_buf.slots[i].order;
            buf.slots[i].consumed.store(
                cur_buf.slots[i].consumed.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            buf.slots[i].valid.store(
                cur_buf.slots[i].valid.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            buf.slots[i].has_data.store(
                cur_buf.slots[i].has_data.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }

        // Append new order
        if (buf.count < SLOT_COUNT) {
            buf.slots[buf.count].order = order;
            buf.slots[buf.count].has_data.store(true, std::memory_order_release);
            buf.slots[buf.count].consumed.store(false, std::memory_order_release);
            buf.slots[buf.count].valid.store(order.valid, std::memory_order_release);
            buf.count++;
        }
        buf.epoch++;

        // Publish immediately: consumer reads active_ then buffer
        active_.store(next, std::memory_order_release);
    }

    /**
     * acquire() — Find and consume a presigned order matching the given params.
     * P0 fix: Lock-free hot path using atomic index load.
     * One-shot semantics: once acquired, slot is marked consumed and cannot
     * be acquired again until the next rebuild().
     *
     * Returns pointer to the order, or nullptr if no match.
     * Hot path — called on every tick when pool is active.
     */
    const PresignedOrder* acquire(uint8_t side, uint64_t price, uint64_t size,
                                  int max_dev_bps = 100) const {
        const uint32_t idx = active_.load(std::memory_order_acquire);
        const auto& buf = buffers_[idx];

        for (size_t i = 0; i < buf.count; i++) {
            const auto& slot = buf.slots[i];

            // P0.3: Skip already consumed slots (one-shot)
            if (slot.consumed.load(std::memory_order_acquire)) continue;

            // Skip slots without data
            if (!slot.has_data.load(std::memory_order_acquire)) continue;

            // Skip invalid (price-stale) orders
            if (!slot.valid.load(std::memory_order_acquire)) continue;

            // Match side
            if (slot.order.side != side) continue;

            // Match size (exact match for presigned orders)
            if (slot.order.size != size) continue;

            // Check price deviation
            if (slot.order.price > 0) {
                double dev_bps = std::abs(static_cast<double>(slot.order.price) -
                                          static_cast<double>(price)) /
                                 static_cast<double>(slot.order.price) * 10000.0;
                if (dev_bps > max_dev_bps) continue;
            }

            // Found a match — mark as consumed atomically (one-shot)
            // Note: const_cast is safe here — slot is from mutable member buffers_
            const_cast<std::atomic<bool>&>(slot.consumed)
                .store(true, std::memory_order_release);
            pool_hits_.fetch_add(1, std::memory_order_relaxed);
            return &slot.order;
        }

        pool_misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    /**
     * check_and_invalidate() — Mark orders stale if book moved beyond threshold.
     * P0 fix: Reads active buffer atomically, no mutex on hot path.
     *
     * Cold path — called when new book snapshot arrives.
     */
    void check_and_invalidate(const Level2Entry& current_best_bid,
                              const Level2Entry& current_best_ask) {
        uint64_t bid_price = current_best_bid.price;
        uint64_t ask_price = current_best_ask.price;

        // Atomically read the active buffer index
        uint32_t idx = active_.load(std::memory_order_acquire);
        auto& buf = buffers_[idx];

        for (size_t i = 0; i < buf.count; i++) {
            auto& slot = buf.slots[i];
            if (!slot.has_data.load(std::memory_order_acquire) ||
                slot.consumed.load(std::memory_order_acquire)) continue;

            uint64_t current_ref = slot.order.side == 0 ? ask_price : bid_price;
            uint64_t order_ref = slot.order.price;

            if (order_ref > 0 && current_ref > 0) {
                double deviation_bps =
                    std::abs(static_cast<double>(current_ref) -
                             static_cast<double>(order_ref)) /
                    static_cast<double>(order_ref) * 10000.0;
                if (deviation_bps > static_cast<double>(max_price_deviation_bps_)) {
                    slot.valid.store(false, std::memory_order_release);
                }
            }
        }
    }

    /**
     * invalidate_all() — Emergency invalidate all orders (kill switch).
     * P0 fix: Lock-free using atomic stores.
     */
    void invalidate_all() {
        uint32_t idx = active_.load(std::memory_order_acquire);
        auto& buf = buffers_[idx];
        for (size_t i = 0; i < buf.count; i++) {
            buf.slots[i].valid.store(false, std::memory_order_release);
            buf.slots[i].consumed.store(true, std::memory_order_release);
        }
    }

    /**
     * remove_order() — Mark a specific order as removed.
     * P0 fix: Lock-free. Does not decrement count (slot remains in buffer
     * but is marked has_data=false, consumed=true).
     */
    void remove_order(const std::string& client_order_id) {
        uint32_t idx = active_.load(std::memory_order_acquire);
        auto& buf = buffers_[idx];
        for (size_t i = 0; i < buf.count; i++) {
            if (buf.slots[i].order.client_order_id == client_order_id) {
                buf.slots[i].has_data.store(false, std::memory_order_release);
                buf.slots[i].consumed.store(true, std::memory_order_release);
                break;
            }
        }
    }

    /**
     * Size — number of non-removed orders in the active buffer.
     * Counts slots where has_data is true (remove_order sets has_data=false).
     */
    size_t size() const {
        uint32_t idx = active_.load(std::memory_order_acquire);
        const auto& buf = buffers_[idx];
        size_t count = 0;
        for (size_t i = 0; i < buf.count; i++) {
            if (buf.slots[i].has_data.load(std::memory_order_acquire)) {
                count++;
            }
        }
        return count;
    }

    /**
     * Valid count — number of orders that are not consumed, have data, and are valid.
     * P0 fix: Lock-free with atomic loads.
     */
    size_t valid_count() const {
        uint32_t idx = active_.load(std::memory_order_acquire);
        const auto& buf = buffers_[idx];
        size_t count = 0;
        for (size_t i = 0; i < buf.count; i++) {
            if (buf.slots[i].has_data.load(std::memory_order_acquire) &&
                !buf.slots[i].consumed.load(std::memory_order_acquire) &&
                buf.slots[i].valid.load(std::memory_order_acquire)) {
                count++;
            }
        }
        return count;
    }

    /**
     * Get all valid order IDs (for cancellation when pool is invalidated).
     * P0 fix: Lock-free, reads atomically-published buffer.
     */
    std::vector<std::string> get_valid_order_ids() const {
        std::vector<std::string> result;
        uint32_t idx = active_.load(std::memory_order_acquire);
        const auto& buf = buffers_[idx];
        for (size_t i = 0; i < buf.count; i++) {
            const auto& slot = buf.slots[i];
            if (slot.has_data.load(std::memory_order_acquire) &&
                !slot.consumed.load(std::memory_order_acquire) &&
                slot.valid.load(std::memory_order_acquire)) {
                result.push_back(slot.order.client_order_id);
            }
        }
        return result;
    }

    /**
     * P0.3 metrics: Pool hit rate = hits / (hits + misses).
     * Returns 0.0 if no attempts have been made.
     */
    double pool_hit_rate() const {
        uint64_t hits = pool_hits_.load(std::memory_order_relaxed);
        uint64_t misses = pool_misses_.load(std::memory_order_relaxed);
        uint64_t total = hits + misses;
        if (total == 0) return 0.0;
        return static_cast<double>(hits) / static_cast<double>(total);
    }

    uint64_t pool_hits() const { return pool_hits_.load(std::memory_order_relaxed); }
    uint64_t pool_misses() const { return pool_misses_.load(std::memory_order_relaxed); }
    uint64_t inline_fallbacks() const { return inline_fallbacks_.load(std::memory_order_relaxed); }
    void record_inline_fallback() { inline_fallbacks_.fetch_add(1, std::memory_order_relaxed); }

    // Backward-compatible alias (used by some existing code paths)
    void set_max_price_deviation_bps(int bps) {
        max_price_deviation_bps_ = bps;
    }
    int max_price_deviation_bps() const { return max_price_deviation_bps_; }

private:
    static constexpr size_t SLOT_COUNT = PresignedBuffer::SLOT_COUNT;

    int max_price_deviation_bps_;
    std::array<PresignedBuffer, 2> buffers_;
    std::atomic<uint32_t> active_;       // 0 or 1: index into buffers_
    mutable std::atomic<uint64_t> pool_hits_;    // P0.3: metrics (mutable for const acquire)
    mutable std::atomic<uint64_t> pool_misses_;  // P0.3: metrics (mutable for const acquire)
    mutable std::atomic<uint64_t> inline_fallbacks_;

    alignas(64) char pad_[56];  // Cache-line padding to prevent false sharing
};

template <size_t N = 256>
struct PresignedPool {
    using Slot = PresignedSlot;
    static constexpr size_t CAPACITY = N < 256 ? N : 256;
    PresignedBuffer buffer;
    std::atomic<uint64_t> generation{0};

    PresignedPool() = default;
};

#endif // PRESIGNED_POOL_HPP
