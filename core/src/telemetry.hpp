#ifndef TELEMETRY_HPP
#define TELEMETRY_HPP

#include <atomic>
#include <chrono>
#include <cstdlib>      // std::getenv (for AlertConfig::load_from_env)
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "spsc_ring_buffer.hpp"
#include "tick_result.hpp"  // For TickResult enum (avoids circular dependency)

/**
 * EventType: Categorías de eventos de auditoría.
 */
enum class EventType {
    ORDER_SUBMITTED,
    ORDER_FILLED,
    ORDER_CANCELLED,
    ORDER_REJECTED,
    RISK_BLOCKED,
    KILL_SWITCH_ACTIVATED,
    BALANCE_CHECK,
    RECONCILIATION_DIFF,
    FEED_DEAD,
    LATENCY_SPIKE,
    ORDER_QUERY,
    POSITION_UPDATE,
    FILL_EVENT,       // Paper-trading fill evidence (adapted from Polywhales)
    PAPER_SKIP,      // Paper-trading skip evidence (adapted from Polywhales)
};

/**
 * TelemetryEvent: Un evento de auditoría con timestamp.
 * Designed to be POD-like for SPSC passage (no dynamic allocation in hot path).
 */
struct TelemetryEvent {
    EventType type;
    uint64_t timestamp_ns;
    std::string market_slug;
    std::string details_json;   // Compact JSON: {"order_id":"...","price":"...","reason":"..."}
    std::string severity;        // INFO, WARN, ERROR, CRITICAL

    TelemetryEvent() : type(EventType::ORDER_SUBMITTED), timestamp_ns(0), severity("INFO") {}
};

/**
 * Telemetry: Asynchronous append-only logger with alerting.
 *
 * Design:
 * - Hot path: log() just pushes to SPSC ring buffer (O(1), never blocks)
 * - Writer thread: drains queue, writes JSON lines to file with O_APPEND
 * - Alerts: triggered when metrics cross configured thresholds
 * - Metrics: counters per TickResult, fills, cancels, PnL
 *
 * CRITICAL: The writer thread uses O_APPEND (append-only) — events are never
 * overwritten. If the queue is full (backpressure), the event is dropped
 * but the drop is counted (alertable metric).
 */
class Telemetry {
public:
    struct AlertConfig {
        double daily_loss_threshold_usd = 500.0;
        int consecutive_429_threshold = 5;
        int latency_spike_us = 100000;  // 100ms
        double position_divergence_threshold = 0.05;
        int feed_dead_ms = 5000;
        std::string webhook_url = "";

        /**
         * Load alert thresholds from environment variables.
         * Cold path: called once at startup, never in the hot path.
         * Follows the same pattern as RiskConfig::load_from_env().
         */
        static AlertConfig load_from_env() {
            AlertConfig cfg;

            auto get_env = [](const char* name, const char* fallback) -> std::string {
                const char* val = std::getenv(name);
                return (val != nullptr) ? std::string(val) : std::string(fallback);
            };

            auto get_env_double = [&](const char* name, double fallback) -> double {
                std::string s = get_env(name, "");
                if (s.empty()) return fallback;
                try { return std::stod(s); } catch (...) { return fallback; }
            };

            auto get_env_int = [&](const char* name, int fallback) -> int {
                std::string s = get_env(name, "");
                if (s.empty()) return fallback;
                try { return std::stoi(s); } catch (...) { return fallback; }
            };

            cfg.daily_loss_threshold_usd      = get_env_double("TELEMETRY_DAILY_LOSS_THRESHOLD_USD", 500.0);
            cfg.consecutive_429_threshold      = get_env_int("TELEMETRY_CONSECUTIVE_429_THRESHOLD", 5);
            cfg.latency_spike_us             = get_env_int("TELEMETRY_LATENCY_SPIKE_US", 100000);
            cfg.position_divergence_threshold = get_env_double("TELEMETRY_POSITION_DIVERGENCE_THRESHOLD", 0.05);
            cfg.feed_dead_ms                 = get_env_int("TELEMETRY_FEED_DEAD_MS", 5000);
            cfg.webhook_url                = get_env("TELEMETRY_WEBHOOK_URL", "");

            return cfg;
        }
    };

    explicit Telemetry(const std::string& log_file = "audit.log")
        : log_file_(log_file), running_(true),
          consecutive_429_(0), daily_loss_(0.0),
          alert_config_(AlertConfig::load_from_env()) {
        // Open log file in append mode (O_APPEND — append-only guarantee)
        log_stream_.open(log_file_, std::ios::app | std::ios::out);
        if (!log_stream_.is_open()) {
            // Fallback: stderr (should not happen in production)
            std::cerr << "⚠️ WARNING: Cannot open audit log file: " << log_file_
                      << ". Falling back to stderr." << std::endl;
            log_stream_.open("/dev/stderr", std::ios::app);
        }

        // Start writer thread
        writer_thread_ = std::thread(&Telemetry::writer_loop, this);
    }

    ~Telemetry() {
        running_.store(false, std::memory_order_release);
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
        if (log_stream_.is_open()) {
            log_stream_.flush();
            log_stream_.close();
        }
    }

    // ─── HOT PATH: Non-blocking log ─────────────────────────────────────
    // O(1) — just a ring buffer push. Never blocks, never allocates.

    void log_event(EventType type, const std::string& market_slug,
                   const std::string& details, const std::string& severity = "INFO") {
        TelemetryEvent event;
        event.type = type;
        event.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        event.market_slug = market_slug;
        event.details_json = details;
        event.severity = severity;

        // SPSC push — single producer (hot path), single consumer (writer thread)
        // If queue is full, drop the event but count it (backpressure metric)
        if (!event_queue_.try_push(event)) {
            dropped_events_.fetch_add(1, std::memory_order_relaxed);
            // Alert on sustained backpressure
            if (dropped_events_.load(std::memory_order_relaxed) % 1000 == 0) {
                log_alert("TELEMETRY_BACKPRESSURE",
                          "Dropped " + std::to_string(dropped_events_.load()) +
                          " events — increase queue capacity");
            }
        }
    }

    // ─── HOT PATH: Log risk block ──────────────────────────────────────

    void log_risk_block(TickResult result, const OrderParams& params) {
        if (result == TickResult::RISK_BLOCKED) {
            risk_blocks_.fetch_add(1, std::memory_order_release);
        }
        std::string reason;
        switch (result) {
            case TickResult::KILL_SWITCH: reason = "KILL_SWITCH"; break;
            case TickResult::RISK_BLOCKED: reason = "RISK_BLOCKED"; break;
            case TickResult::MARKET_NOT_TRADABLE: reason = "MARKET_NOT_TRADABLE"; break;
            case TickResult::INSUFFICIENT_BALANCE: reason = "INSUFFICIENT_BALANCE"; break;
            case TickResult::DUPLICATE_ORDER: reason = "DUPLICATE_ORDER"; break;
            case TickResult::NOT_PROFITABLE: reason = "NOT_PROFITABLE"; break;
            default: reason = "OTHER"; break;
        }
        log_event(EventType::RISK_BLOCKED, "",
                  "{\"reason\":\"" + reason + "\",\"order_size\":" +
                  std::to_string(params.size) + "}",
                  result == TickResult::KILL_SWITCH ? "CRITICAL" : "WARN");
    }

    // ─── Metric incrementors (hot path, atomic — O(1)) ─────────────────

    void increment_orders_submitted()    { orders_submitted_.fetch_add(1, std::memory_order_relaxed); }
    void increment_fills()              { fills_.fetch_add(1, std::memory_order_relaxed); }
    void increment_cancels()           { cancels_.fetch_add(1, std::memory_order_relaxed); }
    void increment_429() {
        int count = consecutive_429_.fetch_add(1, std::memory_order_relaxed);
        if (count + 1 >= alert_config_.consecutive_429_threshold) {
            log_alert("RATE_LIMIT_STREAK",
                      "Consecutive 429 errors: " + std::to_string(count + 1));
        }
    }
    void reset_429_streak()             { consecutive_429_.store(0, std::memory_order_release); }

    void add_realized_pnl(double pnl)    { realized_pnl_.fetch_add(pnl, std::memory_order_relaxed); }
    void add_daily_loss(double loss)     { daily_loss_.fetch_add(loss, std::memory_order_relaxed); }

    void record_tick_result(TickResult result) {
        switch (result) {
            case TickResult::OK: tickresult_ok_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::NO_SIGNAL: tickresult_no_signal_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::NO_EDGE: tickresult_no_edge_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::NOT_PROFITABLE: tickresult_not_profitable_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::RISK_BLOCKED: tickresult_risk_blocked_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::KILL_SWITCH: tickresult_kill_switch_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::MARKET_NOT_TRADABLE: tickresult_market_not_tradable_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::DUPLICATE_ORDER: tickresult_duplicate_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::INSUFFICIENT_BALANCE: tickresult_insufficient_balance_.fetch_add(1, std::memory_order_relaxed); break;
            case TickResult::STALE_BOOK: tickresult_stale_book_.fetch_add(1, std::memory_order_relaxed); break;
        }
    }

    // ─── Evidence Recording (adapted from Polywhales evidence.ts) ─────────
    // Records paper-trading evidence for gate reports and analysis.

    void record_paper_fill(const std::string& action_key,
                           const std::string& token_id,
                           bool is_buy,
                           uint64_t requested_price_micros,
                           uint64_t filled_price_micros,
                           uint64_t filled_units,
                           uint64_t slippage_bps) {
        // Calculate fill ratio and edge capture
        uint64_t fill_ratio = (filled_units > 0) ? (filled_units * 1'000'000ULL / filled_units) : 0;
        uint64_t edge_capture = (is_buy && filled_price_micros < requested_price_micros)
            ? (requested_price_micros - filled_price_micros)
            : 0;

        std::string evidence = "{\"type\":\"fill\",\"action_key\":\"" + action_key +
            "\",\"token_id\":\"" + token_id +
            "\",\"side\":\"" + (is_buy ? "BUY" : "SELL") +
            "\",\"fill_ratio_ppm\":" + std::to_string(fill_ratio) +
            ",\"slippage_bps\":" + std::to_string(slippage_bps) +
            ",\"edge_capture_micros\":" + std::to_string(edge_capture) + "}";

        log_event(EventType::FILL_EVENT, token_id, evidence, "INFO");
        paper_fills_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_paper_skip(const std::string& action_key,
                           const std::string& token_id,
                           const std::string& reason_code) {
        std::string evidence = "{\"type\":\"skip\",\"action_key\":\"" + action_key +
            "\",\"token_id\":\"" + token_id +
            "\",\"reason_code\":\"" + reason_code + "\"}";

        log_event(EventType::RISK_BLOCKED, token_id, evidence, "INFO");
        paper_skips_.fetch_add(1, std::memory_order_relaxed);
    }

    // ─── Alerts (cold path) ────────────────────────────────────────────

    void log_alert(const std::string& alert_type, const std::string& message) {
        log_event(EventType::RISK_BLOCKED, "",
                  "{\"alert_type\":\"" + alert_type + "\",\"message\":\"" +
                  message + "\"}", "CRITICAL");

        // Daily loss alert
        double loss = daily_loss_.load(std::memory_order_relaxed);
        if (loss > alert_config_.daily_loss_threshold_usd) {
            log_event(EventType::RISK_BLOCKED, "",
                      "{\"alert_type\":\"DAILY_LOSS_EXCEEDED\",\"loss\":" +
                      std::to_string(loss) + "}", "CRITICAL");
        }

        // Webhook notification (if configured)
        if (!alert_config_.webhook_url.empty()) {
            send_webhook(alert_type, message);
        }
    }

    // ─── Metrics snapshot (cold path — for dashboard) ──────────────────

    std::string get_metrics_json() const {
        std::string json = "{";
        json += "\"orders_submitted\":" + std::to_string(orders_submitted_.load()) + ",";
        json += "\"fills\":" + std::to_string(fills_.load()) + ",";
        json += "\"cancels\":" + std::to_string(cancels_.load()) + ",";
        json += "\"risk_blocks\":" + std::to_string(risk_blocks_.load()) + ",";
        json += "\"consecutive_429\":" + std::to_string(consecutive_429_.load()) + ",";
        json += "\"dropped_events\":" + std::to_string(dropped_events_.load()) + ",";
        json += "\"daily_loss\":" + std::to_string(daily_loss_.load()) + ",";
        json += "\"realized_pnl\":" + std::to_string(realized_pnl_.load()) + ",";
        json += "\"tickresult_ok\":" + std::to_string(tickresult_ok_.load()) + ",";
        json += "\"tickresult_no_signal\":" + std::to_string(tickresult_no_signal_.load()) + ",";
        json += "\"tickresult_no_edge\":" + std::to_string(tickresult_no_edge_.load()) + ",";
        json += "\"tickresult_not_profitable\":" + std::to_string(tickresult_not_profitable_.load()) + ",";
        json += "\"tickresult_risk_blocked\":" + std::to_string(tickresult_risk_blocked_.load()) + ",";
        json += "\"tickresult_kill_switch\":" + std::to_string(tickresult_kill_switch_.load()) + ",";
        json += "\"tickresult_market_not_tradable\":" + std::to_string(tickresult_market_not_tradable_.load()) + ",";
        json += "\"tickresult_duplicate\":" + std::to_string(tickresult_duplicate_.load()) + ",";
        json += "\"tickresult_insufficient_balance\":" + std::to_string(tickresult_insufficient_balance_.load()) + ",";
        json += "\"tickresult_stale_book\":" + std::to_string(tickresult_stale_book_.load()) + ",";
        json += "\"paper_fills\":" + std::to_string(paper_fills_.load()) + ",";
        json += "\"paper_skips\":" + std::to_string(paper_skips_.load());
        json += "}";
        return json;
    }

    void set_alert_config(const AlertConfig& config) {
        alert_config_ = config;
    }

    size_t get_dropped_count() const {
        return dropped_events_.load(std::memory_order_relaxed);
    }

private:
    static constexpr size_t QUEUE_CAPACITY = 8192;

    std::string log_file_;
    std::ofstream log_stream_;
    std::thread writer_thread_;
    std::atomic<bool> running_;
    SPSC_RingBuffer<TelemetryEvent, QUEUE_CAPACITY> event_queue_;

    // Alert config (loaded from env at construction)
    AlertConfig alert_config_;

    // Counters (all atomic — O(1) hot path updates)
    std::atomic<uint64_t> orders_submitted_{0};
    std::atomic<uint64_t> fills_{0};
    std::atomic<uint64_t> cancels_{0};
    std::atomic<uint64_t> risk_blocks_{0};
    std::atomic<int> consecutive_429_{0};
    std::atomic<uint64_t> dropped_events_{0};
    std::atomic<double> daily_loss_{0.0};
    std::atomic<double> realized_pnl_{0.0};

    // TickResult counters
    std::atomic<uint64_t> tickresult_ok_{0};
    std::atomic<uint64_t> tickresult_no_signal_{0};
    std::atomic<uint64_t> tickresult_no_edge_{0};
    std::atomic<uint64_t> tickresult_not_profitable_{0};
    std::atomic<uint64_t> tickresult_risk_blocked_{0};
    std::atomic<uint64_t> tickresult_kill_switch_{0};
    std::atomic<uint64_t> tickresult_market_not_tradable_{0};
    std::atomic<uint64_t> tickresult_duplicate_{0};
    std::atomic<uint64_t> tickresult_insufficient_balance_{0};
    std::atomic<uint64_t> tickresult_stale_book_{0};

    // Paper-trading evidence counters (adapted from Polywhales evidence.ts)
    std::atomic<uint64_t> paper_fills_{0};
    std::atomic<uint64_t> paper_skips_{0};

    void writer_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            auto event_opt = event_queue_.try_pop();
            if (event_opt) {
                write_event(event_opt.value());
            } else {
                // No events — sleep briefly to avoid busy loop
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
        // Drain remaining events on shutdown
        while (true) {
            auto event_opt = event_queue_.try_pop();
            if (!event_opt) break;
            write_event(event_opt.value());
        }
    }

    void write_event(const TelemetryEvent& event) {
        // JSON line format (append-only, O_APPEND)
        // Example: {"ts":1234567890,"type":"ORDER_SUBMITTED","market":"BTC-USD","severity":"INFO","details":{...}}
        std::string type_str;
        switch (event.type) {
            case EventType::ORDER_SUBMITTED: type_str = "ORDER_SUBMITTED"; break;
            case EventType::ORDER_FILLED: type_str = "ORDER_FILLED"; break;
            case EventType::ORDER_CANCELLED: type_str = "ORDER_CANCELLED"; break;
            case EventType::ORDER_REJECTED: type_str = "ORDER_REJECTED"; break;
            case EventType::RISK_BLOCKED: type_str = "RISK_BLOCKED"; break;
            case EventType::KILL_SWITCH_ACTIVATED: type_str = "KILL_SWITCH_ACTIVATED"; break;
            case EventType::BALANCE_CHECK: type_str = "BALANCE_CHECK"; break;
            case EventType::RECONCILIATION_DIFF: type_str = "RECONCILIATION_DIFF"; break;
            case EventType::FEED_DEAD: type_str = "FEED_DEAD"; break;
            case EventType::LATENCY_SPIKE: type_str = "LATENCY_SPIKE"; break;
            case EventType::ORDER_QUERY: type_str = "ORDER_QUERY"; break;
            case EventType::POSITION_UPDATE: type_str = "POSITION_UPDATE"; break;
            default: type_str = "UNKNOWN"; break;
        }

        // Sanitized logging: never include private keys, API keys, or full signatures
        log_stream_ << "{\"ts\":" << event.timestamp_ns
                    << ",\"type\":\"" << type_str << "\""
                    << ",\"market\":\"" << event.market_slug << "\""
                    << ",\"severity\":\"" << event.severity << "\""
                    << ",\"details\":" << event.details_json
                    << "}\n";
        log_stream_.flush();
    }

    void send_webhook(const std::string& alert_type, const std::string& message) {
        // In production: HTTP POST to webhook URL
        // For now: just log (webhook is cold path, not hot path)
        // Never include secrets in webhook payload
        (void)alert_type; (void)message;
    }
};

#endif // TELEMETRY_HPP
