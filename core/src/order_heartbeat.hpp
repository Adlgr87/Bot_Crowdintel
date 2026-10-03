#ifndef ORDER_HEARTBEAT_HPP
#define ORDER_HEARTBEAT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Order heartbeat (cancel-on-disconnect) — a component that is *separate* from
// the WebSocket PING/PONG keep-alive.
//
// Venue contract (verified 2026-10-03):
//  * POST https://clob.polymarket.com/v1/heartbeats with L2 headers and the
//    exact body {"heartbeat_id":"<id>"}; the first request sends an empty id.
//    Source: Polymarket/py-clob-client py_clob_client/client.py:713-727
//    (POST_HEARTBEAT = "/v1/heartbeats", body serialized with
//    separators=(",", ":")), Polymarket/clob-client src/endpoints.ts
//    (POST_HEARTBEAT = "/v1/heartbeats").
//  * The response returns a new id: {"heartbeat_id":"<id>"}; that id must be
//    sent on the next heartbeat.
//  * Send a heartbeat every 5 seconds.  If a valid heartbeat is not received
//    within 10 seconds, ALL open orders owned by those CLOB API credentials are
//    cancelled.  The cancellation check runs every 5 seconds, so cancellation
//    may happen up to 5 seconds after the timeout.
//    Source: https://docs.polymarket.com/trading/manage-orders ("Order
//    Heartbeats").
//  * An invalid/expired id yields HTTP 400 with the expected id:
//    {"error_msg":"Invalid Heartbeat ID","heartbeat_id":"<expected>"} → sign a
//    new request with that id and retry (resynchronise).
//  * The contract is per-credential, not per-order: enabling it means every
//    order under those credentials dies when the heartbeats stop.  Dedicated
//    credentials per heartbeat-owning process are therefore an operational
//    requirement (documented in docs/DEPLOYMENT.md).
//
// Local policy (fail-closed, all thresholds configurable and range-checked):
//   interval  5 s   – matches the documented cadence
//   warn      7 s   – observability only
//   block     9 s   – stop placing new orders *before* the venue's 10 s timeout
//   assume-cancelled 10 s – treat every resting order as cancelled by the venue
//                           and force a reconciliation before trading again
// The watchdog uses CLOCK_MONOTONIC, so a wall-clock jump can neither hide a
// stalled heartbeat nor invent one.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../include/event_ledger.hpp"
#include "clob_rest_client.hpp"

namespace heartbeat {

// Venue contract, verified 2026-10-03 against the official SDKs and docs:
//   * "if heartbeats are started and one isn't sent within 10s, all orders will
//     be cancelled" — Polymarket/py-clob-client py_clob_client/client.py:715 and
//     Polymarket/clob-client src/client.ts:1144 (both spell 10 s).
//   * "Send a heartbeat every 5 seconds"; "if a valid heartbeat is not received
//     within 10 seconds, all open orders owned by those CLOB API credentials are
//     canceled. The cancellation check runs every five seconds, so cancellation
//     may occur up to five seconds after the timeout." — docs
//     https://docs.polymarket.com/trading/manage-orders, section "Order
//     Heartbeats".  The 5 s cadence and the 5 s evaluation interval appear only
//     in the narrative documentation, not in the SDKs.
inline constexpr uint32_t K_VENUE_TIMEOUT_MS = 10000;   // 10 s: cancel threshold
inline constexpr uint32_t K_VENUE_CHECK_MS = 5000;      // 5 s: evaluation cadence
inline constexpr uint32_t K_VENUE_CADENCE_MS = 5000;    // 5 s: documented send rate
inline constexpr uint32_t K_VENUE_WORST_CASE_MS =
    K_VENUE_TIMEOUT_MS + K_VENUE_CHECK_MS;  // cancellation may lag the timeout
// Minimum slack required between our worst-case acknowledged gap and the venue
// timeout.  With the documented cadence (5 s) and a 2.5 s request cap the gap is
// 7.5 s, i.e. 2.5 s of slack.
inline constexpr uint32_t K_MIN_MARGIN_MS = 2000;

struct Config {
    bool enabled = false;
    uint32_t interval_ms = 5000;
    uint32_t warn_ms = 7000;
    uint32_t block_ms = 9000;
    uint32_t assume_cancelled_ms = 10000;
    uint32_t max_consecutive_failures = 2;
    clob::RequestOptions request_options{1000, 2500, false};

    bool validate(char* error, size_t cap) const noexcept {
        if (!enabled) return true;
        if (interval_ms < 1000 || interval_ms > 6000) {
            std::snprintf(error, cap,
                          "heartbeat interval must be within 1000..6000 ms");
            return false;
        }
        if (warn_ms <= interval_ms || warn_ms >= block_ms) {
            std::snprintf(error, cap,
                          "heartbeat thresholds must satisfy interval < warn < block");
            return false;
        }
        if (block_ms > K_VENUE_TIMEOUT_MS) {
            std::snprintf(error, cap,
                          "heartbeat block threshold must not exceed the venue "
                          "timeout (10000 ms)");
            return false;
        }
        if (assume_cancelled_ms < block_ms ||
            assume_cancelled_ms > K_VENUE_TIMEOUT_MS) {
            // The venue can cancel from `K_VENUE_TIMEOUT_MS` after the last
            // valid beat onwards, so treating orders as possibly-cancelled must
            // start no later than that instant.
            std::snprintf(error, cap,
                          "heartbeat assume-cancelled threshold must be within "
                          "block..%u ms (the venue may cancel from %u ms)",
                          K_VENUE_TIMEOUT_MS, K_VENUE_TIMEOUT_MS);
            return false;
        }
        // Worst case between two *acknowledged* beats is one interval plus one
        // request timeout (deadline scheduling, see next_wakeup_ms).  That must
        // stay at least K_MIN_MARGIN_MS below the venue's 10 s timeout, or a
        // single slow request could let the venue cancel resting orders.
        if (static_cast<uint64_t>(interval_ms) +
                static_cast<uint64_t>(request_options.total_timeout_ms) >
            K_VENUE_TIMEOUT_MS - K_MIN_MARGIN_MS) {
            std::snprintf(error, cap,
                          "heartbeat interval + request timeout must be <= %u ms "
                          "so the acknowledged gap stays >= %u ms below the venue "
                          "timeout of %u ms",
                          K_VENUE_TIMEOUT_MS - K_MIN_MARGIN_MS, K_MIN_MARGIN_MS,
                          K_VENUE_TIMEOUT_MS);
            return false;
        }
        if (max_consecutive_failures < 1 || max_consecutive_failures > 8) {
            std::snprintf(error, cap,
                          "heartbeat failure threshold must be within 1..8");
            return false;
        }
        if (request_options.total_timeout_ms >= interval_ms) {
            std::snprintf(error, cap,
                          "heartbeat request timeout must be shorter than the "
                          "interval");
            return false;
        }
        return true;
    }
};

enum class Health : uint8_t {
    DISABLED = 0,  // heartbeat contract not started: orders survive our death
    HEALTHY,
    WARN,
    BLOCKED,        // stop placing new orders
    INVALIDATED,    // venue rejected the id: orders must be assumed cancelled
    UNREACHABLE     // transport failing: orders must be assumed cancelled
};

inline const char* health_name(Health health) noexcept {
    switch (health) {
        case Health::DISABLED: return "DISABLED";
        case Health::HEALTHY: return "HEALTHY";
        case Health::WARN: return "WARN";
        case Health::BLOCKED: return "BLOCKED";
        case Health::INVALIDATED: return "INVALIDATED";
        case Health::UNREACHABLE: return "UNREACHABLE";
    }
    return "INVALID";
}

inline bool health_blocks_new_orders(Health health) noexcept {
    return health == Health::BLOCKED || health == Health::INVALIDATED ||
           health == Health::UNREACHABLE;
}

// True when resting orders must be assumed cancelled by the venue and the
// account state must be re-derived from authoritative reads.
inline bool health_implies_cancelled_orders(Health health) noexcept {
    return health == Health::INVALIDATED || health == Health::UNREACHABLE;
}

class OrderHeartbeat {
public:
    OrderHeartbeat(clob::ClobApiClient& api, ledger::EventLedger& ledger,
                   const Config& config)
        : api_(api), ledger_(ledger), config_(config) {}

    ~OrderHeartbeat() {
        request_stop();
        join();
    }

    OrderHeartbeat(const OrderHeartbeat&) = delete;
    OrderHeartbeat& operator=(const OrderHeartbeat&) = delete;

    bool enabled() const noexcept { return config_.enabled; }
    const Config& config() const noexcept { return config_; }

    // Performs one heartbeat exchange synchronously.  Exposed so tests (and the
    // paper/replay modes) can drive the state machine deterministically.
    bool tick(char* error, size_t cap) {
        if (!config_.enabled) {
            std::snprintf(error, cap, "heartbeat disabled");
            return false;
        }
        if (!api_.credentials_ready()) {
            std::snprintf(error, cap, "heartbeat requires L2 credentials");
            return false;
        }
        const uint64_t attempt_mono_ms = ledger::now_mono_ms();
        char current_id[80];
        std::snprintf(current_id, sizeof(current_id), "%s", current_id_);
        clob::HeartbeatResult result{};
        clob::CallResult call{};
        const bool transported =
            api_.post_heartbeat(current_id, result, call, config_.request_options);
        last_attempt_mono_ms_.store(attempt_mono_ms, std::memory_order_release);

        if (!transported || call.ambiguous) {
            // Ambiguity: the venue may or may not have seen the heartbeat.  The
            // conservative reading is that it did not.
            ++consecutive_failures_;
            record(0, call.ambiguous ? 3 : 0, "", attempt_mono_ms);
            std::snprintf(error, cap, "%s",
                          call.response.error[0] ? call.response.error
                                                 : "heartbeat transport failure");
            ambiguous_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (result.rejected_invalid_id) {
            // The venue told us which id it expects.  Our chain broke, so any
            // order that was resting may already have been cancelled.
            ++consecutive_failures_;
            ++resync_count_;
            invalidated_.store(true, std::memory_order_release);
            std::snprintf(current_id_, sizeof(current_id_), "%s", result.heartbeat_id);
            record(2, 0, result.heartbeat_id, attempt_mono_ms);
            std::snprintf(error, cap, "heartbeat id rejected by venue");
            return false;
        }
        if (!result.accepted) {
            ++consecutive_failures_;
            record(0, static_cast<int>(call.response.code), "", attempt_mono_ms);
            std::snprintf(error, cap, "heartbeat rejected (http %ld)",
                          call.response.code);
            return false;
        }
        consecutive_failures_ = 0;
        invalidated_.store(false, std::memory_order_release);
        if (result.heartbeat_id[0])
            std::snprintf(current_id_, sizeof(current_id_), "%s", result.heartbeat_id);
        last_accepted_mono_ms_.store(attempt_mono_ms, std::memory_order_release);
        accepted_.fetch_add(1, std::memory_order_relaxed);
        record(1, 0, result.heartbeat_id, attempt_mono_ms);
        error[0] = '\0';
        return true;
    }

    bool start(char* error, size_t cap) {
        if (!config_.enabled) {
            error[0] = '\0';
            return true;  // nothing to run; DISABLED is a valid, safe state
        }
        if (!config_.validate(error, cap)) return false;
        if (running_.load(std::memory_order_acquire)) return true;
        stop_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        // The first exchange runs inline so startup fails closed when the
        // heartbeat contract cannot be established at all.
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void request_stop() noexcept { stop_.store(true, std::memory_order_release); }

    void join() {
        if (thread_.joinable()) thread_.join();
        running_.store(false, std::memory_order_release);
    }

    // Watchdog verdict, derived from CLOCK_MONOTONIC ages only.
    Health health() const noexcept {
        if (!config_.enabled) return Health::DISABLED;
        const uint64_t now = ledger::now_mono_ms();
        const uint64_t accepted = last_accepted_mono_ms_.load(std::memory_order_acquire);
        if (invalidated_.load(std::memory_order_acquire)) return Health::INVALIDATED;
        if (accepted == 0) {
            // Never acknowledged: treat as unreachable once the first attempt is
            // older than the block threshold, or when attempts keep failing.
            const uint64_t attempt = last_attempt_mono_ms_.load(std::memory_order_acquire);
            const uint64_t reference = attempt ? attempt : now;
            if (consecutive_failures_ >= config_.max_consecutive_failures)
                return Health::UNREACHABLE;
            if (now > reference && now - reference >= config_.block_ms)
                return Health::UNREACHABLE;
            return Health::WARN;
        }
        if (consecutive_failures_ >= config_.max_consecutive_failures)
            return Health::UNREACHABLE;
        const uint64_t age = now > accepted ? now - accepted : 0;
        if (age >= config_.assume_cancelled_ms) return Health::UNREACHABLE;
        if (age >= config_.block_ms) return Health::BLOCKED;
        if (age >= config_.warn_ms) return Health::WARN;
        return Health::HEALTHY;
    }

    uint64_t age_ms() const noexcept {
        const uint64_t accepted = last_accepted_mono_ms_.load(std::memory_order_acquire);
        if (!accepted) return UINT64_MAX;
        const uint64_t now = ledger::now_mono_ms();
        return now > accepted ? now - accepted : 0;
    }

    // True once the venue has acknowledged at least one heartbeat.  From that
    // moment on, stopping this process cancels every resting order owned by these
    // credentials — the reason the shutdown path must cancel explicitly and the
    // reason dedicated credentials are mandatory.
    bool chain_active() const noexcept {
        return last_accepted_mono_ms_.load(std::memory_order_acquire) != 0;
    }
    bool invalidated() const noexcept {
        return invalidated_.load(std::memory_order_acquire);
    }
    uint64_t accepted_count() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }
    uint64_t ambiguous_failures() const noexcept {
        return ambiguous_failures_.load(std::memory_order_relaxed);
    }
    uint64_t resync_count() const noexcept { return resync_count_; }
    uint32_t consecutive_failures() const noexcept { return consecutive_failures_; }
    const char* current_id() const noexcept { return current_id_; }

private:
    void run() {
        char error[192]{};
        // Establish the chain immediately, then keep the documented cadence.
        uint64_t deadline = ledger::now_mono_ms();
        (void)tick(error, sizeof(error));
        deadline = next_wakeup_ms(deadline, ledger::now_mono_ms());
        while (!stop_.load(std::memory_order_acquire)) {
            // Deadline scheduling: a slow or failed request must not push the
            // next beat later, because the gap between *acknowledged* beats is
            // what the venue measures against its 10 s timeout.
            const uint64_t now = ledger::now_mono_ms();
            const uint64_t wait = now < deadline ? deadline - now : 0;
            if (!sleep_until(deadline)) break;
            (void)tick(error, sizeof(error));
            deadline = next_wakeup_ms(deadline, ledger::now_mono_ms());
            (void)wait;
        }
        running_.store(false, std::memory_order_release);
    }

    // Sleeps in short slices so request_stop() is honoured within ~50 ms.
    bool sleep_until(uint64_t deadline_mono_ms) const {
        for (;;) {
            if (stop_.load(std::memory_order_acquire)) return false;
            const uint64_t now = ledger::now_mono_ms();
            if (now >= deadline_mono_ms) return true;
            const uint64_t remaining = deadline_mono_ms - now;
            const uint64_t slice = remaining < 50 ? remaining : 50;
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        }
    }

public:
    // Pure scheduler, exposed for tests: the next beat is one interval after the
    // *previous deadline* (never after the response), and a deadline already in
    // the past fires immediately instead of drifting further behind.
    static uint64_t next_wakeup_ms(uint64_t previous_deadline_ms, uint64_t now_ms,
                                   uint32_t interval_ms) noexcept {
        const uint64_t candidate = previous_deadline_ms + interval_ms;
        if (candidate <= now_ms) {
            // We are behind: re-anchor on now so beats do not bunch up, but never
            // schedule in the past.
            return now_ms + interval_ms / 4 + 1;
        }
        return candidate;
    }

    uint64_t next_wakeup_ms(uint64_t previous_deadline_ms, uint64_t now_ms) const
        noexcept {
        return next_wakeup_ms(previous_deadline_ms, now_ms, config_.interval_ms);
    }

private:

    // result: 1 accepted, 2 rejected (invalid id), 3 ambiguous, 0 failure.
    void record(uint8_t result, int http_code, const char* new_id,
                uint64_t attempt_mono_ms) {
        ledger::Event event{};
        event.type = ledger::EventType::HEARTBEAT;
        event.source = ledger::Source::REST;
        event.wall_ns = ledger::now_wall_ns();
        event.venue_ts_ms = attempt_mono_ms;
        event.add_u8(ledger::F_RESULT, result);
        if (http_code != 0)
            event.add_u64(ledger::F_HTTP_CODE, static_cast<uint64_t>(http_code));
        if (new_id && new_id[0]) event.add_str(ledger::F_HEARTBEAT_ID, new_id);
        else if (current_id_[0]) event.add_str(ledger::F_HEARTBEAT_ID, current_id_);
        // One event per attempt: the attempt timestamp is part of the identity.
        ledger::compute_event_key(event.type, event.source, "", "", attempt_mono_ms,
                                  result, event.key);
        char error[128]{};
        auto lock = ledger_.guard();
        if (!ledger_.commit_locked(event, error, sizeof(error)))
            ledger_write_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    clob::ClobApiClient& api_;
    ledger::EventLedger& ledger_;
    Config config_{};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> invalidated_{false};
    std::atomic<uint64_t> last_accepted_mono_ms_{0};
    std::atomic<uint64_t> last_attempt_mono_ms_{0};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> ambiguous_failures_{0};
    std::atomic<uint64_t> ledger_write_failures_{0};
    uint32_t consecutive_failures_ = 0;
    uint64_t resync_count_ = 0;
    char current_id_[80]{};
};

}  // namespace heartbeat

#endif  // ORDER_HEARTBEAT_HPP
