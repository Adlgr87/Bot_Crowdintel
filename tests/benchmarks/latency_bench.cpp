// ─────────────────────────────────────────────────────────────────────────────
// latency_bench: attribution-precise hot-path latency measurement.
//
// Methodology (fixed vs the previous invalid benchmark, which pre-filled a
// 1024-slot queue with 20k signals, measured mostly empty pops, and assumed
// a fixed 3.0 GHz clock):
//   - TSC serialized with rdtscp + lfence, calibrated against CLOCK_MONOTONIC
//     over a 200 ms window each run (no assumed CPU frequency).
//   - Every measured tick is PRODUCTIVE: one signal is pushed immediately
//     before each timed tick (single-thread discipline).
//   - rdtscp call overhead measured and reported.
//   - Warmup excluded; percentiles p10/50/90/99/99.9 + min/max/mean reported.
//   - Stages measured separately:
//       1. full tick with pre-signed pool HIT   (the fast path)
//       2. full tick with pool MISS → inline ECDSA (the fallback path)
//       3. inline sign only (keccak ×3 + ECDSA)
//       4. pool scan only
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/bench_engine.hpp"
#include "../../core/src/presigned_pool.hpp"

static inline uint64_t rdtscp_serialized() {
    uint32_t aux;
    uint64_t rax, rdx;
    asm volatile("lfence\n\trdtscp" : "=a"(rax), "=d"(rdx), "=c"(aux) :: "memory");
    return (rdx << 32) | rax;
}

static double calibrate_ns_per_cycle() {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = rdtscp_serialized();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t c1 = rdtscp_serialized();
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return ns / (double)(c1 - c0);
}

static double measure_rdtscp_overhead_cycles() {
    constexpr int N = 10000;
    uint64_t total = 0;
    for (int i = 0; i < N; ++i) {
        const uint64_t a = rdtscp_serialized();
        const uint64_t b = rdtscp_serialized();
        total += b - a;
    }
    return (double)total / N;
}

static AlphaSignal make_signal(double p_win) {
    AlphaSignal s{};
    s.type = AlphaSignal::Type::WHALE_TRADE;
    s.direction_hint = 0;  // buy
    s.p_win = p_win;
    s.confidence = 0.92;
    s.q_value = 0.01;
    s.timestamp_ns = 1;
    std::snprintf(s.market_slug, sizeof(s.market_slug), "bench");
    return s;
}

int main() {
    std::printf("CROWDINTEL latency benchmark\n");
    const double ns_per_cycle = calibrate_ns_per_cycle();
    const double overhead = measure_rdtscp_overhead_cycles();
    std::printf("calibrated: %.4f ns/cycle (TSC %.2f MHz), rdtscp+lfence overhead %.1f cyc\n",
                ns_per_cycle, 1000.0 / ns_per_cycle, overhead);

    // ── Fixed offline config (no env required) ───────────────────────────────
    const char* env_key =
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";  // KAT key, public
    setenv("BOT_PRIVATE_KEY_HEX", env_key, 1);
    setenv("BOT_MODE", "mock", 1);

    MarketConfig cfg;
    if (const char* err = cfg.load(/*need_trading_creds=*/false)) {
        std::printf("FATAL: %s\n", err);
        return 1;
    }

    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal, 4096> signals;
    EIP712Signer signer;
    signer.init((const uint8_t*)"\x01\x02\x03\x04\x05\x06\x07\x08"
                                 "\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10"
                                 "\x11\x12\x13\x14\x15\x16\x17\x18"
                                 "\x19\x1a\x1b\x1c\x1d\x1e\x1f\x20", cfg.neg_risk);
    PresignedOrderPool pool(cfg, signer, 60000);

    // Two-sided synthetic book: ask 0.53 (530000), bid 0.47 (470000).
    // Deep levels (≥1000 shares) so the engine's liquidity clamp never
    // truncates the Kelly size (that would change the size → pool miss).
    Level2Entry bids[2] = {{470000, 1200000000}, {460000, 1600000000}};
    Level2Entry asks[2] = {{530000, 1200000000}, {540000, 1600000000}};
    book.set_bids(bids, 2);
    book.set_asks(asks, 2);

    // Pre-sign the taker grid so the fast path gets HITS at the exact size the
    // engine computes for p_win=0.60 against the 0.53 ask (exact Kelly).
    const double hit_p_win = 0.60;
    const double miss_p_win = 0.58;   // different Kelly → different size → MISS
    const double ask = 0.53;
    const double k_hit = KellyEngine::kelly_buy(hit_p_win, ask);
    const double usd_hit = KellyEngine::position_usd(k_hit, cfg.kelly_fraction, cfg.bankroll_usd);
    const uint64_t hit_shares = KellyEngine::usd_to_shares_fixed(usd_hit, ask);
    pool.rebuild(500000, hit_shares);
    std::printf("presign: %zu slots, hit-size %.2f shares (taker grid around mid 0.50)\n",
                pool.built_count(), (double)hit_shares * 1e-6);

    BenchEngine engine(cfg, book, signals, signer, pool);

    const size_t WARMUP = 2000;
    const size_t SAMPLES = 20000;
    std::vector<uint64_t> t_fast, t_inline, t_sign, t_scan;
    t_fast.reserve(SAMPLES);
    t_inline.reserve(SAMPLES);
    t_sign.reserve(SAMPLES);
    t_scan.reserve(SAMPLES);

    uint8_t sig65[65];
    WireBody wb;

    // ── Warmup ───────────────────────────────────────────────────────────────
    for (size_t i = 0; i < WARMUP; ++i) {
        signals.try_push(make_signal(hit_p_win));
        engine.run_tick();
        engine.bench_inline_sign(sig65);
        engine.bench_pool_acquire(K_SIDE_BUY, 530000, hit_shares, wb);
    }

    // ── 1. Full tick, pre-signed pool hit (p_win 0.60 → presigned size) ──────
    size_t productive = 0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        signals.try_push(make_signal(hit_p_win));   // un-timed producer step
        const uint64_t c0 = rdtscp_serialized();
        const int r = engine.run_tick();
        const uint64_t c1 = rdtscp_serialized();
        if (r > 0) { ++productive; t_fast.push_back(c1 - c0); }
    }
    std::printf("pool-hit path: %zu/%zu productive\n", productive, SAMPLES);

    // ── 2. Full tick, pool miss → inline ECDSA (p_win 0.58 → unpresigned size)
    productive = 0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        signals.try_push(make_signal(miss_p_win));
        const uint64_t c0 = rdtscp_serialized();
        const int r = engine.run_tick();
        const uint64_t c1 = rdtscp_serialized();
        if (r > 0) { ++productive; t_inline.push_back(c1 - c0); }
    }
    std::printf("inline-sign path: %zu/%zu productive\n", productive, SAMPLES);

    // ── 3. Sign only (keccak + ECDSA, the crypto core) ───────────────────────
    for (size_t i = 0; i < SAMPLES; ++i) {
        const uint64_t c0 = rdtscp_serialized();
        engine.bench_inline_sign(sig65);
        const uint64_t c1 = rdtscp_serialized();
        t_sign.push_back(c1 - c0);
    }

    // ── 4. Pool scan only ────────────────────────────────────────────────────
    for (size_t i = 0; i < SAMPLES; ++i) {
        const uint64_t c0 = rdtscp_serialized();
        engine.bench_pool_acquire(K_SIDE_BUY, 530000, hit_shares, wb);
        const uint64_t c1 = rdtscp_serialized();
        t_scan.push_back(c1 - c0);
    }

    // ── Report ───────────────────────────────────────────────────────────────
    auto report = [&](const char* name, std::vector<uint64_t>& v) {
        if (v.empty()) { std::printf("%-28s NO SAMPLES\n", name); return; }
        std::sort(v.begin(), v.end());
        auto at = [&](double p) {
            const size_t idx = (size_t)(v.size() * p);
            return v[std::min(idx, v.size() - 1)];
        };
        uint64_t sum = 0;
        for (auto c : v) sum += c;
        const double mean = (double)sum / v.size();
        std::printf("%-28s min %8.0f ns | p50 %8.0f | p90 %8.0f | p99 %8.0f | p999 %8.0f | max %8.0f | mean %8.0f\n",
            name,
            v.front() * ns_per_cycle, at(0.50) * ns_per_cycle, at(0.90) * ns_per_cycle,
            at(0.99) * ns_per_cycle, at(0.999) * ns_per_cycle, v.back() * ns_per_cycle,
            mean * ns_per_cycle);
    };
    std::printf("\n%-28s (values in ns)\n", "stage");
    report("tick[pool-hit, full]", t_fast);
    report("tick[inline-sign, full]", t_inline);
    report("sign only (keccak+ecdsa)", t_sign);
    report("pool scan only", t_scan);

    std::printf("\nhot path core measured: no network I/O, calibrated TSC, "
                "%zu samples each, %zu warmup\n", SAMPLES, WARMUP);
    return 0;
}
