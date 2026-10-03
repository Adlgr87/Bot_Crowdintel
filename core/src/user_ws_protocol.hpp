#ifndef USER_WS_PROTOCOL_HPP
#define USER_WS_PROTOCOL_HPP

// ─────────────────────────────────────────────────────────────────────────────
// CLOB *user* channel protocol layer — configuration, subscription frames and
// frame processing.  No sockets and no OpenSSL in this file, so every rule the
// live client depends on is unit-testable offline.
//
// Endpoint and frames (verified 2026-10-03):
//   wss://ws-subscriptions-clob.polymarket.com/ws/user
//     * Polymarket/py-sdk src/polymarket/environments.py (clob_user_ws_url)
//     * https://docs.polymarket.com/getting-started/api ("CLOB User Channel")
//   subscription frame:
//     {"auth":{"apiKey":"…","secret":"…","passphrase":"…"},
//      "markets":["<condition_id>"],"type":"user"}
//     `markets` may be omitted to follow the whole account; in-flight changes
//     use {"operation":"subscribe"|"unsubscribe","markets":[…]}.
//     * https://docs.polymarket.com/trading/realtime-order-updates
//     * Polymarket/py-sdk streams/clob/user_protocol.py:35-56 (build_initial_frame)
//   The frame carries the L2 secret, so it is built into a bounded buffer that
//   the caller wipes immediately after the write and never logs.
//
// Trust model implemented by FrameProcessor:
//   * at-least-once delivery → duplicates are absorbed by the ledger's
//     idempotency keys and the applier's semantic checks;
//   * the stream never replays what was missed while disconnected, so every
//     (re)connect, parse failure, error frame or divergence marks the account
//     view STALE until a REST reconciliation clears it;
//   * a ledger write failure is fatal for the session: the process must not
//     keep trading without a durable record of what happened.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/event_ledger.hpp"
#include "../include/json_scan.hpp"
#include "clob_rest_client.hpp"
#include "user_event.hpp"

namespace user_ws {

inline constexpr size_t K_MAX_MARKETS = 4;
inline constexpr size_t K_MAX_SUBSCRIPTION_FRAME = 1024;

struct Config {
    bool enabled = false;
    char url[256] = "wss://ws-subscriptions-clob.polymarket.com/ws/user";
    char markets[K_MAX_MARKETS][70]{};  // condition ids; empty = whole account
    size_t market_count = 0;
    uint32_t reconnect_min_ms = 250;
    uint32_t reconnect_max_ms = 5000;
    uint32_t connect_timeout_ms = 3000;
    uint32_t keepalive_interval_ms = 8000;   // text PING cadence
    uint32_t idle_timeout_ms = 20000;        // no bytes at all
    uint32_t pong_timeout_ms = 30000;        // no PONG
    bool require_tls = true;                 // wss:// unless loopback test venue
    // Test-only escape hatch; production code never sets it and
    // MarketConfig does not expose it.
    bool allow_loopback_plaintext = false;
    char pin_spki_base64[65]{};  // base64 body of a "sha256//<b64>" pin

    bool validate(char* error, size_t cap) const noexcept {
        if (!enabled) return true;
        if (std::strncmp(url, "wss://", 6) != 0) {
            // Plaintext ws:// is tolerated for one purpose only: the in-process
            // loopback test venue (tests/support/local_venue.hpp).  Any non
            // loopback host must use wss://, so a misconfigured production URL
            // can never downgrade silently.
            const bool loopback = std::strncmp(url, "ws://127.0.0.1", 14) == 0 ||
                                  std::strncmp(url, "ws://localhost", 14) == 0 ||
                                  std::strncmp(url, "ws://[::1]", 10) == 0;
            if (!loopback || !allow_loopback_plaintext) {
                std::snprintf(error, cap,
                              "user WSS url must use the wss:// scheme (plaintext "
                              "ws:// is only accepted for loopback when "
                              "allow_loopback_plaintext is set)");
                return false;
            }
        }
        if (reconnect_min_ms == 0 || reconnect_min_ms > reconnect_max_ms) {
            std::snprintf(error, cap, "user WSS reconnect backoff range is invalid");
            return false;
        }
        if (reconnect_max_ms > 60000) {
            std::snprintf(error, cap, "user WSS reconnect backoff ceiling is too high");
            return false;
        }
        if (keepalive_interval_ms < 1000 || keepalive_interval_ms > 10000) {
            std::snprintf(error, cap,
                          "user WSS keepalive must be within 1000..10000 ms");
            return false;
        }
        if (idle_timeout_ms <= keepalive_interval_ms ||
            pong_timeout_ms <= idle_timeout_ms) {
            std::snprintf(error, cap,
                          "user WSS timeouts must satisfy keepalive < idle < pong");
            return false;
        }
        if (market_count > K_MAX_MARKETS) {
            std::snprintf(error, cap, "user WSS market filter exceeds its bound");
            return false;
        }
        for (size_t i = 0; i < market_count; ++i) {
            if (!json_scan::is_hex_bytes(markets[i], std::strlen(markets[i]), 32)) {
                std::snprintf(error, cap,
                              "user WSS market filter is not a condition id");
                return false;
            }
        }
        return true;
    }
};

enum class StreamState : uint8_t {
    DISABLED = 0,
    STOPPED,
    CONNECTING,
    SUBSCRIBED,
    BACKOFF,
    FAILED
};

inline const char* stream_state_name(StreamState state) noexcept {
    switch (state) {
        case StreamState::DISABLED: return "DISABLED";
        case StreamState::STOPPED: return "STOPPED";
        case StreamState::CONNECTING: return "CONNECTING";
        case StreamState::SUBSCRIBED: return "SUBSCRIBED";
        case StreamState::BACKOFF: return "BACKOFF";
        case StreamState::FAILED: return "FAILED";
    }
    return "INVALID";
}

// ── Frame construction (pure) ───────────────────────────────────────────────
inline bool build_subscription_frame(const Config& config,
                                     const clob::Credentials& credentials, char* out,
                                     size_t cap, size_t& out_len) noexcept {
    if (!out || cap == 0) return false;
    if (!credentials.complete()) return false;
    size_t written = 0;
    auto append = [&](const char* text) {
        const size_t length = std::strlen(text);
        if (written + length >= cap) return false;
        std::memcpy(out + written, text, length);
        written += length;
        out[written] = '\0';
        return true;
    };
    if (!append("{\"auth\":{\"apiKey\":\"")) return false;
    if (!append(credentials.api_key)) return false;
    if (!append("\",\"secret\":\"")) return false;
    if (!append(credentials.api_secret_b64)) return false;
    if (!append("\",\"passphrase\":\"")) return false;
    if (!append(credentials.api_passphrase)) return false;
    if (!append("\"}")) return false;
    if (config.market_count) {
        if (!append(",\"markets\":[")) return false;
        for (size_t i = 0; i < config.market_count && i < K_MAX_MARKETS; ++i) {
            if (i && !append(",")) return false;
            if (!append("\"")) return false;
            if (!append(config.markets[i])) return false;
            if (!append("\"")) return false;
        }
        if (!append("]")) return false;
    }
    if (!append(",\"type\":\"user\"}")) return false;
    out_len = written;
    return true;
}

inline bool build_operation_frame(const char* operation, const char* condition_id,
                                  char* out, size_t cap) noexcept {
    if (!operation || !condition_id || !out || cap == 0) return false;
    if (std::strcmp(operation, "subscribe") != 0 &&
        std::strcmp(operation, "unsubscribe") != 0)
        return false;
    if (!json_scan::is_hex_bytes(condition_id, std::strlen(condition_id), 32))
        return false;
    const int written = std::snprintf(out, cap,
                                      "{\"operation\":\"%s\",\"markets\":[\"%s\"]}",
                                      operation, condition_id);
    return written > 0 && static_cast<size_t>(written) < cap;
}

// ── Frame processing ────────────────────────────────────────────────────────
class FrameProcessor {
public:
    FrameProcessor(ledger::EventLedger& ledger, const ApplierContext& context,
                   bool start_stale = true)
        : ledger_(ledger), applier_(ledger), context_(context) {
        stale_.store(start_stale, std::memory_order_release);
    }

    // Returns false when the session must stop (ledger write failure or an
    // explicit server error frame).
    bool handle_frame(const char* payload, size_t length) {
        frames_.fetch_add(1, std::memory_order_relaxed);
        UserMessage message{};
        if (!parse_user_message(payload, length, message)) {
            parse_failures_.fetch_add(1, std::memory_order_relaxed);
            // An unparseable frame means the local view may be incomplete.
            stale_.store(true, std::memory_order_release);
            record_state_event("user_ws_parse_failure");
            return true;
        }
        switch (message.kind) {
            case MessageKind::HEARTBEAT_ACK:
                keepalive_acks_.fetch_add(1, std::memory_order_relaxed);
                return true;
            case MessageKind::ERROR_FRAME:
                stale_.store(true, std::memory_order_release);
                record_state_event("user_ws_error_frame");
                return false;  // the server rejected this session
            case MessageKind::UNRECOGNIZED:
            case MessageKind::NONE:
                parse_failures_.fetch_add(1, std::memory_order_relaxed);
                stale_.store(true, std::memory_order_release);
                return true;
            case MessageKind::ORDER:
            case MessageKind::TRADE:
                break;
        }
        for (size_t i = 0; i < message.order_count; ++i) {
            ApplyOutcome outcome{};
            if (!applier_.apply_order(message.orders[i], context_, outcome)) {
                apply_failures_.fetch_add(1, std::memory_order_relaxed);
                stale_.store(true, std::memory_order_release);
                record_state_event("user_ws_ledger_failure");
                return false;
            }
            account(outcome);
        }
        for (size_t i = 0; i < message.trade_count; ++i) {
            ApplyOutcome outcome{};
            if (!applier_.apply_trade(message.trades[i], context_, outcome)) {
                apply_failures_.fetch_add(1, std::memory_order_relaxed);
                stale_.store(true, std::memory_order_release);
                record_state_event("user_ws_ledger_failure");
                return false;
            }
            account(outcome);
        }
        last_event_mono_ms_.store(ledger::now_mono_ms(), std::memory_order_release);
        return true;
    }

    // Called by the transport on every (re)connect and on every failure.
    void mark_stale() noexcept { stale_.store(true, std::memory_order_release); }
    // Called only after a successful REST reconciliation.
    void clear_stale() noexcept { stale_.store(false, std::memory_order_release); }
    bool stale() const noexcept { return stale_.load(std::memory_order_acquire); }

    uint64_t frames() const noexcept { return frames_.load(std::memory_order_relaxed); }
    uint64_t events_applied() const noexcept {
        return events_applied_.load(std::memory_order_relaxed);
    }
    uint64_t duplicates() const noexcept {
        return duplicates_.load(std::memory_order_relaxed);
    }
    uint64_t parse_failures() const noexcept {
        return parse_failures_.load(std::memory_order_relaxed);
    }
    uint64_t apply_failures() const noexcept {
        return apply_failures_.load(std::memory_order_relaxed);
    }
    uint64_t divergences() const noexcept {
        return divergences_.load(std::memory_order_relaxed);
    }
    uint64_t confirmations_required() const noexcept {
        return needs_confirmation_.load(std::memory_order_relaxed);
    }
    uint64_t keepalive_acks() const noexcept {
        return keepalive_acks_.load(std::memory_order_relaxed);
    }
    uint64_t last_event_mono_ms() const noexcept {
        return last_event_mono_ms_.load(std::memory_order_acquire);
    }

private:
    void account(const ApplyOutcome& outcome) {
        if (outcome.duplicate) {
            duplicates_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (outcome.applied) events_applied_.fetch_add(1, std::memory_order_relaxed);
        if (outcome.divergence || outcome.adopted_external ||
            outcome.illegal_transition) {
            divergences_.fetch_add(1, std::memory_order_relaxed);
            stale_.store(true, std::memory_order_release);
            if (outcome.reason[0]) record_state_event(outcome.reason);
        }
        if (outcome.needs_rest_confirmation)
            needs_confirmation_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_state_event(const char* reason) {
        ledger::Event event{};
        event.type = ledger::EventType::STATE_EVENT;
        event.source = ledger::Source::USER_WS;
        event.wall_ns = ledger::now_wall_ns();
        event.add_str(ledger::F_REASON, reason);
        ledger::compute_event_key(event.type, event.source, "", "", 0,
                                  ledger::fnv1a(reason, std::strlen(reason)),
                                  event.key);
        char error[128]{};
        (void)ledger_.commit(event, error, sizeof(error));
    }

    ledger::EventLedger& ledger_;
    UserEventApplier applier_;
    ApplierContext context_{};
    std::atomic<bool> stale_{true};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> events_applied_{0};
    std::atomic<uint64_t> duplicates_{0};
    std::atomic<uint64_t> parse_failures_{0};
    std::atomic<uint64_t> apply_failures_{0};
    std::atomic<uint64_t> divergences_{0};
    std::atomic<uint64_t> needs_confirmation_{0};
    std::atomic<uint64_t> keepalive_acks_{0};
    std::atomic<uint64_t> last_event_mono_ms_{0};
};

}  // namespace user_ws

#endif  // USER_WS_PROTOCOL_HPP
