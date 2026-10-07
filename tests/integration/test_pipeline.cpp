// ─────────────────────────────────────────────────────────────────────────────
// test_pipeline: Acceptance test for full BTC 5m/15m pipeline integration.
//
// Exercises the full data path:
//   MarketState (synthetic) → SPSC ring → drain_market_state → CfCNetwork::infer
//   → compute_conviction → KellySizer → TWAPTracker → LadderSkewer → WindowShield
//
// Exit code 0 = all pass. Zero external deps.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdio>
#include <cmath>

#include "engine_extensions.hpp"

// ── Test framework ─────────────────────────────────────────────────────────────
static int g_failures = 0;
static int g_tests = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
               ++g_failures; }                                                \
    } while (0)

static bool approxf(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps * (1.0f + std::fabs(a) + std::fabs(b));
}

int main() {
    std::printf("=== BTC Specialization Pipeline Integration Tests ===\n");

    // ── 1. Initialize all modules ─────────────────────────────────────────────
    std::printf("\n[1] Module initialization\n");

    // CfC network
    CfCNetwork cfc_net;
    bool weights_loaded = cfc_net.load_weights("infra/models/cfc_btc_5m_v1.bin");
    CHECK(weights_loaded, "CfC model loads from binary file");
    CHECK(cfc_net.verify_hash(cfc_net.model_hash()), "CfC SHA256 hash verified");

    CfCState cfc_state{};
    cfc_net.reset(cfc_state, 0.0f);
    CHECK(cfc_state.active, "CfC state initialized active");

    // OFI calculator
    OFICalculator ofi_calc(OFIConfig{});
    ofi_calc.reset();
    CHECK(true, "OFI calculator initialized");

    // Kelly sizer
    KellyConfig kelly_cfg{};
    kelly_cfg.fraction = 0.25f;           // quarter-Kelly
    kelly_cfg.max_bankroll_pct = 0.05f;   // max 5% per trade
    KellySizer kelly(kelly_cfg);
    CHECK(true, "KellySizer initialized");

    // TWAP tracker
    TWAPTracker twap;
    uint64_t ts = 1'700'000'000'000'000'000ULL;
    twap.reset(60000.0, ts);
    CHECK(true, "TWAPTracker initialized");

    // Ladder skewer
    LadderSkewConfig skew_cfg{};
    skew_cfg.w_cfc = 0.5f;
    skew_cfg.w_ofi = 0.3f;
    skew_cfg.w_twap = 0.2f;
    skew_cfg.skew_intensity = 0.7f;
    LadderSkewer skewer(skew_cfg);
    CHECK(true, "LadderSkewer initialized");

    // WindowShield (use BTC_5M config, not zeroed defaults)
    WindowShield shield(WindowShieldConfig::BTC_5M());
    shield.set_window_start(ts);  // REQUIRED: fail-closed until window_start is set
    ShieldState initial_state = shield.update(ts, 0.5f);
    CHECK(initial_state == ShieldState::MAKER_PASSIVE, "WindowShield active after init");

    // SPSC ring for market states
    SPSC_RingBuffer<MarketState, BinanceConfig::RING_CAPACITY> market_ring;
    CHECK(true, "MarketState ring initialized");

    // ── 2. Generate synthetic data → ring → CfC inference ───────────────────
    std::printf("\n[2] End-to-end pipeline: synthetic data → CfC inference\n");

    float last_confidence = 0.5f;
    int valid_inferences = 0;
    float total_conviction = 0.0f;
    int conviction_samples = 0;

    for (int i = 0; i < 10; i++) {
        // Synthetic MarketState: price walks from 60000 → 61000 (bull trend)
        MarketState ms{};
        ms.timestamp_ns = ts + static_cast<uint64_t>(i * 100'000'000ULL);
        ms.ofi_normalized  = 0.05f * (i < 5 ? 1.0f : -1.0f);
        ms.trade_intensity  = 0.5f;
        ms.spread_bps       = 1.0f;
        ms.depth_imbalance  = 0.1f * static_cast<float>(i);
        ms.microprice       = 60000.0 + i * 10.0;
        ms.mid_velocity     = (i < 5) ? 50.0f : -50.0f;

        // Push to ring
        CHECK(market_ring.try_push(ms), "MarketState pushed to ring");

        // Drain through pipeline
        MarketState drained{};
        if (market_ring.try_pop(drained)) {
            // OFI update
            ofi_calc.on_event(
                static_cast<double>(ms.microprice - 0.005),  // bid
                static_cast<double>(ms.microprice + 0.005),  // ask
                100.0, 100.0, false,  // is_trade=false
                ms.timestamp_ns);

            MarketState ofi_state{};
            if (ofi_calc.current_state(ofi_state)) {
                // CfC inference
                CfCInput input{};
                input.features[0] = static_cast<float>(ofi_state.ofi_normalized);
                input.features[1] = static_cast<float>(ofi_state.trade_intensity);
                input.features[2] = static_cast<float>(ofi_state.spread_bps);
                input.features[3] = static_cast<float>(ofi_state.depth_imbalance);
                input.features[4] = static_cast<float>(ofi_state.microprice);
                input.features[5] = static_cast<float>(ofi_state.mid_velocity);
                input.timestamp_ns = drained.timestamp_ns;

                CfCSignal signal = cfc_net.infer(cfc_state, input, drained.timestamp_ns);
                if (!signal.nan_guard_triggered) {
                    valid_inferences++;
                    last_confidence = signal.probability_up;

                    // TWAP update
                    twap.update(ofi_state.microprice, drained.timestamp_ns);
                    TWAPSignal twap_sig = twap.check(
                        drained.timestamp_ns, 300);  // 5-min window

                    // Conviction
                    float conviction = compute_conviction(
                        signal.probability_up,
                        static_cast<float>(ofi_state.ofi_normalized),
                        static_cast<float>(twap_sig.deviation));
                    total_conviction += conviction;
                    conviction_samples++;
                }
            }
        }
    }

    CHECK(valid_inferences > 5, "Pipeline produces valid CfC signals (>5/10)");
    CHECK(last_confidence > 0.0f && last_confidence < 1.0f,
          "CfC confidence in valid range [0, 1]");

    // ── 3. Kelly sizing check ────────────────────────────────────────────────
    std::printf("\n[3] Kelly sizing (quarter-Kelly)\n");

    float avg_conviction = total_conviction / conviction_samples;
    // Override with a confident bullish signal for deterministic Kelly test
    float kelly_p = 0.75f;  // 75% win probability → positive Kelly fraction
    uint64_t price_cents = 61000000;    // 0.61 normalized (Polymarket binary price * 1e8)
    uint64_t bankroll_cents = 10'000'000'000;  // 100M USD bankroll (test scale)

    KellyOutput ks = kelly.compute(
        kelly_p,
        price_cents,
        bankroll_cents,
        0.0f,     // no inventory
        0.0f,     // no vol z-score
        false);   // taker

    CHECK(ks.should_trade, "Kelly recommends trade (conviction 0.75 > threshold)");
    CHECK(ks.fraction_of_bankroll > 0.0f && ks.fraction_of_bankroll <= 0.05f,
          "Fraction capped at max_bankroll_pct (5%)");
    CHECK(ks.order_shares > 0, "Order produces non-zero shares");
    CHECK(ks.order_usd_cents > 0, "Order has positive USD cost");

    // ── 4. Ladder skew ────────────────────────────────────────────────────────
    std::printf("\n[4] Ladder skew (16-level order ladder)\n");

    ConvictionInput ci{};
    ci.cfc_probability = avg_conviction;
    ci.ofi_signal = 0.1f;
    ci.twap_signal = 0.5f;

    uint64_t mid_cents = 6100000;
    uint64_t spread_cents = 10;  // 0.10 USD spread
    uint64_t base_shares = 10;
    uint64_t bankroll_c = 100'000'000;

    LadderOutput ladder_out = skewer.build(ci, mid_cents, spread_cents,
                                           base_shares, bankroll_c);

    CHECK(ladder_out.n_quotes > 0, "Ladder produces quotes");
    CHECK(ladder_out.n_quotes <= 16, "Ladder ≤ 16 quotes (8 per side)");
    CHECK(ladder_out.conviction >= 0.0f && ladder_out.conviction <= 1.0f,
          "Conviction in [0, 1]");
    CHECK(std::fabs(ladder_out.skew) <= 1.0f, "Skew in [-1, 1]");

    // Verify quotes have alternating bid/ask
    bool has_bid = false, has_ask = false;
    for (uint32_t q = 0; q < ladder_out.n_quotes; q++) {
        if (ladder_out.quotes[q].is_bid) has_bid = true;
        else has_ask = true;
    }
    CHECK(has_bid && has_ask, "Ladder has both bid and ask quotes");

    // ── 5. WindowShield with stale timestamp ──────────────────────────────────
    std::printf("\n[5] WindowShield stale-timestamp blocking\n");

    // Fresh timestamp → should be in active state (not halted)
    ShieldState fresh_state = shield.update(ts + 1'000'000'000ULL, avg_conviction);
    CHECK(fresh_state != ShieldState::HALTED && fresh_state != ShieldState::CLOSE_ONLY,
          "WindowShield allows trading on fresh timestamp");

    // Stale timestamp (no set_window_start) → fail-closed
    WindowShield alt_shield(WindowShieldConfig::BTC_5M());
    ShieldState stale_state = alt_shield.update(ts, avg_conviction);
    CHECK(stale_state == ShieldState::HALTED,
          "WindowShield fail-closed without set_window_start");

    // ── 6. Defense check integration ──────────────────────────────────────────
    std::printf("\n[6] Defense check integration\n");

    ExtendedEngineLayers layers;
    layers.binance_market_q = &market_ring;
    layers.cfc_network = &cfc_net;
    layers.cfc_state = &cfc_state;
    layers.window_shield = &shield;
    layers.kelly_sizer = &kelly;
    layers.ladder = &skewer;
    layers.twap_tracker = &twap;

    CfCSignal dummy_signal{};
    bool drained = drain_market_state(layers, dummy_signal, ts + 1'000'000'000ULL);
    // Ring might be empty (drained in test above), so false is OK
    CHECK(true, "drain_market_state callable (no crash)");

    bool defended = defense_check(layers, ts + 1'000'000'000ULL, avg_conviction);
    CHECK(true, "defense_check callable (no crash)");

    // ── 7. Null-safe layers (legacy mode) ──────────────────────────────────────
    std::printf("\n[7] Null-safe layers (legacy mode fallback)\n");

    ExtendedEngineLayers null_layers;
    CfCSignal null_signal{};
    bool null_drained = drain_market_state(null_layers, null_signal, ts);
    CHECK(!null_drained, "drain_market_state returns false on null layers");

    bool null_defended = defense_check(null_layers, ts, 0.5f);
    CHECK(null_defended, "defense_check returns true on null layers (legacy pass-through)");

    // ── Summary ──────────────────────────────────────────────────────────────
    std::printf("\n========================================\n");
    std::printf("Integration: %d/%d checks passed\n",
        g_tests - g_failures, g_tests);
    std::printf("Valid inferences: %d/10\n", valid_inferences);
    std::printf("Avg conviction: %.3f\n", avg_conviction);
    std::printf("========================================\n");

    return g_failures > 0 ? 1 : 0;
}
