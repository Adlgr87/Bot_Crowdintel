// CPU-path microbenchmark.  It deliberately excludes network/HMAC/TLS and
// labels results accordingly.  Presigned slots are consumable, so the fixture
// rebuilds a fresh eight-size BUY ladder outside each measured batch.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/src/bench_engine.hpp"
#include "alpha_parser.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/presigned_pool.hpp"

static uint64_t clock_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static AlphaSignal make_signal(const MarketConfig& cfg, double p_win,
                               uint64_t id) {
    AlphaSignal signal{};
    signal.type = AlphaSignal::Type::WHALE_TRADE;
    signal.direction_hint = 0;
    signal.p_win = p_win;
    signal.confidence = 0.92;
    signal.q_value = 0.01;
    signal.timestamp_ns = AlphaParser::realtime_ns();
    signal.market_hash = cfg.market_hash;
    signal.signal_id = id;
    return signal;
}

// Prints why a loop rejected signals.  Returns false when the loop produced no
// sample at all, which the caller must treat as a benchmark failure: a p50 of an
// empty set is not a fast p50, it is no measurement.
bool report_rejections(const char* loop_name, size_t successful, size_t samples,
                       const size_t* rejected) {
    if (successful == samples) return true;
    for (size_t i = 0; i < static_cast<size_t>(TickResult::COUNT); ++i) {
        if (!rejected[i]) continue;
        std::printf("  %s rejected %zu x %s\n", loop_name, rejected[i],
                    tick_result_name(static_cast<TickResult>(i)));
    }
    if (successful == 0) {
        std::printf("FATAL: %s produced no samples; the latency figures below "
                    "would be meaningless\n", loop_name);
        return false;
    }
    return true;
}

int main() {
    std::printf("CrowdIntel CPU-path latency benchmark (network excluded)\n");
    setenv("BOT_PRIVATE_KEY_HEX",
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b", 1);
    setenv("BOT_MODE", "paper", 1);
    setenv("BOT_MARKET_SLUG", "bench", 1);
    // This instrument measures the CPU path only: it never contacts a venue, so it
    // pins the outcome identity explicitly instead of relying on a mode default.
    // (Paper no longer inherits the replay test vector: a networked paper run must
    // resolve the token from venue metadata, exactly like live.)
    setenv("BOT_TOKEN_ID",
           "71321045679252212594626395510336467040167069592778062791519851593659551227755",
           1);

    MarketConfig cfg;
    if (const char* error = cfg.load(false, true)) {
        std::printf("FATAL: %s\n", error); return 1;
    }
    cfg.bankroll_usd = 10000.0;
    cfg.kelly_fraction = 0.25;
    cfg.max_order_usd = 100.0;
    cfg.max_exposure_usd = 100000000.0;
    cfg.max_daily_loss_usd = 100000000.0;
    cfg.taker_fee_rate = 0.0;  // isolate mechanics, not fee-policy rejections
    cfg.order_type[0] = 'F'; cfg.order_type[1] = 'A';
    cfg.order_type[2] = 'K'; cfg.order_type[3] = '\0';

    uint8_t key[32];
    parse_hex_bytes(cfg.private_key_hex, 64, key, sizeof(key));
    EIP712Signer signer;
    if (!signer.init(key, cfg.neg_risk)) {
        secure_zero(key, sizeof(key));
        std::printf("FATAL: signer initialization failed\n");
        return 1;
    }
    secure_zero(key, sizeof(key));
    secure_zero(cfg.private_key_hex, sizeof(cfg.private_key_hex));
    if (const char* error = cfg.finalize_identity(signer.signer_address())) {
        std::printf("FATAL: %s\n", error); return 1;
    }

    OrderBookL2 book;
    book.set_tick_size(cfg.tick_size);
    Level2Entry bids[2] = {{470000, 2000000000}, {460000, 2000000000}};
    Level2Entry asks[2] = {{530000, 2000000000}, {540000, 2000000000}};
    book.set_book(bids, 2, asks, 2);

    auto signals = std::make_unique<SPSC_RingBuffer<AlphaSignal>>();
    PresignedOrderPool pool(cfg, signer, 60000);
    const uint64_t target_shares = KellyEngine::usd_to_shares_fixed(
        cfg.max_order_usd, 0.53);
    pool.rebuild(470000, 530000, target_shares, cfg.tick_size);
    BenchEngine engine(cfg, book, *signals, signer, pool);

    constexpr size_t WARMUP = 500;
    constexpr size_t SAMPLES = 10000;
    uint64_t signal_id = 1;
    for (size_t i = 0; i < WARMUP; ++i) {
        if (i % PresignedOrderPool::SIZE_BUCKETS == 0)
            pool.rebuild(470000, 530000, target_shares, cfg.tick_size);
        signals->try_push(make_signal(cfg, 0.75, signal_id++));
        (void)engine.run_tick();
    }

    std::vector<uint64_t> pool_hit, inline_sign, sign_only, pool_lookup;
    pool_hit.reserve(SAMPLES); inline_sign.reserve(SAMPLES);
    sign_only.reserve(SAMPLES); pool_lookup.reserve(SAMPLES);

    // A benchmark that measures nothing must not look like a benchmark that
    // measured something: CI compares p50 against a budget, and an empty sample
    // set used to report p50=0 and pass.  Rejections are counted per reason and a
    // wholly unproductive loop is a hard error.
    size_t successful = 0;
    size_t rejected[static_cast<size_t>(TickResult::COUNT)]{};
    for (size_t i = 0; i < SAMPLES; ++i) {
        if (i % PresignedOrderPool::SIZE_BUCKETS == 0)
            pool.rebuild(470000, 530000, target_shares, cfg.tick_size);
        signals->try_push(make_signal(cfg, 0.75, signal_id++));
        const uint64_t begin = clock_ns();
        const int result = engine.run_tick();
        const uint64_t end = clock_ns();
        if (result == 1) { pool_hit.push_back(end - begin); ++successful; }
        else {
            const TickResult reason = static_cast<TickResult>(-result);
            const size_t index = static_cast<size_t>(reason);
            if (index < static_cast<size_t>(TickResult::COUNT)) ++rejected[index];
        }
    }
    std::printf("consumable-pool batches: %zu/%zu productive\n", successful, SAMPLES);
    if (!report_rejections("consumable-pool", successful, SAMPLES, rejected)) return 1;

    // Move top-of-book to a price absent from the active ladder, forcing the
    // exact production fallback path (amount build + Keccak + ECDSA + JSON).
    bids[0] = {450000, 2000000000};
    asks[0] = {550000, 2000000000};
    book.set_book(bids, 2, asks, 2);
    successful = 0;
    for (size_t& count : rejected) count = 0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        signals->try_push(make_signal(cfg, 0.75, signal_id++));
        const uint64_t begin = clock_ns();
        const int result = engine.run_tick();
        const uint64_t end = clock_ns();
        if (result == 1) { inline_sign.push_back(end - begin); ++successful; }
        else {
            const TickResult reason = static_cast<TickResult>(-result);
            const size_t index = static_cast<size_t>(reason);
            if (index < static_cast<size_t>(TickResult::COUNT)) ++rejected[index];
        }
    }
    std::printf("inline fallback: %zu/%zu productive\n", successful, SAMPLES);
    if (!report_rejections("inline-fallback", successful, SAMPLES, rejected)) return 1;

    uint8_t signature[65];
    for (size_t i = 0; i < SAMPLES; ++i) {
        const uint64_t begin = clock_ns();
        (void)engine.bench_inline_sign(signature);
        const uint64_t end = clock_ns();
        sign_only.push_back(end - begin);
    }

    WireBody body{};
    for (size_t i = 0; i < SAMPLES; ++i) {
        if (i % PresignedOrderPool::SIZE_BUCKETS == 0)
            pool.rebuild(470000, 530000, target_shares, cfg.tick_size);
        const uint64_t begin = clock_ns();
        (void)engine.bench_pool_acquire(K_SIDE_BUY, 530000,
                                        target_shares, body);
        const uint64_t end = clock_ns();
        pool_lookup.push_back(end - begin);
    }

    auto report = [](const char* name, std::vector<uint64_t>& samples) {
        if (samples.empty()) {
            // front()/percentile() on an empty vector is undefined behaviour, and
            // "mean=nan" is not a result anyone should have to interpret.
            std::printf("%-33s no samples\n", name);
            return;
        }
        std::sort(samples.begin(), samples.end());
        auto percentile = [&](double p) {
            return samples[std::min(samples.size() - 1,
                static_cast<size_t>(p * static_cast<double>(samples.size())))];
        };
        long double total = 0;
        for (uint64_t value : samples) total += value;
        std::printf("%-33s min=%6llu ns p50=%6llu p90=%6llu p99=%6llu max=%8llu mean=%8.1Lf\n",
            name,
            static_cast<unsigned long long>(samples.front()),
            static_cast<unsigned long long>(percentile(0.50)),
            static_cast<unsigned long long>(percentile(0.90)),
            static_cast<unsigned long long>(percentile(0.99)),
            static_cast<unsigned long long>(samples.back()),
            total / samples.size());
    };

    report("decision+pool+mock-submit", pool_hit);
    report("decision+inline-sign+mock-submit", inline_sign);
    report("sign only (Keccak+ECDSA)", sign_only);
    report("consumable pool lookup+copy", pool_lookup);
    std::printf("No network, HMAC, DNS, TCP or TLS is included in these values.\n");
    return 0;
}
