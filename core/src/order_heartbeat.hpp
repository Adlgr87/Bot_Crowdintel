#ifndef ORDER_HEARTBEAT_HPP
#define ORDER_HEARTBEAT_HPP

// Phase 4 — order heartbeat (dead-man's switch for resting orders).
//
// Documented contract (docs.polymarket.com, "Manage Orders → Order
// Heartbeats"):
//   * POST /v1/heartbeats, authenticated with the same L2 credentials that own
//     the orders, body {"heartbeat_id":""} for the first call;
//   * every successful response returns a new heartbeat_id that the next call
//     must send back; send every 5 seconds;
//   * if a valid heartbeat is not received within 10 seconds, the venue
//     cancels every open order of those credentials; the venue's cancellation
//     sweep runs every 5 seconds, so cancellation happens within 0–5 s after
//     the timeout;
//   * an expired/invalid id is answered with 400 and the expected id, which
//     must be adopted and retried.
//
// Thresholds used here: 5 s send interval, 7 s degraded (one send overdue),
// 10 s critical (the venue will now cancel the account's open orders).
//
// Fail-closed consequences implemented by the caller:
//   * critical → trading stops and every non-terminal journaled order becomes
//     UNKNOWN until reconciliation proves the venue state again;
//   * recovery alone does not resume trading.
//
// The state logic is pure and clock-injected, so it is unit tested without
// sleeping. The threaded service wraps it with the real HTTP transport.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include "../include/json_field.hpp"
#include "l2_auth.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK)
#include <curl/curl.h>
#endif

namespace heartbeat {

enum class State : uint8_t {
    kIdle = 0,      // not started
    kHealthy = 1,   // last acknowledgement within the send interval
    kDegraded = 2,  // a heartbeat is overdue (>= degraded_ms)
    kCritical = 3,  // >= critical_ms: the venue cancels open orders
};

inline const char* state_name(State state) noexcept {
    switch (state) {
        case State::kIdle: return "IDLE";
        case State::kHealthy: return "HEALTHY";
        case State::kDegraded: return "DEGRADED";
        case State::kCritical: return "CRITICAL";
    }
    return "INVALID";
}

struct Thresholds {
    uint64_t interval_ms = 5000;   // documented send cadence
    uint64_t degraded_ms = 7000;   // one cadence missed
    uint64_t critical_ms = 10000;  // documented cancellation timeout
    uint64_t retry_ms = 1000;      // retry delay after a failed send
};

// Interval between sends given the outcome of the last attempt.
inline uint64_t next_delay_ms(const Thresholds& thresholds, bool last_ok) noexcept {
    return last_ok ? thresholds.interval_ms : thresholds.retry_ms;
}

// Pure state evaluation. `last_ack_ms == 0` means "never acknowledged", in
// which case the clock is measured from `started_ms` (0 = not started).
inline State evaluate(uint64_t now_ms, uint64_t started_ms, uint64_t last_ack_ms,
                      const Thresholds& thresholds) noexcept {
    if (started_ms == 0) return State::kIdle;
    const uint64_t reference = last_ack_ms != 0 ? last_ack_ms : started_ms;
    if (now_ms < reference) return State::kHealthy;  // clock went backwards
    const uint64_t age = now_ms - reference;
    if (age >= thresholds.critical_ms) return State::kCritical;
    if (age >= thresholds.degraded_ms) return State::kDegraded;
    return State::kHealthy;
}

// {"heartbeat_id":"<id>"} — the id is echo-able text; anything that would need
// JSON escaping would corrupt the signed body, so it is refused.
inline bool build_body(const char* heartbeat_id, char* out, size_t out_cap,
                       size_t& out_len) noexcept {
    out_len = 0;
    if (!out || out_cap == 0) return false;
    const char* id = heartbeat_id ? heartbeat_id : "";
    for (const char* p = id; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
    }
    const int written =
        std::snprintf(out, out_cap, "{\"heartbeat_id\":\"%s\"}", id);
    if (written <= 0 || static_cast<size_t>(written) >= out_cap) return false;
    out_len = static_cast<size_t>(written);
    return true;
}

// {"heartbeat_id":"<new id>"} on success.
inline bool parse_ack(const char* json, size_t length, char* id_out,
                      size_t id_cap) noexcept {
    if (id_out && id_cap) id_out[0] = '\0';
    if (!json || !id_out || id_cap == 0) return false;
    uint64_t boolean = 0;
    (void)boolean;
    char status[24]{};
    if (json_field::string(json, length, "heartbeat_id", id_out, id_cap))
        return id_out[0] != '\0';
    // The OpenAPI schema for the unversioned route documents {"status":"ok"}
    // without an id; accept it only as an acknowledgement, never as a new id.
    if (json_field::string(json, length, "status", status, sizeof(status)) &&
        std::strcmp(status, "ok") == 0)
        return false;
    return false;
}

// 400 response: {"error_msg":"Invalid Heartbeat ID","heartbeat_id":"<expected>"}
inline bool parse_invalid_id_error(const char* json, size_t length,
                                   char* expected_out,
                                   size_t expected_cap) noexcept {
    if (expected_out && expected_cap) expected_out[0] = '\0';
    if (!json || !expected_out || expected_cap == 0) return false;
    char message[64]{};
    if (!json_field::string(json, length, "error_msg", message, sizeof(message)))
        return false;
    if (std::strcmp(message, "Invalid Heartbeat ID") != 0) return false;
    return json_field::string(json, length, "heartbeat_id", expected_out,
                              expected_cap) &&
           expected_out[0] != '\0';
}

}  // namespace heartbeat

#if defined(CROWDINTEL_HAVE_NETWORK)

// HTTP transport for POST /v1/heartbeats. Owns its curl handle and the decoded
// API secret; never logs either.
class HeartbeatTransport {
public:
    HeartbeatTransport(const char* clob_host, const l2auth::Credentials& credentials)
        : credentials_(credentials) {
        static std::once_flag curl_once;
        std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        std::snprintf(path_, sizeof(path_), "/v1/heartbeats");
        std::snprintf(url_, sizeof(url_), "%s/v1/heartbeats",
                      clob_host ? clob_host : "");
        secret_len_ = 0;
        if (!l2auth::decode_secret(credentials.secret_b64, secret_, sizeof(secret_),
                                   secret_len_))
            secret_len_ = 0;
        curl_ = curl_easy_init();
    }

    ~HeartbeatTransport() {
        if (curl_) curl_easy_cleanup(curl_);
        secure_zero(secret_, sizeof(secret_));
        secret_len_ = 0;
    }

    HeartbeatTransport(const HeartbeatTransport&) = delete;
    HeartbeatTransport& operator=(const HeartbeatTransport&) = delete;

    bool usable() const noexcept { return curl_ != nullptr && secret_len_ > 0; }

    // POSTs `body` and copies the response into `response`. Returns false only
    // when no HTTP response was obtained (transport failure), which the state
    // machine treats as a missed heartbeat.
    bool post(const char* body, size_t body_len, long& http_code, char* response,
              size_t response_cap, size_t& response_len) noexcept {
        http_code = 0;
        response_len = 0;
        if (!usable()) return false;
        if (body_len == 0 || body_len > 256) return false;
        if (response && response_cap) response[0] = '\0';

        char timestamp[24];
        u64_to_dec(static_cast<uint64_t>(::time(nullptr)), timestamp);
        l2auth::Headers headers_text;
        if (!l2auth::build_headers(credentials_, secret_, secret_len_, timestamp,
                                   "POST", path_, body, body_len, headers_text))
            return false;

        curl_slist* headers = nullptr;
        bool headers_ok = append_header(headers, headers_text.address);
        headers_ok = append_header(headers, headers_text.signature) && headers_ok;
        headers_ok = append_header(headers, headers_text.timestamp) && headers_ok;
        headers_ok = append_header(headers, headers_text.api_key) && headers_ok;
        headers_ok = append_header(headers, headers_text.passphrase) && headers_ok;
        headers_ok = append_header(headers, "Content-Type: application/json") &&
                     headers_ok;
        if (!headers_ok) {
            curl_slist_free_all(headers);
            secure_zero(&headers_text, sizeof(headers_text));
            return false;
        }

        response_[0] = '\0';
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_URL, url_);
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_len));
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 3000L);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "identity");
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
        const CURLcode code = curl_easy_perform(curl_);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(headers);
        secure_zero(&headers_text, sizeof(headers_text));
        if (code != CURLE_OK) return false;

        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        response_len = response_length_;
        if (response && response_cap) {
            const size_t copied =
                response_len < response_cap - 1 ? response_len : response_cap - 1;
            std::memcpy(response, response_, copied);
            response[copied] = '\0';
        }
        return true;
    }

private:
    static bool append_header(curl_slist*& list, const char* header) noexcept {
        curl_slist* next = curl_slist_append(list, header);
        if (!next) return false;
        list = next;
        return true;
    }

    static size_t write_cb(char* data, size_t size, size_t nmemb,
                           void* userdata) noexcept {
        auto* self = static_cast<HeartbeatTransport*>(userdata);
        const size_t total = size * nmemb;
        if (self->response_length_ + total > sizeof(self->response_))
            return 0;  // response too large: abort the transfer
        std::memcpy(self->response_ + self->response_length_, data, total);
        self->response_length_ += total;
        return total;
    }

    l2auth::Credentials credentials_{};
    uint8_t secret_[l2auth::kSecretMaxBytes]{};
    size_t secret_len_ = 0;
    CURL* curl_ = nullptr;
    char url_[192]{};
    char path_[24]{};
    char response_[512]{};
    size_t response_length_ = 0;
};

#endif  // CROWDINTEL_HAVE_NETWORK

// Threaded heartbeat service. The transport is injected, so tests drive it with
// a scripted sender and a synthetic clock through step().
template <typename Transport>
class HeartbeatService {
public:
    using CriticalHook = void (*)(void* context);

    HeartbeatService(Transport& transport, const heartbeat::Thresholds& thresholds,
                     uint64_t (*clock_ms)() noexcept)
        : transport_(transport), thresholds_(thresholds), clock_ms_(clock_ms) {}

    ~HeartbeatService() { stop(); }
    HeartbeatService(const HeartbeatService&) = delete;
    HeartbeatService& operator=(const HeartbeatService&) = delete;

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        started_ms_.store(clock_ms_(), std::memory_order_release);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    // The health clock starts at the first attempt, whether the service runs as
    // a thread or is driven step by step (tests, preflight).
    void ensure_started(uint64_t now_ms) {
        if (started_ms_.load(std::memory_order_relaxed) == 0)
            started_ms_.store(now_ms, std::memory_order_release);
    }

    // Performs at most one send when due. Returns true when a request was sent.
    bool step(uint64_t now_ms) {
        ensure_started(now_ms);
        const uint64_t last_attempt = last_attempt_ms_.load(std::memory_order_relaxed);
        if (last_attempt != 0 &&
            now_ms < last_attempt + next_delay_ms(thresholds_, last_ok_.load(std::memory_order_relaxed))) {
            update_state(now_ms);
            return false;
        }
        send(now_ms);
        update_state(now_ms);
        return true;
    }

    heartbeat::State state() const {
        return state_.load(std::memory_order_relaxed);
    }

    bool acknowledged() const {
        return last_ack_ms_.load(std::memory_order_relaxed) != 0;
    }

    // Consumes the "critical episode started" flag: the caller acts on it once
    // (pause trading, mark orders unproven) from its own thread.
    bool take_critical_event() {
        return critical_event_.exchange(false, std::memory_order_acq_rel);
    }

    uint64_t sends() const { return sends_.load(std::memory_order_relaxed); }
    uint64_t acks() const { return acks_.load(std::memory_order_relaxed); }
    uint64_t failures() const { return failures_.load(std::memory_order_relaxed); }
    uint64_t resyncs() const { return resyncs_.load(std::memory_order_relaxed); }
    uint64_t timeouts() const { return timeouts_.load(std::memory_order_relaxed); }
    uint64_t interval_ms() const { return thresholds_.interval_ms; }

    // The chain id and the last error are written by the service thread, so
    // callers read copies under the same lock instead of a live pointer.
    void chain_id_copy(char* out, size_t cap) const {
        std::lock_guard<std::mutex> lock(text_mutex_);
        copy_text(out, cap, chain_id_);
    }
    void last_error_copy(char* out, size_t cap) const {
        std::lock_guard<std::mutex> lock(text_mutex_);
        copy_text(out, cap, last_error_);
    }

    // Used by tests and by the startup gate: performs one send synchronously.
    bool send_now(uint64_t now_ms) {
        ensure_started(now_ms);
        send(now_ms);
        update_state(now_ms);
        return last_ok_.load(std::memory_order_relaxed);
    }

private:
    void run() {
        while (running_.load(std::memory_order_acquire)) {
            const uint64_t now = clock_ms_();
            step(now);
            const uint64_t sleep_ms =
                last_ok_.load(std::memory_order_relaxed) ? thresholds_.interval_ms / 5
                                                      : thresholds_.retry_ms / 2;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(sleep_ms == 0 ? 1 : sleep_ms));
        }
    }

    void send(uint64_t now_ms) {
        char local_chain[64];
        {
            std::lock_guard<std::mutex> lock(text_mutex_);
            std::snprintf(local_chain, sizeof(local_chain), "%s", chain_id_);
        }
        char body[128];
        size_t body_len = 0;
        if (!heartbeat::build_body(local_chain, body, sizeof(body), body_len)) {
            set_error("heartbeat body could not be built");
            ++failures_;
            last_ok_.store(false, std::memory_order_relaxed);
            return;
        }
        long http_code = 0;
        char response[512];
        size_t response_len = 0;
        ++sends_;
        last_attempt_ms_.store(now_ms, std::memory_order_relaxed);
        if (!transport_.post(body, body_len, http_code, response, sizeof(response),
                             response_len)) {
            set_error("heartbeat request failed (no HTTP response)");
            ++failures_;
            last_ok_.store(false, std::memory_order_relaxed);
            return;
        }
        if (http_code == 200) {
            char next_id[64]{};
            if (!heartbeat::parse_ack(response, response_len, next_id,
                                      sizeof(next_id))) {
                // Accepted without a chained id (unversioned route schema):
                // keep the current chain and record the acknowledgement.
                set_error("");
            } else {
                set_chain_id(next_id);
                set_error("");
            }
            last_ack_ms_.store(now_ms, std::memory_order_relaxed);
            last_ok_.store(true, std::memory_order_relaxed);
            ++acks_;
            return;
        }
        if (http_code == 400) {
            char expected[64]{};
            if (heartbeat::parse_invalid_id_error(response, response_len, expected,
                                                  sizeof(expected))) {
                set_chain_id(expected);
                ++resyncs_;
                set_error("invalid heartbeat id: adopted the expected id and will retry");
                last_ok_.store(true, std::memory_order_relaxed);  // retry on the normal cadence
                return;
            }
            set_error("HTTP 400 without a usable expected id");
            ++failures_;
            last_ok_.store(false, std::memory_order_relaxed);
            return;
        }
        char message[64];
        std::snprintf(message, sizeof(message), "HTTP %ld from /v1/heartbeats",
                      http_code);
        set_error(message);
        ++failures_;
        last_ok_.store(false, std::memory_order_relaxed);
    }

    void update_state(uint64_t now_ms) {
        const heartbeat::State next = heartbeat::evaluate(
            now_ms, started_ms_.load(std::memory_order_relaxed),
            last_ack_ms_.load(std::memory_order_relaxed), thresholds_);
        const heartbeat::State previous =
            state_.exchange(next, std::memory_order_acq_rel);
        if (next == heartbeat::State::kCritical &&
            previous != heartbeat::State::kCritical) {
            ++timeouts_;
            critical_event_.store(true, std::memory_order_release);
        }
    }

    void set_error(const char* text) {
        std::lock_guard<std::mutex> lock(text_mutex_);
        std::snprintf(last_error_, sizeof(last_error_), "%s", text);
    }

    static void copy_text(char* out, size_t cap, const char* text) {
        if (!out || cap == 0) return;
        size_t i = 0;
        for (; i + 1 < cap && text[i] != '\0'; ++i) out[i] = text[i];
        out[i] = '\0';
    }

    void set_chain_id(const char* text) {
        std::lock_guard<std::mutex> lock(text_mutex_);
        std::snprintf(chain_id_, sizeof(chain_id_), "%s", text);
    }

    Transport& transport_;
    heartbeat::Thresholds thresholds_;
    uint64_t (*clock_ms_)() noexcept;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<heartbeat::State> state_{heartbeat::State::kIdle};
    std::atomic<uint64_t> started_ms_{0};
    std::atomic<uint64_t> last_attempt_ms_{0};
    std::atomic<uint64_t> last_ack_ms_{0};
    std::atomic<bool> critical_event_{false};
    std::atomic<uint64_t> sends_{0};
    std::atomic<uint64_t> acks_{0};
    std::atomic<uint64_t> failures_{0};
    std::atomic<uint64_t> resyncs_{0};
    std::atomic<uint64_t> timeouts_{0};
    // Read by step()/send_now() from the caller thread and written by the
    // service thread (or by the caller in step-driven use), so it is atomic.
    std::atomic<bool> last_ok_{true};
    mutable std::mutex text_mutex_;
    char chain_id_[64]{};
    char last_error_[160]{};
};

#endif  // ORDER_HEARTBEAT_HPP
