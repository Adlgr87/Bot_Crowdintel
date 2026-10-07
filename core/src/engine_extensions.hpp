// ─────────────────────────────────────────────────────────────────────────────
// engine_extensions.hpp — PHASE-5: Hot-path integration of new modules
//
// Extends ExecutionEngine with BTC 5m/15m specialization:
//   - BinanceWSClient: cold-path market data → SPSC ring
//   - OFICalculator: incremental OFI → MarketState
//   - CfCNetwork: neural conviction < 3μs
//   - WindowShield: settlement-window lifecycle state machine
//   - SpikeDetector: anti-sniping on Binance feed
//   - TWAPTracker: convergence + manipulation detection
//   - KellySizer: quarter-Kelly position sizing
//   - LadderSkewer: dynamic bid/ask skew
//
// Integration is opt-in: all pointers nullable, legacy behavior preserved
// when absent.  Hot-path additions sum to < 12μs p50 (see LATENCY_BUDGET.md).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

// NOTE: cfc_network.hpp must be included before binance_ws_client.hpp because
// it uses SIMD intrinsics that interfere with <chrono> from binance_ws_client.
#include "cfc_network.hpp"              // CfCNetwork, CfCState, CfCSignal, CfCInput
#include "ofi_calculator.hpp"         // OFICalculator, MarketState
#include "binance_ws_client.hpp"        // BinanceWSClient
#include "window_shield.hpp"            // WindowShield, ShieldState
#include "binance_spike_detector.hpp"  // BinanceSpikeDetector
#include "kelly_sizer.hpp"             // KellySizer
#include "twap_tracker.hpp"            // TWAPTracker
#include "ladder_skew.hpp"            // LadderSkewer, ConvictionInput
#include "../include/journal.hpp"
#include "../include/position_tracker.hpp"
#include "../include/risk_manager.hpp"
#include "../include/volatility_gate.hpp"
#include "../include/time_utils.hpp"

// ── Extended EngineLayers ─────────────────────────────────────────────────────
// Standalone struct (not inheriting EngineLayers to avoid circular include).
// The ExecutionEngine constructor merges this into EngineLayers at startup.
// All new pointers are nullable (nullptr = disabled / legacy mode).
//
// Integration approach: the hot path checks these pointers first; if nullptr,
// it skips the new logic and falls through to legacy behavior.
struct ExtendedEngineLayers {
    // ---- Existing layers (copied from EngineLayers for convenience) ----
    SPSC_RingBuffer<JournalEvent>* journal_q = nullptr;
    PositionTracker* tracker = nullptr;
    RiskManager* risk = nullptr;
    VolatilityGate* volatility = nullptr;

    // ---- Phase 1: Binance data pipeline ----
    SPSC_RingBuffer<MarketState, BinanceConfig::RING_CAPACITY>*
        binance_market_q = nullptr;
    OFICalculator* ofi_calc = nullptr;

    // ---- Phase 2: Neural engine ----
    CfCNetwork* cfc_network = nullptr;
    CfCState* cfc_state = nullptr;          // per-market state

    // ---- Phase 3: Defense ----
    WindowShield* window_shield = nullptr;
    BinanceSpikeDetector* spike_detector = nullptr;

    // ---- Phase 4: Strategy ----
    TWAPTracker* twap_tracker = nullptr;
    KellySizer* kelly_sizer = nullptr;
    LadderSkewer* ladder = nullptr;

    // ---- Conviction weights (Phase 4) ----
    float w_cfc = 0.5f;
    float w_ofi = 0.3f;
    float w_twap = 0.2f;

    // ---- Runtime flags ----
    std::atomic<bool>* trading_enabled = nullptr;
};

// ── Hot-path helpers (all inline, < 500ns combined) ───────────────────────────

// Drain latest MarketState from Binance → CfC inference
// Returns true if a new state was available.
static inline bool drain_market_state(
    ExtendedEngineLayers& layers,
    CfCSignal& out_signal,
    uint64_t now_ns) noexcept
{
    if (!layers.binance_market_q || !layers.cfc_network) return false;

    MarketState state{};
    bool got_state = false;
    // Drain all pending states (process latest only for latency)
    while (layers.binance_market_q->try_pop(state)) {
        got_state = true;
    }
    if (!got_state) return false;

    // Convert MarketState → CfCInput
    CfCInput input{};
    input.features[0] = static_cast<float>(state.ofi_normalized);
    input.features[1] = static_cast<float>(state.trade_intensity);
    input.features[2] = static_cast<float>(state.spread_bps);
    input.features[3] = static_cast<float>(state.depth_imbalance);
    input.features[4] = static_cast<float>(state.microprice);
    input.features[5] = static_cast<float>(state.mid_velocity);
    input.timestamp_ns = now_ns;

    out_signal = layers.cfc_network->infer(*layers.cfc_state, input, now_ns);
    return true;
}

// Check WindowShield + SpikeDetector for trade authorization
// Returns false if trading is blocked
static inline bool defense_check(
    const ExtendedEngineLayers& layers,
    uint64_t now_ns,
    float cfc_confidence) noexcept
{
    // WindowShield: settlement phase gate
    if (layers.window_shield) {
        const ShieldState state =
            layers.window_shield->update(now_ns, cfc_confidence);
        if (state == ShieldState::HALTED ||
            state == ShieldState::CLOSE_ONLY) {
            // Log once per transition
            static thread_local ShieldState last_logged = ShieldState::MAKER_PASSIVE;
            if (last_logged != state) {
                std::fprintf(stderr,
                    "[shield] trading blocked: %s\n",
                    layers.window_shield->state_name(state));
                last_logged = state;
            }
            return false;
        }
    }

    // SpikeDetector: anti-sniping
    if (layers.spike_detector) {
        if (layers.spike_detector->has_spike()) {
            std::fprintf(stderr, "[spike] trading blocked: Binance price spike detected\n");
            layers.spike_detector->clear_spike();
            return false;
        }
    }

    return true;
}

// Compute conviction from CfC + OFI + TWAP
static inline float compute_conviction(
    float cfc_p,
    float ofi_signal,
    float twap_signal) noexcept
{
    // Normalize TWAP signal: positive deviation → bullish
    // twap_signal is deviation (-1, 1), map to [0, 1]
    float twap_norm = (twap_signal + 1.0f) / 2.0f;
    twap_norm = twap_norm < 0.0f ? 0.0f : (twap_norm > 1.0f ? 1.0f : twap_norm);

    return LadderSkewer::compute_conviction({
        .cfc_probability = cfc_p,
        .ofi_signal = ofi_signal,
        .twap_signal = twap_norm,
    });
}
