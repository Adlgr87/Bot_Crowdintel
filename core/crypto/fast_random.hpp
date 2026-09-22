#ifndef FAST_RANDOM_HPP
#define FAST_RANDOM_HPP

// ─────────────────────────────────────────────────────────────────────────────
// FastRandom: hardware-entropy salt generator (replaces the old NonceManager).
//
// Why: CLOB V2 removed the on-chain nonce system — order uniqueness comes from
// `timestamp` (ms) + a random `salt`. A salt must therefore be a fresh random
// value per order, not a counter (counters are replayable/guessable).
//
// RDRAND (~1-2 ns) when available; fallback is a ChaCha-style CSPRNG seeded
// from multiple entropy sources (TSC, monotonic clock, ASLR addresses).
// Either way: zero syscalls in steady state, zero allocation.
// Single instance per thread (no internal locking — salts are per-thread by
// design; cross-thread collision probability is cryptographically negligible).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstddef>
#include <ctime>

#if defined(__x86_64__) || defined(__i386__)
  #if defined(__RDRND__)
    #include <immintrin.h>
    #define CROWDINTEL_HAS_RDRAND 1
  #endif
#endif

class FastRandom {
public:
    FastRandom() {
#ifdef CROWDINTEL_HAS_RDRAND
        // Verify RDRAND actually works on this CPU once; fall back otherwise.
        unsigned long long probe;
        hw_ok_ = __builtin_ia32_rdrand64_step(&probe) == 1;
#endif
        if (!hw_ok_) mix_seed();
    }

    // 64-bit random value.
    inline uint64_t next_u64() {
#ifdef CROWDINTEL_HAS_RDRAND
        if (hw_ok_) {
            unsigned long long v;
            for (int tries = 0; tries < 8; ++tries)
                if (__builtin_ia32_rdrand64_step(&v) == 1) return v;
            hw_ok_ = false;  // hardware stopped cooperating — drop to fallback
            mix_seed();
        }
#endif
        return chacha_next();
    }

    // 56-bit random salt (decimal JSON integer, non-negative, < 2^63).
    inline uint64_t next_salt() { return next_u64() & 0x00FFFFFFFFFFFFFFULL; }

private:
    void mix_seed() {
        const uint64_t tsc = __builtin_ia32_rdtsc();
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t stack_addr = (uint64_t)(uintptr_t)&ts;
        state_[0] ^= tsc;
        state_[1] ^= (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20);
        state_[2] ^= stack_addr;
        state_[3] ^= 0x9E3779B97F4A7C15ULL * (tsc | 1);
        state_[0] ^= (uint64_t)(uintptr_t)this;
        seeded_ = true;
    }

    uint64_t chacha_next() {
        if (!seeded_) mix_seed();
        // 4 double-rounds of the ChaCha permutation on 64-bit words. Salt
        // values are public; uniqueness/unpredictability is what matters here.
        uint64_t x0 = state_[0], x1 = state_[1], x2 = state_[2], x3 = state_[3];
        for (int r = 0; r < 4; ++r) {
            x0 += x1; x3 ^= x0; x3 = (x3 << 32) | (x3 >> 32);
            x2 += x3; x1 ^= x2; x1 = (x1 << 25) | (x1 >> 39);
            x0 += x1; x3 ^= x0; x3 = (x3 << 16) | (x3 >> 48);
            x2 += x3; x1 ^= x2; x1 = (x1 << 11) | (x1 >> 53);
        }
        state_[0] = x0; state_[1] = x1; state_[2] = x2; state_[3] = x3;
        return x0 ^ x2;
    }

    alignas(64) uint64_t state_[4] = {0x61707865ULL, 0x3320646eULL, 0x79622d32ULL, 0x6b206574ULL};
    bool seeded_ = false;
    bool hw_ok_ = false;
};

// Single entropy word for one-shot consumers (e.g. context randomization).
inline uint64_t crowdintel_hw_seed_word() {
    FastRandom r;
    return r.next_u64();
}

#endif // FAST_RANDOM_HPP
