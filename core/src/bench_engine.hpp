#ifndef BENCH_ENGINE_HPP
#define BENCH_ENGINE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// BenchEngine: the benchmarking twin of ExecutionEngine.
//
// Same pipeline (signal → filters → book → Kelly → build → sign → submit) with
// a MockCLOBClient so measurements isolate CPU work from network jitter. It
// also exposes the individual stages (sign only, pool acquire only, body+HMAC
// only) so the latency bench can attribute costs precisely.
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/order_book.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../crypto/eip712_signer.hpp"
#include "../crypto/fast_random.hpp"
#include "alpha_receiver.hpp"
#include "kelly_engine.hpp"
#include "mock_client.hpp"
#include "polymarket_order.hpp"
#include "presigned_pool.hpp"

class BenchEngine {
public:
    BenchEngine(const MarketConfig& cfg, OrderBookL2& book,
                SPSC_RingBuffer<AlphaSignal>& signals,
                const EIP712Signer& signer, PresignedOrderPool& pool)
        : cfg_(cfg), book_(book), signals_(signals), signer_(signer), pool_(pool) {}

    // Full productive tick (mirrors ExecutionEngine::run_tick, mock submit).
    int run_tick() {
        AlphaSignal sig;
        if (!signals_.try_pop(sig)) return 0;

        if (!(sig.p_win > 0.0 && sig.p_win < 1.0)) return -1;
        if (sig.confidence < cfg_.min_confidence || sig.q_value > cfg_.max_q_value)
            return -1;

        OrderBookL2::Top top;
        bool have = false;
        for (int a = 0; a < 4 && !have; ++a) have = book_.read_top(top);
        if (!have || top.bid.size == 0 || top.ask.size == 0) return -2;

        const double ask = (double)top.ask.price * 1e-6;
        if (sig.p_win - ask < cfg_.min_edge) return -3;

        const double k = KellyEngine::kelly_buy(sig.p_win, ask);
        const double usd = KellyEngine::position_usd(k, cfg_.kelly_fraction, cfg_.bankroll_usd);
        uint64_t shares = KellyEngine::usd_to_shares_fixed(usd, ask);
        if (shares < cfg_.min_size_shares) return -4;
        if (shares > top.ask.size) shares = top.ask.size;
        if (shares < cfg_.min_size_shares) return -4;

        const uint64_t price_raw = round_price_to_tick(top.ask.price, cfg_.tick_size);
        WireBody body;
        if (!pool_.acquire(K_SIDE_BUY, price_raw, shares, body)) {
            uint64_t ma, ta;
            if (!compute_amounts(K_SIDE_BUY, price_raw, shares, ma, ta)) return -4;
            OrderV2 o{};
            o.salt = rng_.next_salt();
            o.timestamp_ms = PresignedOrderPool::now_ms();
            std::memcpy(o.maker, cfg_.maker, 20);
            std::memcpy(o.signer, cfg_.signer, 20);
            std::memcpy(o.token_id, cfg_.token_id_be, 32);
            o.maker_amount = ma;
            o.taker_amount = ta;
            o.side = K_SIDE_BUY;
            o.signature_type = cfg_.signature_type;
            uint8_t sig65[65];
            if (!signer_.sign_order(o, sig65)) return -5;
            if (!build_wire_body(o, sig65, cfg_.token_id_dec, cfg_.maker_hex,
                                 cfg_.signer_hex, cfg_.owner_api_key,
                                 cfg_.order_type, body)) return -6;
        }
        const auto res = client_.submit(body);
        return res.ok ? 1 : -7;
    }

    // Individual stage: inline ECDSA sign of a representative order.
    bool bench_inline_sign(uint8_t sig65[65]) {
        OrderV2 o{};
        o.salt = rng_.next_salt();
        o.timestamp_ms = PresignedOrderPool::now_ms();
        std::memcpy(o.maker, cfg_.maker, 20);
        std::memcpy(o.signer, cfg_.signer, 20);
        std::memcpy(o.token_id, cfg_.token_id_be, 32);
        o.maker_amount = 5500000;
        o.taker_amount = 10000000;
        o.side = K_SIDE_BUY;
        o.signature_type = cfg_.signature_type;
        return signer_.sign_order(o, sig65);
    }

    // Individual stage: pre-signed pool lookup for a given (price,size,side).
    bool bench_pool_acquire(uint8_t side, uint64_t price, uint64_t size, WireBody& out) {
        return pool_.acquire(side, price, size, out);
    }

    MockCLOBClient& client() { return client_; }

private:
    const MarketConfig& cfg_;
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& signals_;
    const EIP712Signer& signer_;
    PresignedOrderPool& pool_;
    MockCLOBClient client_{cfg_};
    FastRandom rng_;
};

#endif // BENCH_ENGINE_HPP
