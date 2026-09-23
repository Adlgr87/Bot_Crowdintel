#ifndef SPSC_RING_BUFFER_HPP
#define SPSC_RING_BUFFER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// SPSC_RingBuffer: wait-free single-producer/single-consumer queue.
//
// - Capacity is a power of two (compile-time enforced).
// - Storage is a member array (no unique_ptr indirection on every access).
// - Elements must be trivially copyable (POD) — we memcpy slots in/out, which
//   avoids placement-new/destructor bookkeeping and lets the compiler emit
//   simple mov instructions.
// - acquire/release pairing publishes/consumes slot contents; producer's
//   head counter is relaxed (only one producer), consumer's tail likewise.
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

template <typename T, size_t Capacity = 4096>
class SPSC_RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static_assert(Capacity > 1, "Capacity must be > 1");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    // Producer: returns false when full. Counters are unbounded (size_t wrap
    // is benign: Capacity is a power of two and wrap is masked consistently).
    // MutaLambda optimization: __builtin_expect marks rare branch as unlikely.
    inline bool try_push(const T& item) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (__builtin_expect(h - tail_cache_ >= Capacity - 1, 0)) {
            tail_cache_ = tail_.load(std::memory_order_acquire);  // refresh
            if (h - tail_cache_ >= Capacity - 1) return false;    // full
        }
        std::memcpy(&slots_[h & MASK], &item, sizeof(T));
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer: returns false when empty; on success copies into `out`.
    inline bool try_pop(T& out) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (__builtin_expect(t == head_cache_, 0)) {
            head_cache_ = head_.load(std::memory_order_acquire);  // refresh
            if (t == head_cache_) return false;                   // empty
        }
        std::memcpy(&out, &slots_[t & MASK], sizeof(T));
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    static constexpr size_t capacity() { return Capacity; }

private:
    static constexpr size_t MASK = Capacity - 1;

    alignas(64) std::array<uint8_t, sizeof(T) * Capacity> storage_{};
    // Slots overlay storage_ (constructed lazily via memcpy — POD only).
    T* slots_ = reinterpret_cast<T*>(storage_.data());

    alignas(64) std::atomic<size_t> head_{0};   // producer-only writes
    alignas(64) std::atomic<size_t> tail_{0};   // consumer-only writes
    size_t tail_cache_ = 0;                     // producer-side cache of tail_
    size_t head_cache_ = 0;                     // consumer-side cache of head_
};

#endif // SPSC_RING_BUFFER_HPP
