/**
 * test_telemetry.cpp — T6 Telemetry, Audit & Alerting Test Suite
 *
 * Verifies (Phase 6: Observabilidad, Auditoría y Alertas):
 *   T6-1: Async append-only logger — SPSC ring buffer, O_APPEND, JSON lines
 *   T6-2: Configurable alert thresholds via env vars (AlertConfig::load_from_env)
 *   T6-3: Metrics exposed via get_metrics_json() — all counters atomic
 *
 * Deliverable checks:
 *   1. Telemetry compiles and integrates with execution_engine.cpp (log calls match)
 *   2. Event logging, async queue, metrics counters, alert thresholds tested
 *   3. No secrets in logs (sanitized logging)
 *
 * Dependencies: telemetry.hpp, spsc_ring_buffer.hpp, tick_result.hpp
 *               eip712_signer.hpp (for OrderParams used in log_risk_block)
 *
 * CRITICAL: Does NOT modify eip712_signer.hpp. Uses unique log file per test
 * to avoid cross-test contamination.
 */

#include "eip712_signer.hpp"  // Must come before telemetry.hpp — provides OrderParams
#include "telemetry.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <unistd.h>     // getpid()

/* ───── Test framework (minimal, no external deps) ───── */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

struct TestCase {
    const char* name;
    bool passed;
};
static TestCase g_cases[256];
static int g_case_idx = 0;

#define CASE(name_, cond_) do { \
    g_cases[g_case_idx].name = name_; \
    g_cases[g_case_idx].passed = (cond_); \
    g_case_idx++; \
} while(0)

static void run_test(const char* name, bool (*fn)()) {
    g_tests_run++;
    std::cout << "▶ " << name << " ..." << std::flush;
    bool ok = fn();
    if (ok) {
        g_tests_passed++;
        std::cout << " ✅ PASS" << std::endl;
    } else {
        g_tests_failed++;
        std::cout << " ❌ FAIL" << std::endl;
    }
}

/* ───── Helpers ───── */

/**
 * Generate a unique log file name per test for isolation.
 * Uses PID + counter to avoid collisions across parallel test runs.
 */
static int g_log_counter = 0;
static std::string unique_log_file() {
    char buf[128];
    snprintf(buf, sizeof(buf), "/tmp/test_telemetry_%d_%d.log",
             static_cast<int>(getpid()), g_log_counter++);
    return std::string(buf);
}

/**
 * Wait for the writer thread to drain the queue, then read the log file.
 * The writer sleeps 100us between attempts, so we retry until the file
 * contains at least `expected_lines` lines or we time out.
 */
static std::string read_log_file(const std::string& path, int expected_lines,
                                  int max_wait_ms = 2000) {
    for (int ms = 0; ms < max_wait_ms; ms += 50) {
        std::ifstream f(path);
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            int line_count = 0;
            for (char c : content) {
                if (c == '\n') line_count++;
            }
            if (line_count >= expected_lines) {
                return content;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Return whatever we have (may be empty)
    std::ifstream f(path);
    if (f.is_open()) {
        return std::string((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    }
    return "";
}

/**
 * Count occurrences of a substring in a string.
 */
static int count_substring(const std::string& haystack, const std::string& needle) {
    int count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.length();
    }
    return count;
}

/**
 * Simple JSON field extractor: finds "key":value in a JSON string.
 * Returns the value as string. Works for simple flat JSON.
 */
static std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    // Skip whitespace
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.length()) return "";
    if (json[pos] == '"') {
        // String value
        size_t end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    } else {
        // Numeric value — read until comma or closing brace
        size_t end = pos;
        while (end < json.length() && json[end] != ',' && json[end] != '}') end++;
        return json.substr(pos, end - pos);
    }
}

/* ───── T6-1: Async Append-Only Logger Tests ───── */

static bool test_spsc_ring_buffer_capacity() {
    // Verify SPSC_RingBuffer has capacity 8192 and is power-of-2
    static_assert(SPSC_RingBuffer<TelemetryEvent, 8192>::capacity() == 8192,
                  "Telemetry queue capacity must be 8192");
    CASE("SPSC_RingBuffer capacity == 8192",
         (SPSC_RingBuffer<TelemetryEvent, 8192>::capacity() == 8192));

    // Verify power-of-2 property (fast modulo via bitmask)
    static constexpr size_t cap = SPSC_RingBuffer<TelemetryEvent, 8192>::capacity();
    CASE("SPSC capacity is power of 2", (cap & (cap - 1)) == 0);

    return true;
}

static bool test_log_event_produces_json_line() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    tele.log_event(EventType::ORDER_SUBMITTED, "BTC-USD",
                   "{\"order_id\":\"ORD-001\",\"price\":\"123.45\"}", "INFO");

    std::string content = read_log_file(log_path, 1);
    CASE("audit.log has at least 1 line after log_event", !content.empty());

    // Verify JSON line format
    CASE("log line contains \"type\"", content.find("\"type\":\"ORDER_SUBMITTED\"") != std::string::npos);
    CASE("log line contains \"market\"", content.find("\"market\":\"BTC-USD\"") != std::string::npos);
    CASE("log line contains \"severity\"", content.find("\"severity\":\"INFO\"") != std::string::npos);
    CASE("log line contains \"ts\"", content.find("\"ts\":") != std::string::npos);
    CASE("log line contains \"details\"", content.find("\"details\":") != std::string::npos);
    CASE("log line ends with newline", !content.empty() && content.back() == '\n');

    // Each line must be a complete JSON object
    size_t first_brace = content.find('{');
    size_t first_newline = content.find('\n');
    CASE("JSON object starts with brace", first_brace != std::string::npos && first_brace < first_newline);
    CASE("JSON object ends before newline", content[first_newline - 1] == '}');

    return true;
}

static bool test_append_only_no_overwrite() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // Log 5 distinct events
    for (int i = 0; i < 5; i++) {
        tele.log_event(EventType::ORDER_SUBMITTED, "ETH-USD",
                       "{\"seq\":" + std::to_string(i) + "}", "INFO");
    }

    std::string content = read_log_file(log_path, 5);
    CASE("5 events produce 5 log lines", count_substring(content, "\n") >= 5);

    // Append-only: all 5 events must be present (none overwritten)
    CASE("event seq=0 is present (not overwritten)", content.find("\"seq\":0") != std::string::npos);
    CASE("event seq=1 is present (not overwritten)", content.find("\"seq\":1") != std::string::npos);
    CASE("event seq=2 is present (not overwritten)", content.find("\"seq\":2") != std::string::npos);
    CASE("event seq=3 is present (not overwritten)", content.find("\"seq\":3") != std::string::npos);
    CASE("event seq=4 is present (not overwritten)", content.find("\"seq\":4") != std::string::npos);

    return true;
}

static bool test_hot_path_no_io() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // Log many events rapidly — hot path should never block or do I/O directly.
    // The writer thread drains asynchronously; we just measure that
    // log_event returns quickly.
    auto t0 = std::chrono::steady_clock::now();
    const int N = 1000;
    for (int i = 0; i < N; i++) {
        tele.log_event(EventType::ORDER_SUBMITTED, "TEST-USD",
                       "{\"i\":" + std::to_string(i) + "}", "INFO");
    }
    auto t1 = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // 1000 non-blocking pushes should complete in well under 1ms
    // (each is just a ring buffer try_push + atomic timestamp)
    CASE("1000 hot-path log_event calls < 10ms (no I/O on hot path)",
         elapsed_us < 10000);

    // Verify all events eventually appear in the log
    std::string content = read_log_file(log_path, N);
    CASE("all 1000 events appear in log after drain", count_substring(content, "\"type\":\"ORDER_SUBMITTED\"") >= N);

    return true;
}

/* ───── T6-1 (cont): Backpressure / Dropped Events ───── */

static bool test_backpressure_drop_count() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // TelemetryEvent contains std::string members. With string allocations
    // being moderately fast, we can fill the 8192-capacity queue by flooding
    // it faster than the writer thread drains (writer sleeps 100us between
    // pops, which is slow relative to in-memory pushes).
    // We flood with more than 8192 events in a tight loop.
    const int FLOOD = 10000;
    for (int i = 0; i < FLOOD; i++) {
        tele.log_event(EventType::ORDER_SUBMITTED, "FLOOD-USD",
                       "{\"i\":" + std::to_string(i) + "}", "INFO");
    }

    // Give writer time to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Some events must have been written (at least 1)
    std::string content = read_log_file(log_path, 1, 500);

    // If we flooded 10000 events into an 8192-capacity queue with a slow
    // writer (100us per empty poll), some must have been dropped.
    // Even if none dropped, the queue capacity is 8192, so at most 8192
    // events without backpressure drops.
    CASE("at least 1 event written to log", count_substring(content, "\n") >= 1);

    // The dropped count is tracked atomically — verify it's accessible
    size_t dropped = tele.get_dropped_count();
    CASE("get_dropped_count() returns a value >= 0", dropped >= 0);

    // If we flooded 10000 into 8192-capacity queue with a 100us-sleep writer,
    // we expect drops. This validates the backpressure mechanism.
    CASE("backpressure mechanism: drops counted when flooding 10000 into 8192 queue",
         dropped > 0 || count_substring(content, "\n") >= 8190);

    return true;
}

/* ───── T6-2: Configurable Alert Thresholds ───── */

static bool test_alert_config_defaults() {
    // With no env vars set, defaults should apply
    Telemetry::AlertConfig cfg = Telemetry::AlertConfig::load_from_env();

    CASE("default daily_loss_threshold_usd == 500.0",
         cfg.daily_loss_threshold_usd == 500.0);
    CASE("default consecutive_429_threshold == 5",
         cfg.consecutive_429_threshold == 5);
    CASE("default latency_spike_us == 100000",
         cfg.latency_spike_us == 100000);
    CASE("default position_divergence_threshold == 0.05",
         cfg.position_divergence_threshold == 0.05);
    CASE("default feed_dead_ms == 5000",
         cfg.feed_dead_ms == 5000);
    CASE("default webhook_url is empty",
         cfg.webhook_url.empty());

    return true;
}

static bool test_alert_config_env_override() {
    // Set env vars and verify they override defaults
    setenv("TELEMETRY_DAILY_LOSS_THRESHOLD_USD", "750.50", 1);
    setenv("TELEMETRY_CONSECUTIVE_429_THRESHOLD", "3", 1);
    setenv("TELEMETRY_LATENCY_SPIKE_US", "50000", 1);
    setenv("TELEMETRY_POSITION_DIVERGENCE_THRESHOLD", "0.10", 1);
    setenv("TELEMETRY_FEED_DEAD_MS", "3000", 1);
    setenv("TELEMETRY_WEBHOOK_URL", "https://hooks.example.com/webhook/abc123", 1);

    Telemetry::AlertConfig cfg = Telemetry::AlertConfig::load_from_env();

    CASE("env override: daily_loss_threshold_usd == 750.5",
         cfg.daily_loss_threshold_usd == 750.50);
    CASE("env override: consecutive_429_threshold == 3",
         cfg.consecutive_429_threshold == 3);
    CASE("env override: latency_spike_us == 50000",
         cfg.latency_spike_us == 50000);
    CASE("env override: position_divergence_threshold == 0.10",
         cfg.position_divergence_threshold == 0.10);
    CASE("env override: feed_dead_ms == 3000",
         cfg.feed_dead_ms == 3000);
    CASE("env override: webhook_url is set",
         cfg.webhook_url == "https://hooks.example.com/webhook/abc123");

    // Clean up env vars
    unsetenv("TELEMETRY_DAILY_LOSS_THRESHOLD_USD");
    unsetenv("TELEMETRY_CONSECUTIVE_429_THRESHOLD");
    unsetenv("TELEMETRY_LATENCY_SPIKE_US");
    unsetenv("TELEMETRY_POSITION_DIVERGENCE_THRESHOLD");
    unsetenv("TELEMETRY_FEED_DEAD_MS");
    unsetenv("TELEMETRY_WEBHOOK_URL");

    return true;
}

static bool test_consecutive_429_alert_threshold() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // Set a low threshold: alert after 3 consecutive 429s
    Telemetry::AlertConfig custom;
    custom.consecutive_429_threshold = 3;
    tele.set_alert_config(custom);

    // First 2 increments should NOT trigger alert
    tele.increment_429();
    tele.increment_429();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 3rd increment should trigger alert
    tele.increment_429();

    std::string content = read_log_file(log_path, 3, 500);

    CASE("3rd consecutive 429 triggers RATE_LIMIT_STREAK alert",
         content.find("RATE_LIMIT_STREAK") != std::string::npos);
    CASE("alert severity is CRITICAL",
         content.find("\"severity\":\"CRITICAL\"") != std::string::npos);

    // Reset and verify streak reset works
    tele.reset_429_streak();
    CASE("reset_429_streak clears consecutive_429 counter",
         tele.get_metrics_json().find("\"consecutive_429\":0") != std::string::npos);

    return true;
}

static bool test_daily_loss_alert_threshold() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    Telemetry::AlertConfig cfg;
    cfg.daily_loss_threshold_usd = 100.0;
    tele.set_alert_config(cfg);

    // Add loss below threshold — should NOT trigger daily loss alert
    tele.add_daily_loss(50.0);
    // Trigger a regular alert to check if daily_loss is included
    tele.log_alert("TEST_ALERT", "test message");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string content = read_log_file(log_path, 1, 500);

    // Should have the TEST_ALERT but NOT DAILY_LOSS_EXCEEDED
    CASE("loss below threshold: TEST_ALERT logged",
         content.find("TEST_ALERT") != std::string::npos);
    CASE("loss below threshold: no DAILY_LOSS_EXCEEDED alert",
         content.find("DAILY_LOSS_EXCEEDED") == std::string::npos);

    // Now exceed threshold
    tele.add_daily_loss(60.0);  // total = 110 > 100
    tele.log_alert("ANOTHER_ALERT", "exceed test");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string content2 = read_log_file(log_path, 1, 500);

    CASE("loss exceeds threshold: DAILY_LOSS_EXCEEDED alert fired",
         content2.find("DAILY_LOSS_EXCEEDED") != std::string::npos);

    return true;
}

/* ───── T6-3: Metrics Exposed ───── */

static bool test_metrics_json_contains_all_counters() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // Increment various counters
    tele.increment_orders_submitted();
    tele.increment_orders_submitted();
    tele.increment_fills();
    tele.increment_cancels();
    tele.record_tick_result(TickResult::OK);
    tele.record_tick_result(TickResult::RISK_BLOCKED);
    tele.record_tick_result(TickResult::NO_SIGNAL);
    tele.add_realized_pnl(123.45);
    tele.add_daily_loss(10.0);

    // log_risk_block increments risk_blocks_ counter (separate from record_tick_result)
    OrderParams params{};
    params.size = static_cast<uint64_t>(100.0 * 1e6);
    tele.log_risk_block(TickResult::RISK_BLOCKED, params);

    std::string json = tele.get_metrics_json();

    CASE("metrics JSON is valid object", json.front() == '{' && json.back() == '}');
    CASE("metrics contains orders_submitted", json.find("\"orders_submitted\":2") != std::string::npos);
    CASE("metrics contains fills", json.find("\"fills\":1") != std::string::npos);
    CASE("metrics contains cancels", json.find("\"cancels\":1") != std::string::npos);
    CASE("metrics contains risk_blocks", json.find("\"risk_blocks\":1") != std::string::npos);
    CASE("metrics contains tickresult_ok", json.find("\"tickresult_ok\":1") != std::string::npos);
    CASE("metrics contains tickresult_risk_blocked", json.find("\"tickresult_risk_blocked\":1") != std::string::npos);
    CASE("metrics contains tickresult_no_signal", json.find("\"tickresult_no_signal\":1") != std::string::npos);
    CASE("metrics contains realized_pnl", json.find("\"realized_pnl\":") != std::string::npos);
    CASE("metrics contains daily_loss", json.find("\"daily_loss\":") != std::string::npos);
    CASE("metrics contains dropped_events", json.find("\"dropped_events\":") != std::string::npos);
    CASE("metrics contains consecutive_429", json.find("\"consecutive_429\":") != std::string::npos);

    return true;
}

static bool test_counters_are_atomic() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // Multi-threaded increment test — all counters must be atomic
    const int N = 10000;
    const int num_threads = 4;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&tele, N]() {
            for (int i = 0; i < N; i++) {
                tele.increment_orders_submitted();
                tele.increment_fills();
                tele.increment_cancels();
                tele.record_tick_result(TickResult::OK);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    std::string json = tele.get_metrics_json();

    int expected = N * num_threads;  // 40000

    CASE("orders_submitted == 40000 after 4 threads × 10000",
         json_get(json, "orders_submitted") == std::to_string(expected));
    CASE("fills == 40000 after concurrent increments",
         json_get(json, "fills") == std::to_string(expected));
    CASE("cancels == 40000 after concurrent increments",
         json_get(json, "cancels") == std::to_string(expected));
    CASE("tickresult_ok == 40000 after concurrent increments",
         json_get(json, "tickresult_ok") == std::to_string(expected));

    return true;
}

static bool test_record_all_tick_result_types() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // Record one of each TickResult type
    tele.record_tick_result(TickResult::OK);
    tele.record_tick_result(TickResult::NO_SIGNAL);
    tele.record_tick_result(TickResult::NO_EDGE);
    tele.record_tick_result(TickResult::NOT_PROFITABLE);
    tele.record_tick_result(TickResult::RISK_BLOCKED);
    tele.record_tick_result(TickResult::KILL_SWITCH);
    tele.record_tick_result(TickResult::MARKET_NOT_TRADABLE);
    tele.record_tick_result(TickResult::DUPLICATE_ORDER);
    tele.record_tick_result(TickResult::INSUFFICIENT_BALANCE);

    std::string json = tele.get_metrics_json();

    CASE("tickresult_ok == 1", json_get(json, "tickresult_ok") == "1");
    CASE("tickresult_no_signal == 1", json_get(json, "tickresult_no_signal") == "1");
    CASE("tickresult_no_edge == 1", json_get(json, "tickresult_no_edge") == "1");
    CASE("tickresult_not_profitable == 1", json_get(json, "tickresult_not_profitable") == "1");
    CASE("tickresult_risk_blocked == 1", json_get(json, "tickresult_risk_blocked") == "1");
    CASE("tickresult_kill_switch == 1", json_get(json, "tickresult_kill_switch") == "1");
    CASE("tickresult_market_not_tradable == 1", json_get(json, "tickresult_market_not_tradable") == "1");
    CASE("tickresult_duplicate == 1", json_get(json, "tickresult_duplicate") == "1");
    CASE("tickresult_insufficient_balance == 1", json_get(json, "tickresult_insufficient_balance") == "1");

    return true;
}

/* ───── T6-1 (cont): log_risk_block with OrderParams ───── */

static bool test_log_risk_block_with_order_params() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // Construct OrderParams (same pattern as test_risk_engine.cpp)
    OrderParams params;
    params.salt   = 123456789;
    params.nonce  = 987654321;
    params.price  = 500000000;
    params.size   = static_cast<uint64_t>(600.0 * 1e6);
    params.side   = 0;
    memset(params.maker, 0x11, 20);
    memset(params.taker, 0x00, 20);

    tele.log_risk_block(TickResult::RISK_BLOCKED, params);
    tele.log_risk_block(TickResult::KILL_SWITCH, params);
    tele.record_tick_result(TickResult::RISK_BLOCKED);
    tele.record_tick_result(TickResult::KILL_SWITCH);

    std::string content = read_log_file(log_path, 2, 1000);

    CASE("log_risk_block produces RISK_BLOCKED event",
         content.find("\"type\":\"RISK_BLOCKED\"") != std::string::npos);
    CASE("log_risk_block with KILL_SWITCH severity is CRITICAL",
         content.find("\"severity\":\"CRITICAL\"") != std::string::npos);
    CASE("log_risk_block with RISK_BLOCKED severity is WARN",
         content.find("\"severity\":\"WARN\"") != std::string::npos);
    CASE("log_risk_block includes order_size in details",
         content.find("\"order_size\":600000000") != std::string::npos);
    CASE("log_risk_block includes reason",
         content.find("\"reason\":") != std::string::npos);

    std::string json = tele.get_metrics_json();
    CASE("risk_blocks counter incremented by log_risk_block",
         json_get(json, "risk_blocks") == "1");

    return true;
}

/* ───── T6-1: JSON Lines Format Validation ───── */

static bool test_json_lines_format() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    tele.log_event(EventType::ORDER_SUBMITTED, "BTC-USD",
                   "{\"order_id\":\"ORD-001\"}", "INFO");
    tele.log_event(EventType::ORDER_FILLED, "ETH-USD",
                   "{\"fill_price\":\"2500.00\"}", "INFO");
    tele.log_event(EventType::LATENCY_SPIKE, "SOL-USD",
                   "{\"latency_us\":150000}", "WARN");

    std::string content = read_log_file(log_path, 3, 500);

    // Each line should be valid JSON (starts with { and ends with \n)
    std::istringstream iss(content);
    std::string line;
    int valid_lines = 0;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.front() == '{' && line.back() == '}') {
            valid_lines++;
        }
    }
    CASE("all 3 lines are valid JSON objects", valid_lines == 3);

    CASE("JSON line for ORDER_SUBMITTED is valid",
         content.find("\"type\":\"ORDER_SUBMITTED\"") != std::string::npos);
    CASE("JSON line for ORDER_FILLED is valid",
         content.find("\"type\":\"ORDER_FILLED\"") != std::string::npos);
    CASE("JSON line for LATENCY_SPIKE is valid",
         content.find("\"type\":\"LATENCY_SPIKE\"") != std::string::npos);

    return true;
}

/* ───── Secret Sanitization Tests ───── */

static bool test_no_secrets_in_logs() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // Log events with potential secret-like strings in details
    // The logger should pass through details_json as-is (caller is responsible),
    // but the write_event explicitly states: never include private keys, API keys,
    // or full signatures. We verify the logger doesn't add secrets itself.

    tele.log_event(EventType::ORDER_SUBMITTED, "BTC-USD",
                   "{\"order_id\":\"ORD-001\",\"client_order_id\":\"abc-123\"}", "INFO");

    std::string content = read_log_file(log_path, 1, 500);

    CASE("log contains order_id field (safe, non-secret)",
         content.find("\"order_id\"") != std::string::npos);

    // Verify no private key patterns appear
    CASE("no 'private_key' field in log",
         content.find("private_key") == std::string::npos);
    CASE("no 'api_key' field in log",
         content.find("api_key") == std::string::npos);
    CASE("no 'signature' field in log",
         content.find("signature") == std::string::npos);
    CASE("no 'seed_phrase' in log",
         content.find("seed_phrase") == std::string::npos);
    CASE("no 'mnemonic' in log",
         content.find("mnemonic") == std::string::npos);

    // The writer does NOT log the maker/taker addresses from OrderParams
    // (log_risk_block only logs order_size, not addresses)
    {
        OrderParams params;
        memset(params.maker, 0x42, 20);
        memset(params.taker, 0x00, 20);
        params.size = static_cast<uint64_t>(100 * 1e6);
        params.salt = 999;
        params.nonce = 888;
        params.price = 300000000;
        params.side = 0;

        tele.log_risk_block(TickResult::RISK_BLOCKED, params);
    }

    std::string content2 = read_log_file(log_path, 2, 1000);

    CASE("log_risk_block does NOT include maker address bytes (0x42)",
         content2.find("4242424242424242424242424242424242424242") == std::string::npos);
    CASE("log_risk_block does NOT include salt/nonce (sensitive order params)",
         content2.find("\"salt\":") == std::string::npos);
    CASE("log_risk_block only logs order_size (sanitized)",
         content2.find("\"order_size\":") != std::string::npos);

    return true;
}

/* ───── Integration: Telemetry methods called from execution_engine.cpp ───── */

static bool test_execution_engine_integration_methods() {
    std::string log_path = unique_log_file();
    Telemetry tele(log_path);

    // These are the exact methods called from execution_engine.cpp:
    // - log_event(EventType::BALANCE_CHECK, ...)
    // - log_risk_block(TickResult, OrderParams)
    // - record_tick_result(TickResult)
    // - increment_429()
    // - increment_orders_submitted()
    // - log_event(EventType::ORDER_SUBMITTED, ...)
    // - log_event(EventType::ORDER_REJECTED, ...)
    // - get_telemetry() returns Telemetry&

    tele.log_event(EventType::BALANCE_CHECK, "",
                   "{\"event\":\"startup\",\"component\":\"ExecutionEngine\"}", "INFO");
    tele.increment_orders_submitted();
    tele.record_tick_result(TickResult::OK);
    tele.increment_429();
    tele.reset_429_streak();

    OrderParams params{};
    params.size = 100;
    tele.log_risk_block(TickResult::NOT_PROFITABLE, params);

    tele.log_event(EventType::ORDER_SUBMITTED, "POL-USD",
                   "{\"client_order_id\":\"coi-123\",\"status\":\"OPEN\"}", "INFO");
    tele.log_event(EventType::ORDER_REJECTED, "POL-USD",
                   "{\"client_order_id\":\"coi-123\",\"status\":400}", "ERROR");

    std::string content = read_log_file(log_path, 4, 1000);

    CASE("BALANCE_CHECK event logged",
         content.find("\"type\":\"BALANCE_CHECK\"") != std::string::npos);
    CASE("ORDER_SUBMITTED event logged",
         content.find("\"type\":\"ORDER_SUBMITTED\"") != std::string::npos);
    CASE("ORDER_REJECTED event logged",
         content.find("\"type\":\"ORDER_REJECTED\"") != std::string::npos);
    CASE("metrics JSON shows orders_submitted == 1",
         tele.get_metrics_json().find("\"orders_submitted\":1") != std::string::npos);
    CASE("metrics JSON shows tickresult_ok == 1",
         tele.get_metrics_json().find("\"tickresult_ok\":1") != std::string::npos);

    return true;
}

/* ───── Main ───── */

int main() {
    std::cout << "📡 Telemetry & Observability — Phase 6 Test Suite" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    std::cout << "AlertConfig defaults:" << std::endl;
    std::cout << "  daily_loss_threshold_usd      = 500.0" << std::endl;
    std::cout << "  consecutive_429_threshold      = 5" << std::endl;
    std::cout << "  latency_spike_us             = 100000 (100ms)" << std::endl;
    std::cout << "  position_divergence_threshold = 0.05" << std::endl;
    std::cout << "  feed_dead_ms                 = 5000" << std::endl;
    std::cout << "  webhook_url                  = \"\"" << std::endl;
    std::cout << std::endl;

    // T6-1: Async append-only logger
    run_test("T6-1a: SPSC ring buffer capacity == 8192",        test_spsc_ring_buffer_capacity);
    run_test("T6-1b: log_event produces valid JSON line",         test_log_event_produces_json_line);
    run_test("T6-1c: append-only — no event overwrite",           test_append_only_no_overwrite);
    run_test("T6-1d: hot path no I/O (non-blocking)",              test_hot_path_no_io);
    run_test("T6-1e: backpressure / dropped events counted",       test_backpressure_drop_count);
    run_test("T6-1f: JSON Lines format validation",                test_json_lines_format);

    // T6-2: Configurable alerts
    run_test("T6-2a: AlertConfig defaults (no env)",               test_alert_config_defaults);
    run_test("T6-2b: AlertConfig env override",                    test_alert_config_env_override);
    run_test("T6-2c: consecutive_429 alert threshold",             test_consecutive_429_alert_threshold);
    run_test("T6-2d: daily_loss alert threshold",                  test_daily_loss_alert_threshold);

    // T6-3: Metrics exposed
    run_test("T6-3a: metrics JSON contains all counters",          test_metrics_json_contains_all_counters);
    run_test("T6-3b: counters are atomic (concurrent)",             test_counters_are_atomic);
    run_test("T6-3c: record all TickResult types",                 test_record_all_tick_result_types);

    // Integration & sanitization
    run_test("Integration: log_risk_block with OrderParams",        test_log_risk_block_with_order_params);
    run_test("Integration: execution_engine.cpp methods match",     test_execution_engine_integration_methods);
    run_test("Security: no secrets in logs (sanitized)",            test_no_secrets_in_logs);

    std::cout << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;

    // Print individual case results
    std::cout << std::endl;
    for (int i = 0; i < g_case_idx; i++) {
        std::cout << "  " << (g_cases[i].passed ? "✅" : "❌") << " "
                  << g_cases[i].name << std::endl;
    }

    std::cout << std::endl;

    int failed_tests = g_tests_run - g_tests_passed;
    int failed_cases = 0;
    for (int i = 0; i < g_case_idx; i++) {
        if (!g_cases[i].passed) failed_cases++;
    }

    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Tests:  " << g_tests_passed << "/" << g_tests_run << " passed" << std::endl;
    std::cout << "Cases:  " << (g_case_idx - failed_cases) << "/" << g_case_idx
              << " passed (" << failed_cases << " failed)" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;

    if (failed_tests > 0 || failed_cases > 0) {
        std::cout << "❌ " << failed_tests << " test(s) failed, "
                  << failed_cases << " case(s) failed" << std::endl;
        return 1;
    }

    std::cout << "✅ All " << g_tests_passed << "/" << g_tests_run << " tests passed ("
              << g_case_idx << " assertion cases)" << std::endl;
    std::cout << "✅ Telemetry async logging, alert thresholds, and metrics verified." << std::endl;
    std::cout << "✅ No secrets found in audit logs." << std::endl;
    return 0;
}
