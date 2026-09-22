#ifndef PRESIGNED_POOL_HPP
#define PRESIGNED_POOL_HPP

// ─────────────────────────────────────────────────────────────────────────────
// PresignedOrderPool: ECDSA signatures computed BEFORE the signal arrives.
//
// The single most expensive step of tick-to-wire is the per-order ECDSA
// (~2-5 µs with libsecp256k1, ~25-45 µs naive). But on a prediction market
// the plausible trade set is tiny: take the best ask (or bid) at one of a few
// price levels near the mid, at roughly the Kelly size. So a COLD thread
// continuously pre-builds and pre-signs orders over a price grid around the
// mid; the HOT path, on signal, does a linear scan over a handful of slots
// and — on a hit — submits an already-signed order. Tick-to-wire drops from
// "keccak + ECDSA + JSON" (~5-45 µs) to "scan + memcpy + HMAC" (~1-2 µs).
//
// Freshness: every rebuild stamps new salt + timestamp_ms; slots older than
// the TTL are not served (the CLOB treats timestamp as the uniqueness source;
// keeping it current also keeps orders from looking stale).
//
// Concurrency: double-buffered. The producer fills the inactive set, then
// flips an atomic index (release). The consumer reads the active set and a
// slot is only ever replaced wholesale after a flip — no torn reads.
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "../crypto/eip712_signer.hpp"
#include "../crypto/fast_random.hpp"
#include "polymarket_order.hpp"

struct PresignedSlot {
    uint64_t price;        // ×1e6
    uint64_t size;         // ×1e6
    uint8_t  side;
    uint8_t  valid;
    uint16_t _pad0;
    uint32_t _pad1;
    uint64_t built_ms;
    WireBody body;         // fully built wire JSON incl. signature
};

class PresignedOrderPool {
public:
    static constexpr size_t LEVELS_PER_SIDE = 8;   // ±8 ticks around the mid
    static constexpr size_t SLOT_COUNT = LEVELS_PER_SIDE * 2;

    PresignedOrderPool(const MarketConfig& cfg,
                       const EIP712Signer& signer,
                       uint64_t ttl_ms)
        : cfg_(cfg), signer_(signer), ttl_ms_(ttl_ms) {}

    // ── Cold path: rebuild the inactive buffer and flip. ─────────────────────
    // grid_center: ×1e6 price around which the grid is laid out (the mid).
    // shares:      ×1e6 share size to pre-sign (Kelly-sized by the caller).
    void rebuild(uint64_t grid_center, uint64_t shares) {
        const uint32_t next = 1u - active_.load(std::memory_order_relaxed);
        auto& dst = buffers_[next];
        size_t n = 0;
        FastRandom rng;  // fresh salts per slot
        const uint64_t now = now_ms();

        for (int s = 0; s < 2; ++s) {
            const uint8_t side = (s == 0) ? K_SIDE_BUY : K_SIDE_SELL;
            for (size_t l = 0; l < LEVELS_PER_SIDE; ++l) {
                // Taker grid: a BUY takes the ASK side of the book (prices at
                // or above the mid); a SELL takes the BID side (at or below).
                const int64_t off = (int64_t)(l + 1);
                int64_t px = (int64_t)grid_center + (side == K_SIDE_BUY ? +off : -off) *
                             (int64_t)cfg_.tick_size;
                if (px <= 0 || px >= 1000000) continue;
                const uint64_t price = round_price_to_tick((uint64_t)px, cfg_.tick_size);

                uint64_t maker_amt, taker_amt;
                if (!compute_amounts(side, price, shares, maker_amt, taker_amt)) continue;

                OrderV2 o{};
                o.salt = rng.next_salt();
                o.timestamp_ms = now;
                std::memcpy(o.maker, cfg_.maker, 20);
                std::memcpy(o.signer, cfg_.signer, 20);
                std::memcpy(o.token_id, cfg_.token_id_be, 32);
                o.maker_amount = maker_amt;
                o.taker_amount = taker_amt;
                o.side = side;
                o.signature_type = cfg_.signature_type;

                uint8_t sig[65];
                if (!signer_.sign_order(o, sig)) continue;

                PresignedSlot slot;
                slot.price = price;
                slot.size = shares;
                slot.side = side;
                slot.valid = 1;
                slot._pad0 = 0; slot._pad1 = 0;
                slot.built_ms = now;
                if (!build_wire_body(o, sig, cfg_.token_id_dec, cfg_.maker_hex,
                                     cfg_.signer_hex, cfg_.owner_api_key,
                                     cfg_.order_type, slot.body))
                    continue;
                dst[n++] = slot;
            }
        }
        built_count_ = n;
        active_.store(next, std::memory_order_release);   // publish
    }

    // ── Hot path: find a fresh pre-signed order for (side, price, size). ─────
    // On hit, copies the wire body into `out` and returns true.
    inline bool acquire(uint8_t side, uint64_t price, uint64_t size,
                        WireBody& out) const {
        const auto& slots = buffers_[active_.load(std::memory_order_acquire)];
        const uint64_t deadline = now_ms();               // vDSO ~20ns
        for (size_t i = 0; i < built_count_; ++i) {
            const auto& s = slots[i];
            if (s.valid && s.side == side && s.price == price && s.size == size &&
                deadline - s.built_ms <= ttl_ms_) {
                std::memcpy(&out, &s.body, sizeof(WireBody));
                return true;
            }
        }
        return false;
    }

    size_t built_count() const { return built_count_; }
    static uint64_t now_ms() {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    }

private:
    const MarketConfig& cfg_;
    const EIP712Signer& signer_;
    uint64_t ttl_ms_;
    std::atomic<uint32_t> active_{0};
    std::array<PresignedSlot, SLOT_COUNT> buffers_[2] = {};
    size_t built_count_ = 0;   // count in the (last-built) buffer
};

#endif // PRESIGNED_POOL_HPP
