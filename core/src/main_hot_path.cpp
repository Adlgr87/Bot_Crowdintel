// ─────────────────────────────────────────────────────────────────────────────
// Bot CrowdIntel — hot path entrypoint.
//
// Live topology (all threads besides the engine are cold path):
//
//   [WSS market feed thread] ──seqlock──► OrderBookL2 ───┐
//   [CrowdIntel webhook → AlphaParser] ──SPSC──┐          │
//   [presign thread] ──pool flip──┐            │          │
//                                 ▼            ▼          ▼
//                          [ExecutionEngine — pinned core, spin/park loop]
//                                                 │
//                                                 ▼
//                        [CLOB V2 client — persistent TLS + HMAC]
//
// Modes:
//   BOT_MODE unset      → live: real WSS feed, real CLOB submission
//   BOT_MODE=mock       → offline demo: no network, synthetic book + signals
//   BOT_TICKS=N         → stop after N productive ticks (0 = run until ^C)
//
// Required env (live): BOT_PRIVATE_KEY_HEX, CLOB_API_KEY, CLOB_SECRET,
//                      CLOB_PASSPHRASE, BOT_TOKEN_ID (+ optional knobs — see
//                      market_config.hpp). No credentials are ever hardcoded.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <thread>

#include "../include/order_book.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../crypto/eip712_signer.hpp"
#include "market_config.hpp"
#include "mock_client.hpp"
#include "presigned_pool.hpp"
#include "execution_engine.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK) && !defined(CROWDINTEL_FORCE_MOCK)
#include "lightweight_client.hpp"
#include "ws_market_listener.hpp"
#define CROWDINTEL_LIVE 1
#endif

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false); }

namespace {

// Park/spin hybrid: a hot core that must react within microseconds spins on
// `pause`; after ~2 ms of idleness it drops into a 100 µs nanosleep loop
// (saves CPU on shared boxes; re-arms instantly when work arrives).
void engine_idle_wait() {
    thread_local uint32_t spins = 0;
    if (++spins < 20000) {
        __builtin_ia32_pause();
    } else {
        timespec ts{0, 100000};  // 100 µs
        nanosleep(&ts, nullptr);
    }
}

void pin_to_cpu(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        std::fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
}

// Synthetic book + signals for BOT_MODE=mock (no network, deterministic demo).
void mock_feed(SPSC_RingBuffer<AlphaSignal>& q, OrderBookL2& book,
               std::atomic<bool>& running) {
    // Seed a two-sided book: bids ~0.47, asks ~0.53 (1e6 fixed point).
    Level2Entry bids[3] = {{470000, 40000000}, {460000, 60000000}, {450000, 90000000}};
    Level2Entry asks[3] = {{530000, 30000000}, {540000, 55000000}, {550000, 80000000}};
    book.set_bids(bids, 3);
    book.set_asks(asks, 3);
    uint64_t i = 0;
    while (running.load()) {
        AlphaSignal s{};
        s.type = AlphaSignal::Type::WHALE_TRADE;
        s.direction_hint = 2;
        // Alternate an edge on the buy side (p_win 0.60 vs ask 0.53) and a
        // no-edge case (0.50) so the demo shows both trades and rejections.
        s.p_win = (i % 2) ? 0.50 : 0.60;
        s.confidence = 0.92;
        s.q_value = 0.01;
        s.timestamp_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::snprintf(s.market_slug, sizeof(s.market_slug), "mock-%llu",
                      (unsigned long long)(i % 1000));
        q.try_push(s);
        ++i;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::fprintf(stdout,
        "CROWDINTEL v2.0 — Polymarket CLOB V2 ultra-low-latency trader\n");

    // ── 1. Configuration (cold) ──────────────────────────────────────────────
    MarketConfig cfg;
    const bool mock =
#if !defined(CROWDINTEL_LIVE)
        true;  // offline build: mock is the only mode
#else
        std::getenv("BOT_MODE") && std::string(std::getenv("BOT_MODE")) == "mock";
#endif
    if (const char* err = cfg.load(!mock)) {
        std::fprintf(stderr, "FATAL: %s\n", err);
        return 1;
    }
    std::fprintf(stdout,
        "market=%s tick=%.4f neg_risk=%d sig_type=%u order_type=%s mode=%s\n",
        cfg.token_id_dec, (double)cfg.tick_size * 1e-6, (int)cfg.neg_risk,
        cfg.signature_type, cfg.order_type, mock ? "mock" : "live");

    // ── 2. Core objects ──────────────────────────────────────────────────────
    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal> signals;

    // Private key: required in both modes (the signer derives the address).
    const char* key_hex = std::getenv("BOT_PRIVATE_KEY_HEX");
    uint8_t key[32];
    if (!key_hex || !parse_hex_bytes(key_hex, std::strlen(key_hex), key, 32)) {
        std::fprintf(stderr,
            "FATAL: BOT_PRIVATE_KEY_HEX must be set to a 64-char hex private key\n");
        return 1;
    }
    EIP712Signer signer;
    signer.init(key, cfg.neg_risk);
    secure_zero(key, 32);
    std::fprintf(stdout, "signer=");
    for (int i = 0; i < 20; ++i) std::fprintf(stdout, "%02x", signer.signer_address()[i]);
    std::fprintf(stdout, "\n");

    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);

    MockCLOBClient mock_client(cfg);
#if defined(CROWDINTEL_LIVE)
    // The live client is only constructed in live mode (its ctor decodes the
    // CLOB secret — meaningless and failing in mock mode).
    LightweightCLOBClient* live_client = mock ? nullptr : new LightweightCLOBClient(cfg);
    std::fprintf(stdout, "client=%s\n", mock ? "MockCLOBClient (no network I/O)"
                                             : "LightweightCLOBClient (live)");
#else
    std::fprintf(stdout, "client=MockCLOBClient (offline build)\n");
#endif

    // ── 3. Cold-path threads ─────────────────────────────────────────────────
    std::atomic<bool> running{true};

    std::thread feed_thread;
    std::thread wss_thread;
#if defined(CROWDINTEL_LIVE)
    WsMarketListener* listener = nullptr;
#endif
    if (mock) {
        feed_thread = std::thread([&] { mock_feed(signals, book, running); });
    }
#if defined(CROWDINTEL_LIVE)
    else {
        listener = new WsMarketListener(cfg, book);
        wss_thread = std::thread([&] { listener->start(); });
        // Give the feed a moment to produce the first book snapshot.
        for (int i = 0; i < 50 && book.sequence() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (book.sequence() == 0)
            std::fprintf(stdout, "warning: no book snapshot yet (continuing)\n");
    }
#endif

    std::thread presign_thread([&] {
        uint64_t last_mid = 500000;
        while (running.load()) {
            // Grid center = current mid (fallback: last known mid).
            OrderBookL2::Top top;
            if (book.read_top(top) && top.bid.size && top.ask.size)
                last_mid = (top.bid.price + top.ask.price) / 2;
            // Pre-sign at the Kelly-neutral default bucket: 20% of the capped
            // fractional-Kelly position at the grid center.
            const double price = (double)last_mid * 1e-6;
            const double usd = cfg.bankroll_usd * cfg.kelly_fraction * 0.2;
            const uint64_t shares = KellyEngine::usd_to_shares_fixed(usd, price);
            if (shares >= cfg.min_size_shares)
                pool.rebuild(last_mid, shares);
            for (int i = 0; i < 10 && running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    (long)(cfg.presign_ttl_ms / 10)));
        }
    });

    // ── 4. Hot loop (pinned, spin/park) ──────────────────────────────────────
    pin_to_cpu(cfg.pin_cpu);

    uint64_t counts[10] = {0};
    long ticks_done = 0;
    const long max_ticks = cfg.max_ticks;
    const auto t0 = std::chrono::steady_clock::now();

    if (mock) {
        ExecutionEngine<MockCLOBClient> engine(cfg, book, signals, signer, pool, mock_client);
        std::fprintf(stdout, "hot loop running (pin_cpu=%d, max_ticks=%ld)\n",
                     cfg.pin_cpu, max_ticks);
        while (g_running.load()) {
            const TickResult r = engine.run_tick();
            ++counts[(int)r];
            if (r != TickResult::NO_SIGNAL) ++ticks_done;
            else engine_idle_wait();
            if (max_ticks > 0 && ticks_done >= max_ticks) break;
        }
    }
#if defined(CROWDINTEL_LIVE)
    else {
        ExecutionEngine<LightweightCLOBClient> engine(cfg, book, signals, signer,
                                                      pool, *live_client);
        std::fprintf(stdout, "hot loop running (pin_cpu=%d, max_ticks=%ld)\n",
                     cfg.pin_cpu, max_ticks);
        while (g_running.load()) {
            const TickResult r = engine.run_tick();
            ++counts[(int)r];
            if (r != TickResult::NO_SIGNAL) ++ticks_done;
            else engine_idle_wait();
            if (max_ticks > 0 && ticks_done >= max_ticks) break;
        }
    }
#endif
    const auto t1 = std::chrono::steady_clock::now();

    // ── 5. Honest shutdown summary ───────────────────────────────────────────
    running.store(false);
    if (wss_thread.joinable()) wss_thread.join();
    if (feed_thread.joinable()) feed_thread.join();
    presign_thread.join();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    std::fprintf(stdout, "\n— session summary (%.1fs) —\n", secs);
    std::fprintf(stdout, "submitted: %llu  submit_failed: %llu\n",
        (unsigned long long)counts[(int)TickResult::SUBMITTED],
        (unsigned long long)counts[(int)TickResult::SUBMIT_FAILED]);
    std::fprintf(stdout,
        "rejected : no_edge=%llu filtered=%llu no_book=%llu too_small=%llu\n",
        (unsigned long long)counts[(int)TickResult::NO_EDGE],
        (unsigned long long)counts[(int)TickResult::FILTERED_STATS],
        (unsigned long long)counts[(int)TickResult::NO_BOOK],
        (unsigned long long)counts[(int)TickResult::TOO_SMALL]);
#if defined(CROWDINTEL_LIVE)
    if (listener) {
        std::fprintf(stdout, "ws: events=%llu reconnects=%llu\n",
            (unsigned long long)listener->events_seen(),
            (unsigned long long)listener->reconnects());
        delete listener;
    }
    delete live_client;
#endif
    std::fprintf(stdout, "shutdown clean.\n");
    return 0;
}
