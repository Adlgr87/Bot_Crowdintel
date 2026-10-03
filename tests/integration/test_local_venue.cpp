// ═══════════════════════════════════════════════════════════════════════════
// Integration tests against an in-process venue (tests/support/local_venue.hpp).
//
// These are the only tests that put the *production* components on real sockets
// and real threads at the same time, which is what makes ThreadSanitizer
// meaningful for the live topology:
//
//   CurlTransport → ClobApiClient → OrderGateway → LightweightCLOBClient
//   → egress::LedgerOrderObserver → recorder::OrderRecorder → EventLedger
//   UserWsClient → ws::Session → user_ws::FrameProcessor → UserEventApplier
//   OrderHeartbeat → ClobApiClient → EventLedger
//   Reconciler → ClobApiClient → EventLedger
//
// Required coverage (canary precondition, decided 2026-10-03):
//   1. duplicated and out-of-order user-channel events
//   2. disconnect in the middle of a message
//   3. heartbeat rejected (HTTP 400) and heartbeat delayed past the budget
//   4. HTTP 200 carrying an invalid body
//   5. timeout during POST /order → UNKNOWN, no retry, resolved by reconciliation
//   6. restart with an order still open on the venue
//
// Nothing here touches the real venue, a wallet or credentials: the "secret" is
// a fixture, the server is loopback, and no order can leave the process.
// Network build only (CROWDINTEL_NETWORK=ON); registered in CTest as
// `local_venue_integration`.
// ═══════════════════════════════════════════════════════════════════════════

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/include/event_ledger.hpp"
#include "../../core/include/venue_metadata.hpp"
#include "../../core/src/curl_transport.hpp"
#include "../../core/src/ledger_order_observer.hpp"
#include "../../core/src/lightweight_client.hpp"
#include "../../core/src/order_gateway.hpp"
#include "../../core/src/order_heartbeat.hpp"
#include "../../core/src/reconciliation.hpp"
#include "../../core/src/rpc_client.hpp"
#include "../../core/src/user_ws_client.hpp"
#include "../support/local_venue.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));  \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

#define SECTION(name) std::printf("%s\n", name)

constexpr const char* K_CONDITION_ID =
    "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75";
constexpr const char* K_TOKEN_YES =
    "107505882767731489358349912513945399560393482969656700824895970500493757150417";
constexpr const char* K_EXCHANGE = "0xE111180000d2663C0091e4f400237545B87B996B";
constexpr const char* K_ADDRESS = "0xC0Ffe2585196fe484bB6861E1c19f7b3Fc269724";
constexpr const char* K_API_KEY = "7b1e2d60-6f9a-4dd7-8f3e-21b8f94c77a2";
constexpr const char* K_API_SECRET = "Rnl2cWN0Rk5sc1ZzR1E9PQ==";  // fixture, not a credential
constexpr const char* K_PASSPHRASE = "9fH3kL7m2qW8xP4r";

// Per-process root: two instances of this binary (ctest -j, a stress loop, an
// editor task) must not share ledger directories, or one restarts onto the
// other's journal and the assertions become meaningless.
std::string integration_root() {
    std::string root = "/tmp/crowdintel-integration-";
    root += std::to_string(static_cast<long>(::getpid()));
    ::mkdir(root.c_str(), 0700);
    return root;
}

std::string state_dir(const char* name) {
    std::string path = integration_root() + "/" + name;
    const std::string command = "rm -rf " + path;
    if (std::system(command.c_str()) != 0) std::printf("  (cleanup warning)\n");
    return path;
}

ledger::EventLedger::Options ledger_options(const std::string& path) {
    ledger::EventLedger::Options options{};
    std::snprintf(options.directory, sizeof(options.directory), "%s", path.c_str());
    options.fsync_each_append = false;  // tests: speed over durability theatre
    options.checkpoint_every = 64;
    return options;
}

// A configuration built by hand: the integration tests must not depend on the
// process environment, and they must never be able to select live mode.
struct TestConfig {
    MarketConfig config;
    TestConfig(const std::string& http_base, const std::string& ledger_dir) {
        config.bot_mode = BotMode::PAPER;  // never LIVE inside a test
        config.mock_mode = true;
        config.live_armed = false;
        std::snprintf(config.clob_host, sizeof(config.clob_host), "%s",
                      http_base.c_str());
        std::snprintf(config.gamma_host, sizeof(config.gamma_host), "%s",
                      http_base.c_str());
        std::snprintf(config.condition_id, sizeof(config.condition_id), "%s",
                      K_CONDITION_ID);
        std::snprintf(config.token_id_dec, sizeof(config.token_id_dec), "%s",
                      K_TOKEN_YES);
        uint64_t limbs[4] = {0, 0, 0, 0};
        venue::parse_uint256_limbs(K_TOKEN_YES, std::strlen(K_TOKEN_YES), limbs);
        for (int word = 0; word < 4; ++word)
            for (int byte = 0; byte < 8; ++byte)
                config.token_id_be[word * 8 + byte] = static_cast<uint8_t>(
                    (limbs[3 - word] >> (8 * (7 - byte))) & 0xFFULL);
        config.tick_size = 1000;
        config.min_size_shares = 5000000ULL;
        config.neg_risk = false;
        config.signature_type = 0;
        std::snprintf(config.order_type, sizeof(config.order_type), "FAK");
        std::snprintf(config.owner_api_key, sizeof(config.owner_api_key), "%s",
                      K_API_KEY);
        std::snprintf(config.api_secret_b64, sizeof(config.api_secret_b64), "%s",
                      K_API_SECRET);
        std::snprintf(config.api_passphrase, sizeof(config.api_passphrase), "%s",
                      K_PASSPHRASE);
        std::snprintf(config.api_address_hex, sizeof(config.api_address_hex), "%s",
                      K_ADDRESS);
        std::snprintf(config.maker_hex, sizeof(config.maker_hex), "%s", K_ADDRESS);
        std::snprintf(config.signer_hex, sizeof(config.signer_hex), "%s", K_ADDRESS);
        std::snprintf(config.market_slug, sizeof(config.market_slug), "integration");
        config.market_hash = alpha_hash_bytes(config.market_slug,
                                              std::strlen(config.market_slug));
        std::snprintf(config.ledger_dir, sizeof(config.ledger_dir), "%s",
                      ledger_dir.c_str());
        config.min_collateral_base = 1000000;
        config.target_allowance_base = 2000000;
        config.heartbeat_interval_ms = 5000;
        config.heartbeat_warn_ms = 7000;
        config.heartbeat_block_ms = 9000;
        config.heartbeat_assume_cancelled_ms = 10000;
    }
};

clob::Credentials test_credentials() {
    clob::Credentials credentials{};
    std::snprintf(credentials.address, sizeof(credentials.address), "%s", K_ADDRESS);
    std::snprintf(credentials.api_key, sizeof(credentials.api_key), "%s", K_API_KEY);
    std::snprintf(credentials.api_secret_b64, sizeof(credentials.api_secret_b64),
                  "%s", K_API_SECRET);
    std::snprintf(credentials.api_passphrase, sizeof(credentials.api_passphrase),
                  "%s", K_PASSPHRASE);
    credentials.signature_type = 0;
    return credentials;
}

std::string order_json(const char* id, const char* status, const char* original,
                       const char* matched, const char* side = "BUY",
                       const char* price = "0.49") {
    std::string json = "{\"id\":\"";
    json += id;
    json += "\",\"market\":\"";
    json += K_CONDITION_ID;
    json += "\",\"asset_id\":\"";
    json += K_TOKEN_YES;
    json += "\",\"owner\":\"";
    json += K_API_KEY;
    json += "\",\"maker_address\":\"";
    json += K_ADDRESS;
    json += "\",\"side\":\"";
    json += side;
    json += "\",\"price\":\"";
    json += price;
    json += "\",\"original_size\":\"";
    json += original;
    json += "\",\"size_matched\":\"";
    json += matched;
    json += "\",\"outcome\":\"Yes\",\"order_type\":\"GTC\",\"status\":\"";
    json += status;
    json += "\",\"created_at\":1714136516,\"expiration\":\"0\"}";
    return json;
}

std::string orders_page(const std::vector<std::string>& items,
                        const char* cursor = "LTE=") {
    std::string json = "{\"limit\":500,\"count\":";
    json += std::to_string(items.size());
    json += ",\"next_cursor\":\"";
    json += cursor;
    json += "\",\"data\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) json += ",";
        json += items[i];
    }
    json += "]}";
    return json;
}

// Trade frame including maker_orders, so the applier can attribute the fill to
// our resting order exactly as the venue does.
std::string trade_json(const char* id, const char* order_id, const char* status,
                       const char* size) {
    // Raw CLOB user-channel shape: a flat object discriminated by `event_type`,
    // with maker_orders so the applier attributes the fill to our resting order
    // (docs trading/realtime-order-updates; py-sdk models/clob/user_events.py).
    std::string json = "{\"event_type\":\"trade\",\"type\":\"TRADE\",\"id\":\"";
    json += id;
    json += "\",\"taker_order_id\":\"";
    json += order_id;
    json += "\",\"market\":\"";
    json += K_CONDITION_ID;
    json += "\",\"asset_id\":\"";
    json += K_TOKEN_YES;
    json += "\",\"side\":\"BUY\",\"size\":\"";
    json += size;
    json += "\",\"fee_rate_bps\":0,\"price\":\"0.49\",\"status\":\"";
    json += status;
    json += "\",\"match_time\":\"1714136520\",\"last_update\":\"1714136520\","
            "\"outcome\":\"Yes\",\"owner\":\"";
    json += K_API_KEY;
    json += "\",\"trade_owner\":\"";
    json += K_API_KEY;
    json += "\",\"maker_address\":\"";
    json += K_ADDRESS;
    json += "\",\"transaction_hash\":\"0xbeef\",\"bucket_index\":1,"
            "\"maker_orders\":[{\"order_id\":\"";
    json += order_id;
    json += "\",\"owner\":\"";
    json += K_API_KEY;
    json += "\",\"maker_address\":\"";
    json += K_ADDRESS;
    json += "\",\"matched_amount\":\"";
    json += size;
    json += "\",\"price\":\"0.49\",\"fee_rate_bps\":0,\"asset_id\":\"";
    json += K_TOKEN_YES;
    json += "\",\"outcome\":\"Yes\",\"outcome_index\":0,\"side\":\"BUY\"}],"
            "\"trader_side\":\"TAKER\",\"timestamp\":\"1714136520500\"}";
    return json;
}

std::string trades_page(const std::vector<std::string>& items) {
    std::string json = "{\"limit\":500,\"count\":";
    json += std::to_string(items.size());
    json += ",\"next_cursor\":\"LTE=\",\"data\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) json += ",";
        json += items[i];
    }
    json += "]}";
    return json;
}

std::string balances(const char* balance, const char* allowance) {
    std::string json = "{\"balance\":\"";
    json += balance;
    json += "\",\"allowances\":{\"";
    json += K_EXCHANGE;
    json += "\":\"";
    json += allowance;
    json += "\"}}";
    return json;
}

void stub_healthy_account(testvenue::Server& server) {
    server.on("GET", "/data/trades",
              testvenue::ResponseSpec{200, trades_page({}), testvenue::Fault::NONE, 0});
    server.on("GET", "asset_type=COLLATERAL",
              testvenue::ResponseSpec{200, balances("5000000", "5000000"),
                                      testvenue::Fault::NONE, 0});
    server.on("GET", "asset_type=CONDITIONAL",
              testvenue::ResponseSpec{200, balances("0", "1"),
                                      testvenue::Fault::NONE, 0});
}

recon::Context reconciliation_context() {
    recon::Context context{};
    std::snprintf(context.condition_id, sizeof(context.condition_id), "%s",
                  K_CONDITION_ID);
    std::snprintf(context.token_id, sizeof(context.token_id), "%s", K_TOKEN_YES);
    std::snprintf(context.collateral_spender, sizeof(context.collateral_spender),
                  "%s", K_EXCHANGE);
    context.neg_risk = false;
    context.min_collateral = 1000000;
    context.target_allowance = 2000000;
    context.signature_type = 0;
    return context;
}

// Signs a real V2 order and serialises the exact production wire body.
struct SignedOrder {
    EIP712Signer signer;
    WireBody body{};
    uint64_t salt = 0;
    uint8_t side = 0;
    uint64_t maker_amount = 0;
    uint64_t taker_amount = 0;
    bool ok = false;
};

void build_signed_order(SignedOrder& out, uint64_t salt, uint8_t side,
                        uint64_t price_raw, uint64_t shares) {
    uint8_t private_key[32];
    for (int i = 0; i < 32; ++i) private_key[i] = static_cast<uint8_t>(i + 7);
    if (!out.signer.init(private_key, false)) return;
    OrderV2 order{};
    order.salt = salt;
    std::memcpy(order.maker, out.signer.signer_address(), 20);
    std::memcpy(order.signer, out.signer.signer_address(), 20);
    uint64_t limbs[4] = {0, 0, 0, 0};
    if (!venue::parse_uint256_limbs(K_TOKEN_YES, std::strlen(K_TOKEN_YES), limbs))
        return;
    for (int word = 0; word < 4; ++word)
        for (int byte = 0; byte < 8; ++byte)
            order.token_id[word * 8 + byte] = static_cast<uint8_t>(
                (limbs[3 - word] >> (8 * (7 - byte))) & 0xFFULL);
    const uint64_t cost = price_raw * shares / 1000000ULL;
    order.maker_amount = side == 0 ? cost : shares;
    order.taker_amount = side == 0 ? shares : cost;
    order.side = side;
    order.signature_type = 0;
    order.timestamp_ms = 1714136516123ULL;
    uint8_t signature[65];
    if (!out.signer.sign_order(order, signature)) return;
    char maker_hex[43]{};
    char signer_hex[43]{};
    rpc::bytes_to_hex(order.maker, 20, maker_hex, sizeof(maker_hex));
    rpc::bytes_to_hex(order.signer, 20, signer_hex, sizeof(signer_hex));
    out.salt = salt;
    out.side = side;
    out.maker_amount = order.maker_amount;
    out.taker_amount = order.taker_amount;
    out.ok = build_wire_body(order, signature, K_TOKEN_YES, maker_hex, signer_hex,
                             K_API_KEY, "FAK", out.body, 0);
}


// Test-only egress client.  The production order client
// (LightweightCLOBClient) is HTTPS-only by design (CURLOPT_PROTOCOLS_STR),
// which the loopback venue cannot satisfy, so the test transports the same wire
// body through clob::CurlTransport and classifies the response with the
// *production* classifier (LightweightCLOBClient::classify_response) and the
// production ambiguity rule (clob::CurlTransport::is_ambiguous).  Everything
// downstream — gateway, observer, recorder, ledger — is the real code.
class TestEgressClient {
public:
    TestEgressClient(clob::CurlTransport& transport, const std::string& base)
        : transport_(transport), url_(base + "/order") {}
    void warmup() {}
    SubmitResult submit(const WireBody& body) {
        clob::RequestOptions options{2000, 3000, false};
        clob::HttpResponse response{};
        response.body = buffer_;
        transport_.request("POST", url_.c_str(), body.buf, body.len, nullptr, 0,
                           options, buffer_, sizeof(buffer_), response);
        last_len_ = response.body_len;
        last_truncated_ = response.truncated;
        if (!response.transport_ok) {
            SubmitResult result{};
            result.http_code = response.code;
            result.ambiguous = response.ambiguous;
            result.retryable = false;  // never replay a POST /order
            std::snprintf(result.error, sizeof(result.error), "%s", response.error);
            return result;
        }
        return LightweightCLOBClient::classify_response(response.code, buffer_,
                                                        response.body_len);
    }
    const char* last_response_body() const noexcept { return buffer_; }
    size_t last_response_len() const noexcept { return last_len_; }
    bool last_response_truncated() const noexcept { return last_truncated_; }

private:
    clob::CurlTransport& transport_;
    std::string url_;
    char buffer_[16384]{};
    size_t last_len_ = 0;
    bool last_truncated_ = false;
};

bool wait_for(const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

std::string user_order_frame(const char* id, const char* update_type,
                             const char* status, const char* original,
                             const char* matched, const char* timestamp_ms) {
    std::string json = "{\"event_type\":\"order\",\"id\":\"";
    json += id;
    json += "\",\"owner\":\"";
    json += K_API_KEY;
    json += "\",\"market\":\"";
    json += K_CONDITION_ID;
    json += "\",\"asset_id\":\"";
    json += K_TOKEN_YES;
    json += "\",\"side\":\"BUY\",\"order_owner\":\"";
    json += K_API_KEY;
    json += "\",\"original_size\":\"";
    json += original;
    json += "\",\"size_matched\":\"";
    json += matched;
    json += "\",\"price\":\"0.49\",\"associate_trades\":null,\"outcome\":\"Yes\","
            "\"type\":\"";
    json += update_type;
    json += "\",\"created_at\":\"1714136516\",\"expiration\":\"0\","
            "\"order_type\":\"GTC\",\"status\":\"";
    json += status;
    json += "\",\"maker_address\":\"";
    json += K_ADDRESS;
    json += "\",\"timestamp\":\"";
    json += timestamp_ms;
    json += "\"}";
    return json;
}

// ═══════════════════════════════════════════════════════════════════════════
// 1 + 2. User channel over a real socket: duplicates, out-of-order delivery,
//        keep-alive and a disconnect in the middle of a message.
// ═══════════════════════════════════════════════════════════════════════════
void test_user_channel_over_socket() {
    SECTION("integration/user_channel_over_socket");
    testvenue::Server server;
    CHECK(server.start("/ws/user"), "test venue started");
    const std::string dir = state_dir("user-channel");
    auto ledger = std::make_unique<ledger::EventLedger>();
    char error[192]{};
    CHECK(ledger->open(ledger_options(dir), error, sizeof(error)), error);

    user_ws::Config config{};
    config.enabled = true;
    std::snprintf(config.url, sizeof(config.url), "%s", server.ws_url().c_str());
    config.market_count = 1;
    std::snprintf(config.markets[0], sizeof(config.markets[0]), "%s", K_CONDITION_ID);
    config.require_tls = false;              // loopback test venue only
    config.allow_loopback_plaintext = true;
    config.keepalive_interval_ms = 1000;
    config.idle_timeout_ms = 4000;
    config.pong_timeout_ms = 6000;
    config.reconnect_min_ms = 50;
    config.reconnect_max_ms = 150;
    CHECK(config.validate(error, sizeof(error)), error);

    user_ws::ApplierContext context{};
    std::snprintf(context.api_owner, sizeof(context.api_owner), "%s", K_API_KEY);
    std::snprintf(context.maker_address, sizeof(context.maker_address), "%s", K_ADDRESS);
    std::snprintf(context.condition_id, sizeof(context.condition_id), "%s",
                  K_CONDITION_ID);
    std::snprintf(context.token_id, sizeof(context.token_id), "%s", K_TOKEN_YES);

    user_ws::UserWsClient client(config, test_credentials(), *ledger, context);
    CHECK(client.start(error, sizeof(error)), error);
    CHECK(server.wait_for_ws(5000), "client completed the WebSocket handshake");
    // The subscription frame must carry the credentials and the market filter.
    CHECK(wait_for([&] { return server.ws_frame_count() >= 1; }, 3000),
          "subscription frame received");
    const std::vector<std::string> frames = server.ws_frames();
    CHECK(!frames.empty() && frames[0].find("\"type\":\"user\"") != std::string::npos,
          "subscription frame is a user frame");
    CHECK(!frames.empty() && frames[0].find(K_CONDITION_ID) != std::string::npos,
          "subscription frame filters by condition id");
    CHECK(!frames.empty() && frames[0].find(K_API_SECRET) != std::string::npos,
          "subscription frame carries the L2 secret (loopback fixture only)");

    // Out-of-order: the trade arrives before the order that produced it, then
    // both are redelivered, then two UPDATEs arrive newest-first.
    const std::string trade = trade_json("t-1", "0xaa", "TRADE_STATUS_MATCHED", "40");
    const std::string placement =
        user_order_frame("0xaa", "PLACEMENT", "LIVE", "100", "0", "1714136516123");
    const std::string newer_update =
        user_order_frame("0xaa", "UPDATE", "MATCHED", "100", "40", "1714136520000");
    const std::string older_update =
        user_order_frame("0xaa", "UPDATE", "MATCHED", "100", "10", "1714136519000");
    server.ws_send_text(trade);
    server.ws_send_text(placement);
    server.ws_send_text(trade);       // duplicate trade
    server.ws_send_text(placement);   // duplicate placement
    server.ws_send_text(newer_update);
    server.ws_send_text(older_update);  // stale update, must not regress state

    // Cross-thread reads go through the ledger's locked accessors: the raw maps
    // belong to whoever holds guard() (here, the user-channel thread).
    //
    // Wait for the WHOLE batch to be consumed before asserting idempotency.  The
    // first trade already credits the fill and the matched size, so checking the
    // duplicates earlier can both fail spuriously (under TSan the reader thread
    // is slower) and pass vacuously (inventory "credited exactly once" before the
    // duplicate ever arrived proves nothing).  frames() >= 6 means the stale
    // UPDATE - sent last - was processed too.
    CHECK(wait_for([&] {
        return client.processor().duplicates() >= 2 &&
               client.processor().frames() >= 6;
    }, 5000),
          "duplicate and stale frames were processed");
    CHECK(wait_for([&] {
        ledger::FillRecord fill{};
        return ledger->fill_copy("t-1", fill);
    }, 5000),
          "fill applied from the stream");
    CHECK(wait_for([&] {
        ledger::OrderRecord snapshot{};
        return ledger->order_copy("0xaa", snapshot) &&
               snapshot.matched_size == 40000000ULL;
    }, 5000),
          "order matched size applied");
    ledger::OrderRecord record{};
    CHECK(ledger->order_copy("0xaa", record), "order record readable");
    CHECK(record.state == OrderState::PARTIALLY_FILLED,
          "out-of-order UPDATE did not regress the state");
    CHECK(record.matched_size == 40000000ULL,
          "matched size stayed monotonic under out-of-order delivery");
    ledger::PositionRecord position{};
    CHECK(ledger->position_copy(K_TOKEN_YES, position) &&
              position.shares == 40000000ULL,
          "inventory credited exactly once despite duplicate trades");
    CHECK(client.processor().duplicates() >= 2, "duplicates were detected");
    CHECK(client.processor().parse_failures() == 0, "no parse failures on valid frames");

    // Application-level keep-alive must be answered by the venue.
    CHECK(wait_for([&] { return client.processor().keepalive_acks() >= 1; }, 8000),
          "PING/PONG keep-alive observed");

    // Disconnect in the middle of a message: the client must not apply a partial
    // frame, must mark the account stale and must reconnect on the same thread.
    const size_t handshakes_before = server.ws_handshakes();
    const std::string big_frame =
        user_order_frame("0xbb", "PLACEMENT", "LIVE", "100", "0", "1714136530000");
    server.ws_send_partial_and_close(big_frame, 12);  // header + a few bytes only
    CHECK(wait_for([&] { return server.ws_handshakes() > handshakes_before; }, 8000),
          "client reconnected after a mid-message disconnect");
    CHECK(client.stale(), "the account view is stale until REST reconciliation");
    ledger::OrderRecord truncated{};
    CHECK(!ledger->order_copy("0xbb", truncated),
          "a truncated frame never became an order");
    CHECK(client.state() == user_ws::StreamState::SUBSCRIBED ||
              client.state() == user_ws::StreamState::CONNECTING ||
              client.state() == user_ws::StreamState::BACKOFF,
          "the single listener thread owns the reconnect");

    client.stop();
    client.join();
    server.stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Heartbeat over a real socket: rejection (HTTP 400) and delay.
// ═══════════════════════════════════════════════════════════════════════════
void test_heartbeat_over_socket() {
    SECTION("integration/heartbeat_over_socket");
    testvenue::Server server;
    CHECK(server.start(), "test venue started");
    const std::string dir = state_dir("heartbeat");
    auto ledger = std::make_unique<ledger::EventLedger>();
    char error[192]{};
    CHECK(ledger->open(ledger_options(dir), error, sizeof(error)), error);

    clob::CurlTransport transport;
    transport.allow_loopback_plain_http(true);  // loopback test venue only
    CHECK(transport.usable(), "curl transport usable");
    clob::ClobApiClient api(transport, test_credentials(), server.http_base().c_str(),
                            server.http_base().c_str());

    heartbeat::Config config{};
    config.enabled = true;
    config.interval_ms = 1000;
    config.warn_ms = 1200;
    config.block_ms = 1500;
    config.assume_cancelled_ms = 2000;
    config.max_consecutive_failures = 4;
    config.request_options.connect_timeout_ms = 200;
    config.request_options.total_timeout_ms = 400;
    CHECK(config.validate(error, sizeof(error)), error);
    heartbeat::OrderHeartbeat monitor(api, *ledger, config);

    // Accepted chain over the documented SDK path.
    server.on("POST", "/v1/heartbeats",
              testvenue::ResponseSpec{200, "{\"heartbeat_id\":\"hb-1\"}",
                                      testvenue::Fault::NONE, 0});
    CHECK(monitor.tick(error, sizeof(error)), error);
    CHECK(std::strcmp(monitor.current_id(), "hb-1") == 0, "id chained");
    CHECK(server.request_count("POST", "/v1/heartbeats") == 1, "one beat sent");
    CHECK(server.requests().back().body == "{\"heartbeat_id\":\"\"}",
          "the first beat sends the documented empty id");

    // Rejected with the expected id → invalidated, resynchronised, never guessed.
    server.once("POST", "/v1/heartbeats",
                testvenue::ResponseSpec{
                    400,
                    "{\"error_msg\":\"Invalid Heartbeat ID\",\"heartbeat_id\":\"hb-9\"}",
                    testvenue::Fault::NONE, 0});
    CHECK(!monitor.tick(error, sizeof(error)), "a rejected beat is not success");
    CHECK(monitor.invalidated(), "invalidation flagged");
    CHECK(monitor.health() == heartbeat::Health::INVALIDATED, "health invalidated");
    CHECK(std::strcmp(monitor.current_id(), "hb-9") == 0, "expected id adopted");
    CHECK(monitor.tick(error, sizeof(error)), "resynchronised with the expected id");
    CHECK(!monitor.invalidated(), "invalidation cleared after resync");
    CHECK(server.requests().back().body == "{\"heartbeat_id\":\"hb-9\"}",
          "the resync beat carries the venue's id");

    // Delayed beyond the request budget → ambiguous, never treated as success.
    server.once("POST", "/v1/heartbeats",
                testvenue::ResponseSpec{200, "{\"heartbeat_id\":\"hb-late\"}",
                                        testvenue::Fault::DELAY, 1500});
    CHECK(!monitor.tick(error, sizeof(error)), "a late beat is not an acknowledgement");
    CHECK(monitor.ambiguous_failures() >= 1, "ambiguity counted");
    CHECK(monitor.health() != heartbeat::Health::HEALTHY ||
              monitor.consecutive_failures() > 0,
          "the watchdog degrades after a late beat");
    CHECK(ledger->heartbeat().consecutive_failures >= 1,
          "the failure is durable in the ledger");

    // A venue that only implements the OpenAPI spelling (/heartbeats, answer
    // {"status":"ok"}) must still work: the SDK path answers 404 once and the
    // client falls back and remembers which path answered.
    server.clear_rules();
    // "/heartbeats" is a substring of "/v1/heartbeats", so the 404 for the SDK
    // spelling has to be a one-shot rule that wins the first match.
    server.once("POST", "/v1/heartbeats",
                testvenue::ResponseSpec{404, "{\"error\":\"not found\"}",
                                        testvenue::Fault::NONE, 0});
    server.on("POST", "/heartbeats",
              testvenue::ResponseSpec{200, "{\"status\":\"ok\"}",
                                      testvenue::Fault::NONE, 0});
    heartbeat::OrderHeartbeat monitor2(api, *ledger, config);
    CHECK(monitor2.tick(error, sizeof(error)), "fallback path accepted");
    CHECK(std::strcmp(api.heartbeat_path(), "/heartbeats") == 0,
          "the answering path is remembered");

    monitor.request_stop();
    monitor.join();
    monitor2.request_stop();
    monitor2.join();
    server.stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. HTTP 200 with an invalid body must never become state.
// ═══════════════════════════════════════════════════════════════════════════
void test_invalid_body_with_http_200() {
    SECTION("integration/invalid_body_http_200");
    testvenue::Server server;
    CHECK(server.start(), "test venue started");
    const std::string dir = state_dir("invalid-body");
    auto ledger = std::make_unique<ledger::EventLedger>();
    char error[192]{};
    CHECK(ledger->open(ledger_options(dir), error, sizeof(error)), error);
    clob::CurlTransport transport;
    transport.allow_loopback_plain_http(true);  // loopback test venue only
    clob::ClobApiClient api(transport, test_credentials(), server.http_base().c_str(),
                            server.http_base().c_str());

    server.on("GET", "/data/orders",
              testvenue::ResponseSpec{200, "{\"data\": [{\"id\":",
                                      testvenue::Fault::NONE, 0});
    server.on("GET", "/data/trades",
              testvenue::ResponseSpec{200, "not json at all",
                                      testvenue::Fault::NONE, 0});
    stub_healthy_account(server);

    recon::Reconciler reconciler(api, *ledger, reconciliation_context());
    recon::Report report{};
    CHECK(!reconciler.run_startup(report, nullptr), "invalid body blocks readiness");
    char reasons[512]{};
    report.format_reasons(reasons, sizeof(reasons));
    CHECK(std::strstr(reasons, "orders_query_failed") != nullptr ||
              std::strstr(reasons, "orders_page_incomplete") != nullptr,
          reasons);
    CHECK(ledger->order_count() == 0, "no order was invented from a broken body");
    CHECK(report.readiness == recon::Readiness::BLOCKED, "verdict is BLOCKED");

    // A truncated body (server hangs up mid-response) is ambiguous, not empty.
    server.clear_rules();
    server.once("GET", "/data/orders",
                testvenue::ResponseSpec{200, orders_page({order_json("0xaa", "LIVE",
                                                                     "10", "0")}),
                                        testvenue::Fault::CLOSE_MID_BODY, 0});
    stub_healthy_account(server);
    clob::OrderPage page{};
    clob::CallResult call{};
    CHECK(!api.open_orders(K_CONDITION_ID, nullptr, nullptr, page, call),
          "a truncated page is rejected");
    CHECK(call.ambiguous || !call.ok, "and reported as unusable");

    server.stop();
}

// ═══════════════════════════════════════════════════════════════════════════
// 5 + 6. Timeout during POST /order → UNKNOWN, no retry, resolved by
//        reconciliation; then a restart with the order still open.
// ═══════════════════════════════════════════════════════════════════════════
void test_unknown_order_and_restart() {
    SECTION("integration/unknown_order_and_restart");
    testvenue::Server server;
    CHECK(server.start(), "test venue started");
    const std::string dir = state_dir("unknown-order");
    auto ledger = std::make_unique<ledger::EventLedger>();
    char error[192]{};
    CHECK(ledger->open(ledger_options(dir), error, sizeof(error)), error);

    TestConfig test_config(server.http_base(), dir);
    const MarketConfig& config = test_config.config;

    SignedOrder signed_order{};
    build_signed_order(signed_order, 20261003ULL, 0, 490000ULL, 10000000ULL);
    CHECK(signed_order.ok, "order signed and serialized");

    std::atomic<bool> trading_enabled{true};
    egress::LedgerOrderObserver observer(ledger.get(), config, trading_enabled);
    clob::CurlTransport egress_transport;
    egress_transport.allow_loopback_plain_http(true);  // loopback test venue only
    CHECK(egress_transport.usable(), "egress transport usable");
    auto client = std::make_unique<TestEgressClient>(egress_transport,
                                                     server.http_base());
    auto gateway = std::make_unique<OrderGateway<TestEgressClient>>(
        *client, &trading_enabled, &observer);

    // The venue receives the order but answers after the client's 3 s budget:
    // the classic ambiguous outcome.
    server.once("POST", "/order",
                testvenue::ResponseSpec{
                    200,
                    "{\"success\":true,\"errorMsg\":\"\",\"makingAmount\":\"4900000\","
                    "\"orderID\":\"0xresting\",\"status\":\"live\","
                    "\"takingAmount\":\"0\",\"tradeIDs\":[],\"transactionsHashes\":[]}",
                    testvenue::Fault::DELAY, 5000});
    gateway->start();
    const SubmitResult queued = gateway->submit(signed_order.body);
    CHECK(queued.ok && !queued.final, "order queued for egress");

    CHECK(wait_for([&] { return observer.outcomes() >= 1; }, 15000),
          "egress completed");
    CHECK(!trading_enabled.load(), "trading disabled after an ambiguous outcome");
    CHECK(gateway->ambiguous() == 1, "the outcome was classified ambiguous");
    CHECK(server.request_count("POST", "/order") == 1,
          "an ambiguous POST is never retried");

    const std::string ticket = observer.last_ticket();
    CHECK(ticket.compare(0, 2, "L:") == 0, "the submission ticket is keyed locally");
    ledger::OrderRecord ticket_record{};
    CHECK(ledger->order_copy(ticket.c_str(), ticket_record), "ticket recorded");
    CHECK(ticket_record.state == OrderState::UNKNOWN, "ticket is UNKNOWN");
    CHECK(ledger->blocking_orders() == 1, "UNKNOWN blocks new orders");
    CHECK(ledger->reserved_collateral() == signed_order.maker_amount,
          "worst-case cost stays reserved while UNKNOWN");

    // Reconciliation resolves it: the venue lists the resting order and its
    // fingerprint matches the ticket exactly.
    server.clear_rules();
    server.on("GET", "/data/orders",
              testvenue::ResponseSpec{
                  200,
                  orders_page({order_json("0xresting", "LIVE", "10", "0", "BUY",
                                          "0.49")}),
                  testvenue::Fault::NONE, 0});
    stub_healthy_account(server);
    clob::CurlTransport stable_transport;
    stable_transport.allow_loopback_plain_http(true);  // loopback test venue only
    clob::ClobApiClient stable_api(stable_transport, test_credentials(),
                                   server.http_base().c_str(),
                                   server.http_base().c_str());
    recon::Reconciler stable_reconciler(stable_api, *ledger, reconciliation_context());
    recon::Report report{};
    const bool first_ready = stable_reconciler.run_startup(report, nullptr);
    char first_reasons[512]{};
    report.format_reasons(first_reasons, sizeof(first_reasons));
    CHECK(first_ready, first_reasons);
    CHECK(report.tickets_linked == 1, "the ticket was linked to the venue order");
    ledger::OrderRecord venue_record{};
    CHECK(ledger->order_copy("0xresting", venue_record), "venue record exists");
    CHECK(venue_record.state == OrderState::LIVE, "venue order is LIVE");
    ledger::OrderRecord after{};
    CHECK(ledger->order_copy(ticket.c_str(), after), "ticket still present");
    CHECK(after.state == OrderState::SUPERSEDED, "ticket retired after linking");
    CHECK(ledger->blocking_orders() == 0, "nothing unknown remains");
    CHECK(report.readiness == recon::Readiness::READY, first_reasons);

    gateway->stop(false);
    client.reset();
    gateway.reset();
    ledger.reset();  // simulate a process restart

    // ── Restart with the order still open on the venue ───────────────────────
    auto restarted = std::make_unique<ledger::EventLedger>();
    CHECK(restarted->open(ledger_options(dir), error, sizeof(error)), error);
    CHECK(restarted->replayed_events() > 0 || restarted->applied_events() > 0,
          "the ledger recovered its history");
    ledger::OrderRecord recovered{};
    CHECK(restarted->order_copy("0xresting", recovered),
          "the open order survived the restart");
    CHECK(recovered.state == OrderState::LIVE, "and it is still LIVE");
    recon::Reconciler restart_reconciler(stable_api, *restarted,
                                         reconciliation_context());
    recon::Report restart_report{};
    const bool restart_ready = restart_reconciler.run_startup(restart_report, nullptr);
    char restart_reasons[512]{};
    restart_report.format_reasons(restart_reasons, sizeof(restart_reasons));
    CHECK(restart_ready, restart_reasons);
    CHECK(restart_report.adopted_orders == 0, "nothing had to be adopted");
    CHECK(restart_report.local_open_orders == 1 &&
              restart_report.venue_open_orders == 1,
          "local and venue views agree");

    // A second order appears on the venue that this process never sent.
    server.clear_rules();
    server.on("GET", "/data/orders",
              testvenue::ResponseSpec{
                  200,
                  orders_page({order_json("0xresting", "LIVE", "10", "0", "BUY",
                                          "0.49"),
                               order_json("0xforeign", "LIVE", "20", "0", "BUY",
                                          "0.48")}),
                  testvenue::Fault::NONE, 0});
    stub_healthy_account(server);
    recon::Report foreign_report{};
    CHECK(!restart_reconciler.run_startup(foreign_report, nullptr),
          "an unknown venue order blocks readiness");
    CHECK(foreign_report.adopted_orders == 1, "and is adopted into the ledger");
    char reasons[512]{};
    foreign_report.format_reasons(reasons, sizeof(reasons));
    CHECK(std::strstr(reasons, "venue_order_missing_locally") != nullptr, reasons);
    ledger::OrderRecord foreign{};
    CHECK(restarted->order_copy("0xforeign", foreign), "foreign order recorded");
    CHECK(foreign.externally_observed, "and flagged as externally observed");

    restarted.reset();
    server.stop();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== CROWDINTEL local-venue integration tests ==\n");
    test_user_channel_over_socket();
    test_heartbeat_over_socket();
    test_invalid_body_with_http_200();
    test_unknown_order_and_restart();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
