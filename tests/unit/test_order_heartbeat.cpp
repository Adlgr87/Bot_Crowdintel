// Phase 4 unit tests: L2 request signing and the order heartbeat.
//
// The signature vectors below were computed independently (Python
// hmac/hashlib/base64) from the documented rule
//   message = timestamp + METHOD + path [+ exact body]
//   signature = urlsafeBase64WithPadding(HMAC-SHA256(base64Decode(secret), message))
// so a regression in the C++ signer cannot pass this suite.

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../../core/src/l2_auth.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/order_heartbeat.hpp"
#include "../../core/src/order_ledger.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__);      \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

constexpr const char* kSecretB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
constexpr const char* kAddress =
    "0x1234567890123456789012345678901234567890";

// ── L2 signing ───────────────────────────────────────────────────────────────

void test_l2_signing_vectors() {
    std::printf("l2_signing_vectors\n");
    uint8_t secret[l2auth::kSecretMaxBytes];
    size_t secret_len = 0;
    CHECK(l2auth::decode_secret(kSecretB64, secret, sizeof(secret), secret_len) &&
              secret_len == 32,
          "secret decodes to 32 bytes");

    char signature[l2auth::kSignatureB64Chars + 1]{};
    size_t signature_len = 0;
    const char* body = "{\"heartbeat_id\":\"\"}";
    CHECK(l2auth::sign(secret, secret_len, "1700000000", "POST", "/v1/heartbeats",
                       body, std::strlen(body), signature, sizeof(signature),
                       signature_len) &&
              std::strcmp(signature, "gnGt9dMY7xDmqdx8SjC0pG3lzRjw-b7aijqO032Kp2Q=") == 0,
          "heartbeat signature matches the independent vector");

    const char* chained = "{\"heartbeat_id\":\"abc123\"}";
    CHECK(l2auth::sign(secret, secret_len, "1700000000", "POST", "/v1/heartbeats",
                       chained, std::strlen(chained), signature,
                       sizeof(signature), signature_len) &&
              std::strcmp(signature,
                          "Nzqg-M9GhUaFQncUzw4gU-2eO7FJ17s8mhlzAl3OMqc=") == 0,
          "chained-id signature matches");

    CHECK(l2auth::sign(secret, secret_len, "1700000000", "POST", "/order", "ab", 2,
                       signature, sizeof(signature), signature_len) &&
              std::strcmp(signature,
                          "J64kLWfz3vHSF242zvzEuoPeV5B8CLVm7qOyad4hdZE=") == 0,
          "order signature matches");

    // A body-less request signs timestamp+method+path only.
    CHECK(l2auth::sign(secret, secret_len, "1700000000", "DELETE", "/cancel-all",
                       nullptr, 0, signature, sizeof(signature), signature_len) &&
              signature_len == l2auth::kSignatureB64Chars,
          "body-less signature measured");

    // Rejections: this path must never emit a plausible-looking signature.
    CHECK(!l2auth::sign(secret, secret_len, "1700000000", "POST", "/v1/heartbeats",
                        body, std::strlen(body), signature, 16, signature_len),
          "undersized output buffer rejected");
    CHECK(!l2auth::sign(nullptr, 32, "1700000000", "POST", "/v1/heartbeats", body,
                        std::strlen(body), signature, sizeof(signature),
                        signature_len),
          "missing secret rejected");
    CHECK(!l2auth::decode_secret("not base64!!", secret, sizeof(secret),
                                 secret_len),
          "invalid secret rejected");
    CHECK(!l2auth::decode_secret("", secret, sizeof(secret), secret_len),
          "empty secret rejected");
}

void test_l2_headers() {
    std::printf("l2_headers\n");
    l2auth::Credentials credentials{"api-key-uuid", kAddress, kSecretB64, "pass-1"};
    uint8_t secret[l2auth::kSecretMaxBytes];
    size_t secret_len = 0;
    CHECK(l2auth::decode_secret(kSecretB64, secret, sizeof(secret), secret_len),
          "secret decodes");
    l2auth::Headers headers;
    CHECK(l2auth::build_headers(credentials, secret, secret_len, "1700000000",
                                "POST", "/v1/heartbeats", "{}", 2, headers),
          "headers build");
    CHECK(std::strcmp(headers.address,
                      "POLY_ADDRESS: 0x1234567890123456789012345678901234567890") == 0,
          "address header");
    CHECK(std::strcmp(headers.api_key, "POLY_API_KEY: api-key-uuid") == 0,
          "api key header");
    CHECK(std::strcmp(headers.passphrase, "POLY_PASSPHRASE: pass-1") == 0,
          "passphrase header");
    constexpr size_t kSignaturePrefixChars = 16;  // "POLY_SIGNATURE: "
    CHECK(std::strncmp(headers.signature, "POLY_SIGNATURE: ",
                       kSignaturePrefixChars) == 0 &&
              std::strlen(headers.signature) ==
                  kSignaturePrefixChars + l2auth::kSignatureB64Chars,
          "signature header");
    CHECK(std::strcmp(headers.timestamp, "POLY_TIMESTAMP: 1700000000") == 0,
          "timestamp header");
    l2auth::Credentials incomplete{"api-key-uuid", nullptr, kSecretB64, "pass-1"};
    CHECK(!l2auth::build_headers(incomplete, secret, secret_len, "1700000000",
                                 "POST", "/v1/heartbeats", "{}", 2, headers),
          "incomplete credentials rejected");
}

// ── pure heartbeat logic ─────────────────────────────────────────────────────

void test_body_and_response_parsing() {
    std::printf("heartbeat_body_and_responses\n");
    char body[128];
    size_t body_len = 0;
    CHECK(heartbeat::build_body("", body, sizeof(body), body_len) &&
              std::strcmp(body, "{\"heartbeat_id\":\"\"}") == 0,
          "first body is the documented empty id");
    CHECK(heartbeat::build_body("abc-123", body, sizeof(body), body_len) &&
              std::strcmp(body, "{\"heartbeat_id\":\"abc-123\"}") == 0,
          "chained body");
    CHECK(!heartbeat::build_body("bad\"id", body, sizeof(body), body_len),
          "quote in id refused");
    CHECK(!heartbeat::build_body("bad\\id", body, sizeof(body), body_len),
          "backslash in id refused");
    CHECK(!heartbeat::build_body("bad\nid", body, sizeof(body), body_len),
          "control character in id refused");
    CHECK(!heartbeat::build_body(nullptr, body, 8, body_len),
          "undersized buffer refused");

    char next_id[64]{};
    const char* ok = R"({"heartbeat_id":"9f8e7d6c-1234-5678-9abc-def012345678"})";
    CHECK(heartbeat::parse_ack(ok, std::strlen(ok), next_id, sizeof(next_id)) &&
              std::strcmp(next_id, "9f8e7d6c-1234-5678-9abc-def012345678") == 0,
          "ack carries the next id");
    const char* status_only = R"({"status":"ok"})";
    CHECK(!heartbeat::parse_ack(status_only, std::strlen(status_only), next_id,
                                sizeof(next_id)),
          "status-only response is not an id");
    const char* empty_id = R"({"heartbeat_id":""})";
    CHECK(!heartbeat::parse_ack(empty_id, std::strlen(empty_id), next_id,
                                sizeof(next_id)),
          "empty id is not a chain");
    const char* duplicate =
        R"({"heartbeat_id":"a","heartbeat_id":"b"})";
    CHECK(!heartbeat::parse_ack(duplicate, std::strlen(duplicate), next_id,
                                sizeof(next_id)),
          "duplicate id rejected");
    const char* malformed = R"({"heartbeat_id":)";
    CHECK(!heartbeat::parse_ack(malformed, std::strlen(malformed), next_id,
                                sizeof(next_id)),
          "malformed response rejected");

    char expected[64]{};
    const char* invalid =
        R"({"error_msg":"Invalid Heartbeat ID","heartbeat_id":"expected-id"})";
    CHECK(heartbeat::parse_invalid_id_error(invalid, std::strlen(invalid),
                                            expected, sizeof(expected)) &&
              std::strcmp(expected, "expected-id") == 0,
          "400 carries the expected id");
    const char* other_error = R"({"error_msg":"Rate limited"})";
    CHECK(!heartbeat::parse_invalid_id_error(other_error,
                                             std::strlen(other_error), expected,
                                             sizeof(expected)),
          "unrelated error rejected");
}

void test_state_thresholds() {
    std::printf("heartbeat_thresholds\n");
    const heartbeat::Thresholds thresholds;  // 5 / 7 / 10 s
    CHECK(thresholds.interval_ms == 5000 && thresholds.degraded_ms == 7000 &&
              thresholds.critical_ms == 10000,
          "documented 5/7/10 defaults");

    const uint64_t started = 1000;
    CHECK(heartbeat::evaluate(0, 0, 0, thresholds) == heartbeat::State::kIdle,
          "not started is idle");
    CHECK(heartbeat::evaluate(started + 4999, started, 0, thresholds) ==
              heartbeat::State::kHealthy,
          "healthy before the degraded bound");
    CHECK(heartbeat::evaluate(started + 7000, started, 0, thresholds) ==
              heartbeat::State::kDegraded,
          "degraded at 7 s");
    CHECK(heartbeat::evaluate(started + 9999, started, 0, thresholds) ==
              heartbeat::State::kDegraded,
          "still degraded just before 10 s");
    CHECK(heartbeat::evaluate(started + 10000, started, 0, thresholds) ==
              heartbeat::State::kCritical,
          "critical at 10 s without an ack");
    CHECK(heartbeat::evaluate(started + 4000, started, started + 1000, thresholds) ==
              heartbeat::State::kHealthy,
          "ack resets the clock");
    CHECK(heartbeat::evaluate(started + 8000, started, started + 1000, thresholds) ==
              heartbeat::State::kDegraded,
          "7 s after the last ack is degraded");
    CHECK(heartbeat::evaluate(started + 11000, started, started + 1000, thresholds) ==
              heartbeat::State::kCritical,
          "10 s after the last ack is critical");
    CHECK(heartbeat::evaluate(started - 5, started, started - 100000, thresholds) ==
              heartbeat::State::kHealthy,
          "clock going backwards is not a failure");

    CHECK(heartbeat::next_delay_ms(thresholds, true) == 5000 &&
              heartbeat::next_delay_ms(thresholds, false) == 1000,
          "retry cadence after a failure");
    CHECK(std::strcmp(heartbeat::state_name(heartbeat::State::kCritical),
                      "CRITICAL") == 0,
          "state names");
}

// ── service with a scripted transport ────────────────────────────────────────

struct ScriptedTransport {
    struct Call {
        std::string body;
    };
    std::vector<Call> calls;
    long http_code = 200;
    std::string response = R"({"heartbeat_id":"h-2"})";
    bool transport_ok = true;
    int failures_left = 0;

    bool post(const char* body, size_t body_len, long& code, char* out,
              size_t out_cap, size_t& out_len) {
        calls.push_back(Call{std::string(body, body_len)});
        if (failures_left > 0) {
            --failures_left;
            code = 0;
            out_len = 0;
            if (out && out_cap) out[0] = '\0';
            return false;
        }
        if (!transport_ok) {
            code = 0;
            out_len = 0;
            return false;
        }
        code = http_code;
        out_len = response.size();
        if (out && out_cap) {
            const size_t copied =
                out_len < out_cap - 1 ? out_len : out_cap - 1;
            std::memcpy(out, response.data(), copied);
            out[copied] = '\0';
        }
        return true;
    }
};

uint64_t g_fake_now_ms = 0;
uint64_t fake_clock() noexcept { return g_fake_now_ms; }

void test_service_chains_and_recovers() {
    std::printf("heartbeat_service_chain\n");
    ScriptedTransport transport;
    heartbeat::Thresholds thresholds;
    HeartbeatService<ScriptedTransport> service(transport, thresholds,
                                                fake_clock);

    // First send uses the documented empty id and adopts the returned one.
    g_fake_now_ms = 100000;
    CHECK(service.send_now(g_fake_now_ms), "first send succeeds");
    CHECK(transport.calls.size() == 1 &&
              transport.calls[0].body == "{\"heartbeat_id\":\"\"}",
          "first body uses an empty id");
    char chain[64];
    service.chain_id_copy(chain, sizeof(chain));
    CHECK(std::strcmp(chain, "h-2") == 0, "chain adopts the new id");
    CHECK(service.state() == heartbeat::State::kHealthy, "healthy after the ack");
    CHECK(service.acks() == 1 && service.sends() == 1, "counters");

    // The next send is due only after the documented 5 s cadence.
    transport.response = R"({"heartbeat_id":"h-3"})";
    g_fake_now_ms += 4999;
    CHECK(!service.step(g_fake_now_ms) && transport.calls.size() == 1,
          "no send before the cadence");
    CHECK(service.state() == heartbeat::State::kHealthy, "still healthy");
    g_fake_now_ms += 1;
    CHECK(service.step(g_fake_now_ms) && transport.calls.size() == 2 &&
              transport.calls[1].body == "{\"heartbeat_id\":\"h-2\"}",
          "send at the cadence carries the chained id");

    // A transport failure is retried sooner (1 s) and degrades the state.
    transport.failures_left = 1;
    g_fake_now_ms += 5000;
    CHECK(service.step(g_fake_now_ms), "failed send is attempted");
    CHECK(service.failures() == 1, "failure counted");
    g_fake_now_ms += 999;
    CHECK(!service.step(g_fake_now_ms), "no retry before the retry delay");
    g_fake_now_ms += 1;
    CHECK(service.step(g_fake_now_ms), "retry after 1 s");
    CHECK(service.failures() == 1 && service.acks() == 3, "recovered");

    // The venue stops answering: sends keep failing, so the ack clock ages.
    CHECK(!service.take_critical_event(), "no critical event while healthy");
    transport.failures_left = 100;  // venue unreachable
    g_fake_now_ms += 7000;
    service.step(g_fake_now_ms);
    CHECK(service.state() == heartbeat::State::kDegraded, "degraded at 7 s");
    CHECK(!service.take_critical_event(), "no critical event at 7 s");

    // 10 s without an ack: critical, signalled exactly once.
    g_fake_now_ms += 3000;
    service.step(g_fake_now_ms);
    CHECK(service.state() == heartbeat::State::kCritical, "critical at 10 s");
    CHECK(service.take_critical_event(), "critical episode reported");
    CHECK(!service.take_critical_event(), "critical episode is consumed once");
    g_fake_now_ms += 1000;
    service.step(g_fake_now_ms);
    CHECK(service.state() == heartbeat::State::kCritical &&
              !service.take_critical_event(),
          "staying critical does not re-signal");
    CHECK(service.timeouts() == 1, "timeout counted once");

    // Recovery does not clear the episode flag by itself, but a fresh
    // critical episode would be reported again.
    transport.failures_left = 0;
    g_fake_now_ms += 1000;
    CHECK(service.step(g_fake_now_ms), "recovery send");
    CHECK(service.state() == heartbeat::State::kHealthy, "healthy again");

    // 400 with the expected id: adopted and retried on the normal cadence.
    transport.http_code = 400;
    transport.response =
        R"({"error_msg":"Invalid Heartbeat ID","heartbeat_id":"h-9"})";
    g_fake_now_ms += 5000;
    CHECK(service.step(g_fake_now_ms), "400 handled");
    service.chain_id_copy(chain, sizeof(chain));
    CHECK(service.resyncs() == 1 && std::strcmp(chain, "h-9") == 0,
          "expected id adopted");
    transport.http_code = 200;
    transport.response = R"({"heartbeat_id":"h-10"})";
    g_fake_now_ms += 5000;
    CHECK(service.step(g_fake_now_ms) &&
              transport.calls.back().body == "{\"heartbeat_id\":\"h-9\"}",
          "resync id used on the next send");
}

void test_service_400_without_id_and_5xx() {
    std::printf("heartbeat_service_errors\n");
    ScriptedTransport transport;
    heartbeat::Thresholds thresholds;
    HeartbeatService<ScriptedTransport> service(transport, thresholds,
                                                fake_clock);
    g_fake_now_ms = 50000;
    CHECK(service.send_now(g_fake_now_ms), "first send");
    const size_t calls_after_first = transport.calls.size();

    transport.http_code = 400;
    transport.response = R"({"error_msg":"Not an id error"})";
    g_fake_now_ms += 5000;
    service.step(g_fake_now_ms);
    CHECK(service.resyncs() == 0 && service.failures() == 1,
          "400 without an expected id counts as a failure");
    char last_error[128];
    service.last_error_copy(last_error, sizeof(last_error));
    CHECK(std::strstr(last_error, "400") != nullptr,
          "error message names the status");

    transport.http_code = 503;
    transport.response = R"({"error":"Service Unavailable"})";
    g_fake_now_ms += 1000;
    service.step(g_fake_now_ms);
    CHECK(service.failures() == 2, "5xx counts as a failure");
    CHECK(transport.calls.size() > calls_after_first, "requests were attempted");

    // A 200 in the unversioned shape ({"status":"ok"}) is still an
    // acknowledgement: the venue accepted the heartbeat, so the timer resets
    // and the previous chain id is kept for the next request.
    const uint64_t acks_before = service.acks();
    char chain_before[64];
    service.chain_id_copy(chain_before, sizeof(chain_before));
    transport.http_code = 200;
    transport.response = R"({"status":"ok"})";
    g_fake_now_ms += 1000;
    CHECK(service.step(g_fake_now_ms) && service.acks() == acks_before + 1,
          "status-only acknowledgement counted");
    char chain_after[64];
    service.chain_id_copy(chain_after, sizeof(chain_after));
    CHECK(std::strcmp(chain_before, chain_after) == 0, "chain id unchanged");
    CHECK(service.state() == heartbeat::State::kHealthy, "healthy after it");
}

// ── ledger integration ───────────────────────────────────────────────────────

void test_transport_lost_marks_orders_unknown() {
    std::printf("heartbeat_ledger_integration\n");
    char templ[] = "/tmp/crowdintel-heartbeat-XXXXXX";
    const char* dir = ::mkdtemp(templ);
    CHECK(dir != nullptr, "temp dir");
    const std::string path = std::string(dir) + "/orders.journal";
    char error[192];

    cledger::OrderLedger ledger;
    cledger::OpenOptions options;
    options.fsync_records = false;
    CHECK(ledger.open(path.c_str(), options, error, sizeof(error)), "ledger opens");

    for (int i = 0; i < 3; ++i) {
        cledger::IntentRecord intent{};
        std::snprintf(intent.client_order_id, sizeof(intent.client_order_id),
                      "hb-%d", i);
        intent.signal_id = static_cast<uint64_t>(i);
        intent.market_hash = 1;
        intent.price_fixed6 = 500000;
        intent.shares_fixed6 = 10000000;
        intent.notional_fixed6 = 5000000;
        intent.side = K_SIDE_BUY;
        intent.order_type = 1;
        CHECK(ledger.record_intent(intent, error, sizeof(error)), "intent");
        CHECK(ledger.record_transition(intent.client_order_id,
                                       cledger::LedgerEvent::kSubmitAck,
                                       cledger::Evidence::kVenueAck, "venue",
                                       0, error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "ack");
    }
    // Order 2 is already terminal: heartbeat loss must not touch it.
    CHECK(ledger.record_transition("hb-2", cledger::LedgerEvent::kOrderCanceled,
                                   cledger::Evidence::kVenueChannel, "", 0, error,
                                   sizeof(error))
              .result == cledger::TransitionResult::kApplied,
          "third order canceled");
    CHECK(!ledger.gate_open(), "unconfirmed orders already block trading");

    const size_t moved = ledger.mark_transport_lost(error, sizeof(error));
    CHECK(moved == 2, "both non-terminal orders moved to UNKNOWN");
    CHECK(ledger.unknown_orders() == 2, "UNKNOWN recorded");
    CHECK(ledger.find("hb-0")->state == cledger::OrderState::kUnknown &&
              ledger.find("hb-1")->state == cledger::OrderState::kUnknown,
          "states are UNKNOWN");
    CHECK(ledger.find("hb-2")->state == cledger::OrderState::kCanceled,
          "terminal order untouched");
    CHECK(!ledger.gate_open(), "trading blocked until reconciliation");

    // Idempotent: a second loss episode with no live orders moves nothing.
    CHECK(ledger.mark_transport_lost(error, sizeof(error)) == 0,
          "nothing left to mark");
    CHECK(std::strcmp(cledger::ledger_event_name(
                          cledger::LedgerEvent::kHeartbeatLost),
                      "HEARTBEAT_LOST") == 0,
          "event name");

    // The transition is durable: reopening the journal keeps the orders
    // unproven.
    ledger.close();
    cledger::OrderLedger reopened;
    CHECK(reopened.open(path.c_str(), options, error, sizeof(error)),
          "journal reopens");
    CHECK(reopened.unknown_orders() == 2 && !reopened.gate_open(),
          "UNKNOWN survives a restart");

    ::unlink(path.c_str());
    ::rmdir(dir);
}

// Mirrors the production sequence in main_hot_path.cpp: a critical heartbeat
// pauses the engine, marks the transport lost and leaves every open order
// unproven until Phase 5 reconciliation.
void test_critical_heartbeat_pauses_trading() {
    std::printf("heartbeat_pauses_trading\n");
    char templ[] = "/tmp/crowdintel-heartbeat-pause-XXXXXX";
    const char* dir = ::mkdtemp(templ);
    CHECK(dir != nullptr, "temp dir");
    const std::string path = std::string(dir) + "/orders.journal";
    char error[192];

    cledger::OrderLedger ledger;
    cledger::OpenOptions options;
    options.fsync_records = false;
    CHECK(ledger.open(path.c_str(), options, error, sizeof(error)), "ledger opens");
    cledger::IntentRecord intent{};
    std::snprintf(intent.client_order_id, sizeof(intent.client_order_id), "pause-1");
    intent.signal_id = 1;
    intent.market_hash = 7;
    intent.price_fixed6 = 400000;
    intent.shares_fixed6 = 5000000;
    intent.notional_fixed6 = 2000000;
    intent.side = K_SIDE_BUY;
    intent.order_type = 1;
    CHECK(ledger.record_intent(intent, error, sizeof(error)), "intent");
    CHECK(ledger.record_transition("pause-1", cledger::LedgerEvent::kSubmitAck,
                                   cledger::Evidence::kVenueAck, "venue", 0,
                                   error, sizeof(error))
              .result == cledger::TransitionResult::kApplied,
          "ack");

    ScriptedTransport transport;
    heartbeat::Thresholds thresholds;
    HeartbeatService<ScriptedTransport> service(transport, thresholds, fake_clock);
    std::atomic<bool> trading_enabled{true};
    uint64_t paused_events = 0;

    g_fake_now_ms = 1000000;
    service.start();
    // First ack, then the venue goes silent.
    service.send_now(g_fake_now_ms);
    transport.failures_left = 1000;
    // 10 s without a valid heartbeat: the venue may cancel every order.
    g_fake_now_ms += 10000;
    service.step(g_fake_now_ms);

    if (service.take_critical_event()) {
        ++paused_events;
        trading_enabled.store(false, std::memory_order_release);
        ledger.mark_transport_lost(error, sizeof(error));
    }
    CHECK(paused_events == 1, "one pause event");
    CHECK(!trading_enabled.load(), "trading disabled");
    CHECK(ledger.unknown_orders() == 1 && !ledger.gate_open(),
          "order unproven and gate closed");
    // The service keeps the heartbeat alive; recovery alone does not resume.
    transport.failures_left = 0;
    g_fake_now_ms += 50000;
    service.step(g_fake_now_ms);
    CHECK(service.state() == heartbeat::State::kHealthy, "heartbeat healthy again");
    CHECK(!trading_enabled.load() && !ledger.gate_open(),
          "trading stays paused until reconciliation");
    service.stop();

    ::unlink(path.c_str());
    ::rmdir(dir);
}

#if defined(CROWDINTEL_HAVE_NETWORK)
void test_transport_configuration() {
    std::printf("heartbeat_transport_configuration\n");
    l2auth::Credentials credentials{"api-key", kAddress, kSecretB64, "pass"};
    HeartbeatTransport transport("https://clob.polymarket.com", credentials);
    CHECK(transport.usable(), "transport is usable with a valid secret");
    l2auth::Credentials bad{"api-key", kAddress, "%%%not-base64%%%", "pass"};
    HeartbeatTransport broken("https://clob.polymarket.com", bad);
    CHECK(!broken.usable(), "undecodable secret refuses to send");
    long http_code = 0;
    char response[64];
    size_t response_len = 0;
    CHECK(!broken.post("{}", 2, http_code, response, sizeof(response),
                       response_len),
          "broken transport never sends");
}
#endif

}  // namespace

int main() {
    std::printf("== CROWDINTEL heartbeat tests ==\n");
    test_l2_signing_vectors();
    test_l2_headers();
    test_body_and_response_parsing();
    test_state_thresholds();
    test_service_chains_and_recovers();
    test_service_400_without_id_and_5xx();
    test_transport_lost_marks_orders_unknown();
    test_critical_heartbeat_pauses_trading();
#if defined(CROWDINTEL_HAVE_NETWORK)
    test_transport_configuration();
#endif
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
