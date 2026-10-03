#ifndef USER_WS_CLIENT_HPP
#define USER_WS_CLIENT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// CLOB *user* channel transport: one thread, one socket, internal reconnects.
//
// All protocol rules live in user_ws_protocol.hpp (offline-testable) and all
// socket/TLS mechanics in ws_session.hpp.  This file only wires them together:
//
//   connect (TLS + RFC 6455 handshake)
//     → send the subscription frame (carries the L2 secret; wiped after write)
//     → read frames → FrameProcessor::handle_frame → ledger
//     → on any failure: close, mark the account view STALE, exponential backoff,
//       reconnect in the SAME thread (never a second listener)
//
// The account view is STALE from construction until a REST reconciliation
// clears it, and it becomes STALE again on every connect, reconnect, parse
// failure, error frame and divergence.  Live trading must be disabled while
// `stale()` is true — this is the mechanism that makes "reconnect ≠ resume
// trading" explicit.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../crypto/secure_zero.hpp"
#include "../include/event_ledger.hpp"
#include "clob_rest_client.hpp"
#include "user_event.hpp"
#include "user_ws_protocol.hpp"
#include "ws_session.hpp"

namespace user_ws {

class UserWsClient {
public:
    UserWsClient(const Config& config, const clob::Credentials& credentials,
                 ledger::EventLedger& ledger, const ApplierContext& context)
        : config_(config), credentials_(credentials), processor_(ledger, context) {
        state_.store(config_.enabled ? StreamState::STOPPED : StreamState::DISABLED,
                     std::memory_order_release);
    }

    ~UserWsClient() {
        stop();
        join();
        secure_zero(&credentials_, sizeof(credentials_));
    }

    UserWsClient(const UserWsClient&) = delete;
    UserWsClient& operator=(const UserWsClient&) = delete;

    bool start(char* error, size_t cap) {
        if (!config_.enabled) {
            if (error && cap) error[0] = '\0';
            return true;
        }
        if (!config_.validate(error, cap)) return false;
        if (!credentials_.complete()) {
            std::snprintf(error, cap, "user WSS requires L2 credentials");
            return false;
        }
        if (running_.load(std::memory_order_acquire)) return true;
        stop_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() noexcept {
        stop_.store(true, std::memory_order_release);
        session_.request_stop();
    }

    void join() {
        if (thread_.joinable()) thread_.join();
        running_.store(false, std::memory_order_release);
    }

    StreamState state() const noexcept { return state_.load(std::memory_order_acquire); }
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    bool stale() const noexcept { return processor_.stale(); }
    void clear_stale() noexcept { processor_.clear_stale(); }
    void mark_stale() noexcept { processor_.mark_stale(); }
    const FrameProcessor& processor() const noexcept { return processor_; }
    ws::SessionError last_error() const noexcept { return last_error_; }
    uint64_t connect_failures() const noexcept {
        return connect_failures_.load(std::memory_order_relaxed);
    }
    uint64_t session_failures() const noexcept {
        return session_failures_.load(std::memory_order_relaxed);
    }

private:
    struct CallbackContext {
        UserWsClient* self;
    };

    static bool on_message(void* context, const char* payload, size_t length) {
        auto* callback = static_cast<CallbackContext*>(context);
        return callback->self->processor_.handle_frame(payload, length);
    }

    void run() {
        uint32_t backoff_ms = config_.reconnect_min_ms;
        CallbackContext context{this};
        while (!stop_.load(std::memory_order_acquire)) {
            state_.store(StreamState::CONNECTING, std::memory_order_release);
            ws::SessionConfig session_config{};
            std::snprintf(session_config.url, sizeof(session_config.url), "%s",
                          config_.url);
            session_config.connect_timeout_ms = config_.connect_timeout_ms;
            session_config.keepalive_interval_ms = config_.keepalive_interval_ms;
            session_config.idle_timeout_ms = config_.idle_timeout_ms;
            session_config.pong_timeout_ms = config_.pong_timeout_ms;
            session_config.require_tls = config_.require_tls;
            std::snprintf(session_config.pin_spki_base64,
                          sizeof(session_config.pin_spki_base64), "%s",
                          config_.pin_spki_base64);

            char frame[K_MAX_SUBSCRIPTION_FRAME];
            size_t frame_len = 0;
            if (!build_subscription_frame(config_, credentials_, frame, sizeof(frame),
                                          frame_len)) {
                state_.store(StreamState::FAILED, std::memory_order_release);
                secure_zero(frame, sizeof(frame));
                break;
            }
            ws::SessionError error = ws::SessionError::NONE;
            const bool connected =
                session_.connect(session_config, frame, frame_len, error);
            // The secret never outlives the write.
            secure_zero(frame, sizeof(frame));
            if (!connected) {
                last_error_ = error;
                connect_failures_.fetch_add(1, std::memory_order_relaxed);
                processor_.mark_stale();
                session_.disconnect();
                state_.store(StreamState::BACKOFF, std::memory_order_release);
                if (!sleep_backoff(backoff_ms)) break;
                backoff_ms = next_backoff(backoff_ms);
                continue;
            }
            backoff_ms = config_.reconnect_min_ms;
            state_.store(StreamState::SUBSCRIBED, std::memory_order_release);
            // A fresh socket proves nothing about what happened while we were
            // away: reconciliation must clear the stale flag.
            processor_.mark_stale();
            const bool clean = session_.run(&on_message, &context, error);
            last_error_ = error;
            session_.disconnect();
            processor_.mark_stale();
            if (!clean) session_failures_.fetch_add(1, std::memory_order_relaxed);
            if (stop_.load(std::memory_order_acquire)) break;
            state_.store(StreamState::BACKOFF, std::memory_order_release);
            if (!sleep_backoff(backoff_ms)) break;
            backoff_ms = next_backoff(backoff_ms);
        }
        state_.store(stop_.load(std::memory_order_acquire) ? StreamState::STOPPED
                                                           : StreamState::FAILED,
                     std::memory_order_release);
        running_.store(false, std::memory_order_release);
    }

    uint32_t next_backoff(uint32_t current) const noexcept {
        const uint64_t doubled = static_cast<uint64_t>(current) * 2;
        return doubled > config_.reconnect_max_ms ? config_.reconnect_max_ms
                                                  : static_cast<uint32_t>(doubled);
    }

    bool sleep_backoff(uint32_t milliseconds) const {
        for (uint32_t slept = 0; slept < milliseconds; slept += 25) {
            if (stop_.load(std::memory_order_acquire)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return !stop_.load(std::memory_order_acquire);
    }

    Config config_{};
    clob::Credentials credentials_{};
    FrameProcessor processor_;
    ws::Session session_{};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<StreamState> state_{StreamState::STOPPED};
    std::atomic<uint64_t> connect_failures_{0};
    std::atomic<uint64_t> session_failures_{0};
    ws::SessionError last_error_ = ws::SessionError::NONE;
};

}  // namespace user_ws

#endif  // USER_WS_CLIENT_HPP
