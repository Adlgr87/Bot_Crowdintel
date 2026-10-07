// ─────────────────────────────────────────────────────────────────────────────
// ofi_calculator.hpp — Order Flow Imbalance (Cont et al., 2014)
//
// Incremental O(1) calculator. Receives raw book changes from BinanceWSClient
// and emits MarketState structs (the CfC input vector).
//
// INVARIANTS:
//   - O(1) per market event (EWMA decay, no buffer scan)
//   - Zero heap allocation (stack + fixed ring buffer)
//   - Thread-safe: cold-path producer (Binance thread) → hot-path consumer
//     via SPSC ring (drop-oldest on overflow)
//
// TODO(P1-T2): Implement event parsing and OFI math.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <array>

// ── MarketState: the 6-9 feature vector consumed by CfC ──────────────────────
struct alignas(64) MarketState {
    double ofi_normalized;       // OFI / (B_vol + A_vol), range [-1, 1]
    double trade_intensity;      // EWMA trades/sec
    double spread_bps;           // (ask - bid) / mid * 10000
    double depth_imbalance;      // (B_vol - A_vol) / (B_vol + A_vol)
    double microprice;           // weighted: (B_vol*ask + A_vol*bid) / (B_vol + A_vol)
    double mid_velocity;         // Δmid / Δt in bps/sec

    uint64_t timestamp_ns;       // CLOCK_MONOTONIC_RAW
    double delta_t_sec;          // time since last event

    // Phase-2 additions (optional, zero-initialized for backward compat):
    double realized_vol_1h = 0.0;
    double funding_rate = 0.0;
    double volume_zscore = 0.0;
};
static_assert(sizeof(MarketState) <= 128, "Cache-line aligned, no padding bloat");

// ── Configuration ────────────────────────────────────────────────────────────
struct OFIConfig {
    static constexpr double DECAY_LAMBDA = 0.95;        // EWMA decay
    static constexpr uint32_t HISTORY_DEPTH = 64;       // ring buffer for spikes
    static constexpr double MIN_PRICE_TICK = 0.01;      // USD
    static constexpr double DEPTH_SCALE = 100.0;        // normalize to basis
};

// ── OFI Calculator ───────────────────────────────────────────────────────────
// Cold-path only. Produces MarketState into a ring buffer.
class OFICalculator {
public:
    explicit OFICalculator(const OFIConfig& cfg) : cfg_(cfg) {}

    // Process one order-book event (called from Binance thread).
    // bid_vol, ask_vol: volumes at best bid/ask in USD.
    // bid_px, ask_px: prices in USD (e.g., 26500.00).
    // is_trade: true if this is a trade event (not a book update).
    void on_event(double bid_px, double ask_px,
                  double bid_vol, double ask_vol,
                  bool is_trade, uint64_t now_ns) noexcept;

    // Get latest state (called by hot-path drain). Returns false if no events yet.
    bool current_state(MarketState& out) const noexcept;

    // Reset accumulators (called at window start by WindowShield).
    void reset() noexcept;

    // Access to the SPSC ring for hot-path drain.
    // Uses MarketStateQ from binance_ws_client.hpp.
    // NOTE: defined inline to avoid include cycle.

private:
    OFIConfig cfg_;
    double ofi_running_ = 0.0;
    double trade_intensity_ewma_ = 0.0;
    double prev_bid_px_ = 0.0;
    double prev_ask_px_ = 0.0;
    double prev_mid_ = 0.0;
    uint64_t last_event_ns_ = 0;
    uint64_t event_count_ = 0;

    // Fixed ring for spike detection (Hurst exponent estimation).
    std::array<double, OFIConfig::HISTORY_DEPTH> price_history_{};
    uint32_t price_head_ = 0;
};
