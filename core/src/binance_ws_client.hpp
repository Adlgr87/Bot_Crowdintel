// ─────────────────────────────────────────────────────────────────────────────
// binance_ws_client.hpp — Phase 1 stub (DATA-PIPELINE-AGENT)
//
// Persistent WebSocket client for Binance Spot streams. Runs on a cold
// thread. Produces MarketState structs to a SPSC ring buffer.
//
// INVARIANTS:
//   - NEVER allocates on the hot path (ring buffer is fixed-size)
//   - NEVER blocks the hot path (lock-free SPSC push, drop-oldest policy)
//   - Uses CLOCK_MONOTONIC_RAW for all internal timestamps
//
// TODO(P1-T1): Implement actual Binance WS connection + JSON parsing.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <cstring>
#include "../include/spsc_ring_buffer.hpp"

// Forward-declare the MarketState struct (defined in ofi_calculator.hpp).
struct MarketState;

// ── Configuration (constexpr-compatible for compile-time tuning) ────────────
struct BinanceConfig {
    static constexpr const char* API_ENDPOINT =
        "wss://stream.binance.com:9443/stream";
    static constexpr uint32_t RING_CAPACITY = 4096;
    static constexpr uint64_t HEARTBEAT_INTERVAL_NS = 30'000'000'000ULL; // 30s
    static constexpr uint32_t MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr uint64_t RECONNECT_BASE_DELAY_NS = 1'000'000'000ULL;  // 1s
    static constexpr uint64_t RECONNECT_MAX_DELAY_NS = 30'000'000'000ULL;  // 30s

    // Streams: btcusdt@depth20@100ms/btcusdt@trade/btcusdt@kline_1m
    const char* symbol = "btcusdt";
    const char* depth_stream = "@depth20@100ms";
    const char* trade_stream = "@trade";
    const char* kline_stream = "@kline_1m";
};

// ── Public Interface ─────────────────────────────────────────────────────────
class BinanceWSClient {
public:
    explicit BinanceWSClient(const BinanceConfig& cfg)
        : cfg_(cfg), running_(false), reconnect_count_(0) {}

    // ── Thread lifecycle (cold path) ─────────────────────────────────────────
    void start();        // Blocks. Connect + read loop. Cold thread only.
    void stop();         // Sets running_=false, signals thread exit.
    bool is_connected() const noexcept { return connected_; }

    // ── Access to the output ring buffer (consumed by hot path) ─────────────
    // Capacity is always BinanceConfig::RING_CAPACITY (power of 2).
    using MarketStateQ = SPSC_RingBuffer<MarketState, BinanceConfig::RING_CAPACITY>;
    MarketStateQ& market_q() noexcept { return market_q_; }

    // ── Metrics (cold path only, never called from hot path) ────────────────
    uint64_t events_produced() const noexcept { return events_produced_; }
    uint64_t reconnect_count() const noexcept { return reconnect_count_; }

private:
    BinanceConfig cfg_;
    MarketStateQ market_q_;
    bool running_;
    bool connected_ = false;
    uint32_t reconnect_count_;
    uint64_t events_produced_ = 0;
    uint64_t last_heartbeat_ns_ = 0;
};
