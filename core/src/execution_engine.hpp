#ifndef EXECUTION_ENGINE_HPP
#define EXECUTION_ENGINE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// ExecutionEngine: the hot path.
//
// On each tick: pop one CrowdIntel signal → apply economic filters against the
// LIVE book (edge, liquidity) → size with exact Kelly → build a CLOB V2 order
// → sign (pre-signed pool hit, or inline ECDSA fallback) → submit.
//
// Contract & correlation guarantees:
//   - The signal carries p_win; the engine pairs it with the live best price.
//     A signal without a two-sided book is never traded (no price-0 orders).
//   - BUY consumes the best ASK, SELL consumes the best BID — the levels the
//     order would actually hit.
//   - salt/timestamp are unique per order (FastRandom + wall clock ms).
//   - No heap allocation, no I/O syscalls, no logging on this path.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>
#include <ctime>

#include "../include/order_book.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../crypto/eip712_signer.hpp"
#include "../crypto/fast_random.hpp"
#include "alpha_receiver.hpp"
#include "kelly_engine.hpp"
#include "polymarket_order.hpp"
#include "presigned_pool.hpp"
// NOTE: the Client is a template parameter (duck-typed submit(body)); this
// header deliberately does not include the network client so offline builds
// never pull curl/OpenSSL.

enum class TickResult : int {
    NO_SIGNAL = 0,       // nothing to do (not an error)
    FILTERED_STATS,      // q/confidence/p_win rejected
    NO_BOOK,             // no live two-sided market
    NO_EDGE,             // |p_win − price| below min_edge
    TOO_SMALL,           // position below min size / rounds to zero
    SIGN_FAILED,         // ECDSA failure (should never happen)
    BODY_FAILED,         // wire body overflow (should never happen)
    SUBMIT_FAILED,       // network/CLOB rejected
    SUBMITTED
};

inline const char* tick_result_name(TickResult r) {
    switch (r) {
        case TickResult::NO_SIGNAL:      return "no_signal";
        case TickResult::FILTERED_STATS: return "filtered_stats";
        case TickResult::NO_BOOK:        return "no_book";
        case TickResult::NO_EDGE:        return "no_edge";
        case TickResult::TOO_SMALL:      return "too_small";
        case TickResult::SIGN_FAILED:    return "sign_failed";
        case TickResult::BODY_FAILED:    return "body_failed";
        case TickResult::SUBMIT_FAILED:  return "submit_failed";
        case TickResult::SUBMITTED:      return "submitted";
    }
    return "?";
}

template <typename Client>
class ExecutionEngine {
public:
    ExecutionEngine(const MarketConfig& cfg,
                    OrderBookL2& book,
                    SPSC_RingBuffer<AlphaSignal>& signals,
                    const EIP712Signer& signer,
                    PresignedOrderPool& pool,
                    Client& client)
        : cfg_(cfg), book_(book), signals_(signals), signer_(signer),
          pool_(pool), client_(client) {}

    // One hot tick. Returns what happened (benchmarked and counted).
    TickResult run_tick() {
        AlphaSignal sig;
        if (!signals_.try_pop(sig)) return TickResult::NO_SIGNAL;

        // ── 1. Statistical pre-filters (defensive repeat of the cold path). ──
        if (!(sig.p_win > 0.0 && sig.p_win < 1.0)) return TickResult::FILTERED_STATS;
        if (sig.confidence < cfg_.min_confidence || sig.q_value > cfg_.max_q_value)
            return TickResult::FILTERED_STATS;

        // ── 2. Live top of book (seqlock-guarded; a few retries suffice — the
        //       producer updates at most every few hundred µs). ───────────────
        OrderBookL2::Top top;
        bool have_book = false;
        for (int attempt = 0; attempt < 4 && !have_book; ++attempt)
            have_book = book_.read_top(top);
        if (!have_book || top.bid.size == 0 || top.ask.size == 0)
            return TickResult::NO_BOOK;

        // ── 3. Direction & price actually hit. ───────────────────────────────
        uint8_t side;
        if (sig.direction_hint == 1) {
            side = K_SIDE_SELL;
        } else {
            // Default: trade the direction of the edge.
            const double buy_edge  = sig.p_win - (double)top.ask.price * 1e-6;
            const double sell_edge = (double)top.bid.price * 1e-6 - sig.p_win;
            side = (buy_edge >= sell_edge) ? K_SIDE_BUY : K_SIDE_SELL;
        }
        const uint64_t price_raw = round_price_to_tick(
            (side == K_SIDE_BUY) ? top.ask.price : top.bid.price, cfg_.tick_size);
        const double price = (double)price_raw * 1e-6;

        // ── 4. Edge filter (economic, needs the live price). ─────────────────
        const double edge = (side == K_SIDE_BUY)
            ? (sig.p_win - price)
            : (price - sig.p_win);
        if (edge < cfg_.min_edge) return TickResult::NO_EDGE;

        // ── 5. Exact Kelly sizing (×1e6 fixed shares). ───────────────────────
        const double k = (side == K_SIDE_BUY)
            ? KellyEngine::kelly_buy(sig.p_win, price)
            : KellyEngine::kelly_sell(sig.p_win, price);
        const double usd = KellyEngine::position_usd(k, cfg_.kelly_fraction, cfg_.bankroll_usd);
        uint64_t shares = KellyEngine::usd_to_shares_fixed(usd, price);
        if (shares < cfg_.min_size_shares) return TickResult::TOO_SMALL;
        // Never exceed visible liquidity at the level we take (partial take is
        // fine; blowing through the book is not — naive version).
        const uint64_t avail = (side == K_SIDE_BUY) ? top.ask.size : top.bid.size;
        if (shares > avail) shares = avail;
        if (shares < cfg_.min_size_shares) return TickResult::TOO_SMALL;

        uint64_t maker_amt, taker_amt;
        if (!compute_amounts(side, price_raw, shares, maker_amt, taker_amt))
            return TickResult::TOO_SMALL;

        // ── 6. Try the pre-signed pool (≈1 µs), else inline sign (~3-5 µs). ──
        WireBody body;
        bool ok = pool_.acquire(side, price_raw, shares, body);
        if (!ok) {
            OrderV2 o{};
            o.salt = rng_.next_salt();
            o.timestamp_ms = PresignedOrderPool::now_ms();
            std::memcpy(o.maker, cfg_.maker, 20);
            std::memcpy(o.signer, cfg_.signer, 20);
            std::memcpy(o.token_id, cfg_.token_id_be, 32);
            o.maker_amount = maker_amt;
            o.taker_amount = taker_amt;
            o.side = side;
            o.signature_type = cfg_.signature_type;

            uint8_t sig65[65];
            if (!signer_.sign_order(o, sig65)) return TickResult::SIGN_FAILED;
            if (!build_wire_body(o, sig65, cfg_.token_id_dec, cfg_.maker_hex,
                                 cfg_.signer_hex, cfg_.owner_api_key,
                                 cfg_.order_type, body))
                return TickResult::BODY_FAILED;
        }

        // ── 7. Submit (HMAC + TLS write on the persistent connection). ───────
        const auto res = client_.submit(body);
        if (res.ok) {
            stats_.submitted.fetch_add(1, std::memory_order_relaxed);
            return TickResult::SUBMITTED;
        }
        stats_.submit_failed.fetch_add(1, std::memory_order_relaxed);
        return TickResult::SUBMIT_FAILED;
    }

    struct Stats {
        std::atomic<uint64_t> submitted{0};
        std::atomic<uint64_t> submit_failed{0};
    };
    const Stats& stats() const { return stats_; }

private:
    const MarketConfig& cfg_;
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& signals_;
    const EIP712Signer& signer_;
    PresignedOrderPool& pool_;
    Client& client_;
    FastRandom rng_;
    Stats stats_;
};

#endif // EXECUTION_ENGINE_HPP
