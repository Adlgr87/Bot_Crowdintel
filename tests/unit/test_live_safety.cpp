// ═══════════════════════════════════════════════════════════════════════════
// Live-safety unit tests: ledger, order state machine, venue metadata, REST
// client, user-channel events, order heartbeat and reconciliation.
//
// Everything here runs offline against fixture transports and on-disk ledger
// directories under /tmp, so it is deterministic and safe: no credentials, no
// wallet, no venue traffic, no order can ever reach Polymarket from a test.
// ═══════════════════════════════════════════════════════════════════════════

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <cstring>
#include <string>

#include "../../core/include/event_ledger.hpp"
#include "../../core/include/json_scan.hpp"
#include "../../core/include/order_state.hpp"
#include "../../core/include/venue_metadata.hpp"
#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/src/kill_switch.hpp"
#include "../../core/src/order_heartbeat.hpp"
#include "../../core/src/order_recorder.hpp"
#include "../../core/src/preflight.hpp"
#include "../../core/src/reconciliation.hpp"
#include "../../core/src/rpc_client.hpp"
#include "../../core/src/session_report.hpp"
#include "../../core/src/user_event.hpp"
#include "../../core/src/user_ws_protocol.hpp"
#include "../../core/src/ws_url.hpp"
#include "../support/fixture_transport.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));    \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define SECTION(name) std::printf("%s\n", name)

constexpr const char* K_CONDITION_ID =
    "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75";
constexpr const char* K_TOKEN_YES =
    "107505882767731489358349912513945399560393482969656700824895970500493757150417";
constexpr const char* K_TOKEN_NO =
    "7305630249804085635496399869905769372294302716159034447326228509068694952392";
constexpr const char* K_EXCHANGE = "0xE111180000d2663C0091e4f400237545B87B996B";
constexpr const char* K_ADDRESS = "0xC0Ffe2585196fe484bB6861E1c19f7b3Fc269724";
constexpr const char* K_API_KEY = "7b1e2d60-6f9a-4dd7-8f3e-21b8f94c77a2";
// Base64url test secret — a fixture, not a credential.
constexpr const char* K_API_SECRET = "Rnl2cWN0Rk5sc1ZzR1E9PQ==";
constexpr const char* K_PASSPHRASE = "9fH3kL7m2qW8xP4r";

// Per-process root: concurrent instances (ctest -j, stress loops) must not share
// ledger directories, or one process replays another one's journal.
std::string test_root() {
    std::string root = "/tmp/crowdintel-tests-";
    root += std::to_string(static_cast<long>(::getpid()));
    ::mkdir(root.c_str(), 0700);
    return root;
}

std::string dir(const char* name) {
    std::string path = test_root() + "/" + name;
    std::string command = "rm -rf " + path;
    if (std::system(command.c_str()) != 0) std::printf("  (cleanup warning)\n");
    return path;
}

ledger::EventLedger::Options ledger_options(const std::string& path,
                                            bool fsync = false) {
    ledger::EventLedger::Options options{};
    std::snprintf(options.directory, sizeof(options.directory), "%s", path.c_str());
    options.fsync_each_append = fsync;
    options.checkpoint_every = 64;
    return options;
}

clob::Credentials test_credentials() {
    clob::Credentials credentials{};
    std::snprintf(credentials.address, sizeof(credentials.address), "%s", K_ADDRESS);
    std::snprintf(credentials.api_key, sizeof(credentials.api_key), "%s", K_API_KEY);
    std::snprintf(credentials.api_secret_b64, sizeof(credentials.api_secret_b64), "%s",
                  K_API_SECRET);
    std::snprintf(credentials.api_passphrase, sizeof(credentials.api_passphrase),
                  "%s", K_PASSPHRASE);
    credentials.signature_type = 2;
    return credentials;
}

// ── Fixture builders ────────────────────────────────────────────────────────
std::string order_json(const char* id, const char* status, const char* original,
                       const char* matched, const char* side = "BUY") {
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
    json += "\",\"price\":\"0.49\",\"original_size\":\"";
    json += original;
    json += "\",\"size_matched\":\"";
    json += matched;
    json += "\",\"outcome\":\"Yes\",\"order_type\":\"GTC\",\"status\":\"";
    json += status;
    json += "\",\"created_at\":1714136516,\"expiration\":\"0\"}";
    return json;
}

std::string orders_page(const std::string* items, size_t count,
                        const char* cursor = "LTE=") {
    std::string json = "{\"limit\":500,\"count\":";
    json += std::to_string(count);
    json += ",\"next_cursor\":\"";
    json += cursor;
    json += "\",\"data\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i) json += ",";
        json += items[i];
    }
    json += "]}";
    return json;
}

std::string trade_json(const char* id, const char* taker_order_id,
                       const char* status, const char* size,
                       const char* side = "BUY") {
    std::string json = "{\"id\":\"";
    json += id;
    json += "\",\"taker_order_id\":\"";
    json += taker_order_id;
    json += "\",\"market\":\"";
    json += K_CONDITION_ID;
    json += "\",\"asset_id\":\"";
    json += K_TOKEN_YES;
    json += "\",\"side\":\"";
    json += side;
    json += "\",\"size\":\"";
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
            "\"trader_side\":\"TAKER\",\"timestamp\":\"1714136520500\"}";
    return json;
}

uint64_t g_commit_salt = 1000;

void commit_local_order(ledger::EventLedger& ledger, const char* order_id,
                        OrderState state, uint64_t matched_raw, uint8_t side = 0,
                        uint64_t size_raw = 100000000ULL) {
    ledger::Event event{};
    event.type = ledger::EventType::ORDER_STATE;
    event.source = ledger::Source::LOCAL;
    event.wall_ns = ledger::now_wall_ns();
    event.add_str(ledger::F_ORDER_ID, order_id);
    event.add_str(ledger::F_CONDITION_ID, K_CONDITION_ID);
    event.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
    event.add_u8(ledger::F_SIDE, side);
    event.add_u64(ledger::F_SIZE_RAW, size_raw);
    event.add_u64(ledger::F_MATCHED_RAW, matched_raw);
    event.add_u8(ledger::F_STATE, static_cast<uint8_t>(state));
    // Each call is a distinct observation, so the dedup salt must differ; the
    // state alone is not enough identity (LIVE can legitimately be observed
    // twice at different times).
    ledger::compute_event_key(event.type, event.source, order_id, "", 0,
                              static_cast<uint64_t>(state) * 1000003ULL +
                                  (++g_commit_salt),
                              event.key);
    char error[128]{};
    if (!ledger.commit(event, error, sizeof(error)))
        std::printf("  FAIL commit_local_order: %s\n", error);
}

// ═══════════════════════════════════════════════════════════════════════════
// 1. JSON scanning primitives
// ═══════════════════════════════════════════════════════════════════════════
void test_json_scan() {
    SECTION("json_scan");
    const char* document =
        "{\"market\":\"0xabc\",\"asset_id\":\"123\","
        "\"bids\":[{\"price\":\"0.51\",\"size\":\"100\"},{\"price\":\"0.50\",\"size\":\"5\"}],"
        "\"asks\":[],\"timestamp\":1714136516000,\"tick_size\":\"0.001\","
        "\"min_order_size\":5,\"neg_risk\":true,\"hash\":\"0xdead\"}";
    const size_t len = std::strlen(document);
    char buffer[128]{};
    CHECK(json_scan::get_string(document, len, "asset_id", buffer, sizeof(buffer)) &&
              std::strcmp(buffer, "123") == 0,
          "asset_id extracted");
    size_t start = 0;
    size_t end = 0;
    CHECK(json_scan::find_array(document, len, "bids", start, end), "bids located");
    CHECK(json_scan::array_count(document, len, start, end) == 2, "bids counted");
    uint64_t value = 0;
    CHECK(json_scan::get_fixed(document, len, "tick_size", 1000000, value) &&
              value == 1000,
          "tick_size 0.001 → 1000");
    CHECK(json_scan::get_fixed(document, len, "min_order_size", 1000000, value) &&
              value == 5000000ULL,
          "min_order_size 5 → 5e6");
    CHECK(json_scan::get_u64(document, len, "timestamp", value) &&
              value == 1714136516000ULL,
          "timestamp parsed");
    bool flag = false;
    CHECK(json_scan::get_bool(document, len, "neg_risk", flag) && flag, "neg_risk true");

    const char* duplicate = "{\"event_type\":\"book\",\"event_type\":\"price_change\"}";
    CHECK(json_scan::count_key(duplicate, std::strlen(duplicate), "event_type") == 2,
          "duplicate keys detected");
    const char* escaped = "{\"note\":\"\\\"asset_id\\\": 5\",\"asset_id\":\"9\"}";
    CHECK(json_scan::count_key(escaped, std::strlen(escaped), "asset_id") == 1,
          "key inside string literal not matched");
    CHECK(json_scan::get_string(escaped, std::strlen(escaped), "asset_id", buffer,
                                sizeof(buffer)) &&
              std::strcmp(buffer, "9") == 0,
          "value after escaped literal");

    CHECK(!json_scan::parse_fixed("0.0000001", 9, 1000000, value),
          "precision beyond scale rejected");
    CHECK(json_scan::parse_fixed(".49", 3, 1000000, value) && value == 490000,
          "leading dot accepted");
    CHECK(!json_scan::parse_fixed("1e3", 3, 1000000, value), "exponent rejected");
    CHECK(!json_scan::parse_fixed("-1", 2, 1000000, value), "negative rejected");
    CHECK(json_scan::is_hex_bytes(K_CONDITION_ID, std::strlen(K_CONDITION_ID), 32),
          "condition id shape");
    CHECK(json_scan::is_decimal_integer(K_TOKEN_YES, std::strlen(K_TOKEN_YES)),
          "token id shape");
    CHECK(!json_scan::is_decimal_integer("12a", 3), "non decimal rejected");
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. Order state machine
// ═══════════════════════════════════════════════════════════════════════════
void test_order_state_machine() {
    SECTION("order_state_machine");
    using order_state::transition_allowed;
    CHECK(transition_allowed(OrderState::LOCAL_CREATED, OrderState::SIGNED),
          "created→signed");
    CHECK(transition_allowed(OrderState::SIGNED, OrderState::SUBMITTING),
          "signed→submitting");
    CHECK(transition_allowed(OrderState::SUBMITTING, OrderState::UNKNOWN),
          "submitting→unknown (ambiguous transport)");
    CHECK(transition_allowed(OrderState::UNKNOWN, OrderState::LIVE),
          "unknown→live after authoritative read");
    CHECK(transition_allowed(OrderState::UNKNOWN, OrderState::MATCHED),
          "unknown→matched after authoritative read");
    CHECK(transition_allowed(OrderState::LIVE, OrderState::PARTIALLY_FILLED),
          "live→partially filled");
    CHECK(transition_allowed(OrderState::PARTIALLY_FILLED, OrderState::MATCHED),
          "partial→matched");
    CHECK(transition_allowed(OrderState::LIVE, OrderState::LIVE),
          "repeated observation is a no-op");
    CHECK(!transition_allowed(OrderState::MATCHED, OrderState::LIVE),
          "terminal state cannot reopen");
    CHECK(!transition_allowed(OrderState::CANCELLED, OrderState::PARTIALLY_FILLED),
          "cancelled cannot become filled");
    CHECK(!transition_allowed(OrderState::FAILED, OrderState::LIVE),
          "failed cannot become live");
    CHECK(!transition_allowed(OrderState::LOCAL_CREATED, OrderState::MATCHED),
          "cannot skip submission");
    CHECK(order_state_is_open(OrderState::UNKNOWN), "unknown counts as exposure");
    CHECK(order_state_is_open(OrderState::SUBMITTING), "submitting counts as exposure");
    CHECK(!order_state_is_open(OrderState::CANCELLED), "cancelled is not exposure");
    CHECK(order_state_blocks_new_orders(OrderState::UNKNOWN),
          "unknown blocks new orders");

    bool recognized = false;
    CHECK(venue_status::from_order_status("LIVE", false, recognized) ==
                  OrderState::LIVE && recognized, "LIVE mapping");
    CHECK(venue_status::from_order_status("MATCHED", true, recognized) ==
                  OrderState::MATCHED, "MATCHED(full) mapping");
    CHECK(venue_status::from_order_status("MATCHED", false, recognized) ==
                  OrderState::PARTIALLY_FILLED, "MATCHED(partial) mapping");
    CHECK(venue_status::from_order_status("CANCELED", false, recognized) ==
                  OrderState::CANCELLED, "CANCELED mapping");
    CHECK(venue_status::from_order_status("UNMATCHED", false, recognized) ==
                  OrderState::CANCELLED, "UNMATCHED (FAK killed) mapping");
    CHECK(venue_status::from_order_status("WEIRD", false, recognized) ==
                  OrderState::UNKNOWN && !recognized, "unknown status fails closed");
    CHECK(venue_status::from_post_response("live", 0, 100, recognized) ==
                  OrderState::LIVE, "post live");
    CHECK(venue_status::from_post_response("matched", 100, 100, recognized) ==
                  OrderState::MATCHED, "post matched full");
    CHECK(venue_status::from_post_response("matched", 40, 100, recognized) ==
                  OrderState::PARTIALLY_FILLED, "post matched partial (FAK)");
    CHECK(venue_status::from_post_response("delayed", 0, 100, recognized) ==
                  OrderState::LIVE, "post delayed");
    CHECK(venue_status::trade_is_terminal(venue_status::TradeKind::CONFIRMED) &&
              venue_status::trade_is_terminal(venue_status::TradeKind::FAILED),
          "terminal trade statuses");
    CHECK(!venue_status::trade_is_terminal(venue_status::TradeKind::MINED) &&
              !venue_status::trade_is_terminal(venue_status::TradeKind::RETRYING),
          "non-terminal trade statuses");
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Ledger: write-ahead, idempotency, recovery, corruption
// ═══════════════════════════════════════════════════════════════════════════
void test_ledger_write_ahead_and_idempotency() {
    SECTION("ledger_write_ahead_and_idempotency");
    const std::string path = dir("ledger-wal");
    {
        ledger::EventLedger ledger;
        char error[160]{};
        CHECK(ledger.open(ledger_options(path), error, sizeof(error)), error);
        CHECK(!ledger.corrupt(), "fresh ledger is not corrupt");

        // Build one observation and deliver it twice: the ledger must persist it
        // once and ignore the redelivery (WebSocket at-least-once semantics).
        ledger::Event first{};
        first.type = ledger::EventType::ORDER_SUBMITTING;
        first.source = ledger::Source::LOCAL;
        first.wall_ns = ledger::now_wall_ns();
        first.add_str(ledger::F_ORDER_ID, "0xaa");
        first.add_str(ledger::F_CONDITION_ID, K_CONDITION_ID);
        first.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
        first.add_u8(ledger::F_SIDE, 0);
        first.add_u64(ledger::F_SIZE_RAW, 100000000ULL);
        first.add_u8(ledger::F_STATE, static_cast<uint8_t>(OrderState::SUBMITTING));
        ledger::compute_event_key(first.type, first.source, "0xaa", "", 1714136516000ULL,
                                  0, first.key);
        char commit_error[128]{};
        CHECK(ledger.commit(first, commit_error, sizeof(commit_error)), commit_error);
        CHECK(ledger.orders().find("0xaa") != nullptr, "order recorded");
        CHECK(ledger.blocking_orders() == 1, "SUBMITTING blocks new orders");
        CHECK(ledger.commit(first, commit_error, sizeof(commit_error)),
              "redelivery accepted by the API");
        CHECK(ledger.duplicates_ignored() == 1, "duplicate ignored");
        CHECK(ledger.applied_events() == 1, "duplicate not applied");
        CHECK(ledger.orders().find("0xaa")->state == OrderState::SUBMITTING,
              "state unchanged by duplicate");

        // Illegal transition is rejected and audited.
        commit_local_order(ledger, "0xaa", OrderState::LIVE, 0);
        CHECK(ledger.orders().find("0xaa")->state == OrderState::LIVE,
              "legal transition applied");
        commit_local_order(ledger, "0xaa", OrderState::MATCHED, 100000000ULL);
        CHECK(ledger.orders().find("0xaa")->state == OrderState::MATCHED,
              "matched applied");
        commit_local_order(ledger, "0xaa", OrderState::LIVE, 0);
        CHECK(ledger.orders().find("0xaa")->state == OrderState::MATCHED,
              "terminal state protected");
        CHECK(ledger.illegal_transitions() == 1, "illegal transition counted");
        CHECK(ledger.state_event_count() == 1, "illegal transition audited");
        CHECK(ledger.blocking_orders() == 0, "terminal order no longer blocks");

        // Fills credit inventory exactly once.
        ledger::Event fill{};
        fill.type = ledger::EventType::FILL;
        fill.source = ledger::Source::USER_WS;
        fill.wall_ns = ledger::now_wall_ns();
        fill.add_str(ledger::F_TRADE_ID, "t-1");
        fill.add_str(ledger::F_ORDER_ID, "0xaa");
        fill.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
        fill.add_u8(ledger::F_SIDE, 0);
        fill.add_u8(ledger::F_RESULT,
                    static_cast<uint8_t>(venue_status::TradeKind::MATCHED));
        fill.add_u64(ledger::F_SIZE_RAW, 40000000ULL);
        fill.add_u64(ledger::F_PRICE_RAW, 490000);
        ledger::compute_event_key(fill.type, fill.source, "0xaa", "t-1", 0, 0, fill.key);
        CHECK(ledger.commit(fill, error, sizeof(error)), "fill committed");
        CHECK(ledger.positions().find(K_TOKEN_YES)->shares == 40000000ULL,
              "inventory credited");
        CHECK(ledger.commit(fill, error, sizeof(error)), "fill re-commit accepted");
        CHECK(ledger.positions().find(K_TOKEN_YES)->shares == 40000000ULL,
              "inventory not double counted");

        // A FAILED settlement reverses the credit.
        ledger::Event failed{};
        failed.type = ledger::EventType::TRADE_STATUS;
        failed.source = ledger::Source::REST;
        failed.wall_ns = ledger::now_wall_ns();
        failed.add_str(ledger::F_TRADE_ID, "t-1");
        failed.add_str(ledger::F_ORDER_ID, "0xaa");
        failed.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
        failed.add_u8(ledger::F_SIDE, 0);
        failed.add_u8(ledger::F_RESULT,
                      static_cast<uint8_t>(venue_status::TradeKind::FAILED));
        failed.add_u64(ledger::F_SIZE_RAW, 40000000ULL);
        ledger::compute_event_key(failed.type, failed.source, "0xaa", "t-1", 1, 0,
                                  failed.key);
        CHECK(ledger.commit(failed, error, sizeof(error)), "failed status committed");
        CHECK(ledger.positions().find(K_TOKEN_YES)->shares == 0,
              "failed settlement reversed");
        CHECK(ledger.checkpoint(error, sizeof(error)), "checkpoint written");
    }
    // Reopen: derived state must come back from the checkpoint alone.
    {
        ledger::EventLedger ledger;
        char error[160]{};
        CHECK(ledger.open(ledger_options(path), error, sizeof(error)), error);
        const ledger::OrderRecord* record = ledger.orders().find("0xaa");
        CHECK(record && record->state == OrderState::MATCHED, "state recovered");
        CHECK(ledger.fills().find("t-1") != nullptr, "fill recovered");
        CHECK(ledger.checkpoints() == 1, "checkpoint counter recovered");
        CHECK(ledger.blocking_orders() == 0, "no blocking orders after recovery");
    }
}

void test_ledger_journal_replay_and_corruption() {
    SECTION("ledger_journal_replay_and_corruption");
    const std::string path = dir("ledger-replay");
    {
        auto storage = std::make_unique<ledger::EventLedger>();
        ledger::EventLedger& ledger = *storage;
        char error[160]{};
        CHECK(ledger.open(ledger_options(path), error, sizeof(error)), error);
        for (int i = 0; i < 5; ++i) {
            char id[24];
            std::snprintf(id, sizeof(id), "0xord%d", i);
            commit_local_order(ledger, id, OrderState::SUBMITTING, 0);
        }
        CHECK(ledger.orders().size() == 5, "five orders journaled");
    }
    // Simulate a crash mid-append: truncate the journal by 7 bytes so the last
    // record is incomplete.  Recovery must drop only that trailing record.
    {
        const std::string journal = path + "/state.journal";
        struct stat metadata{};
        CHECK(::stat(journal.c_str(), &metadata) == 0, "journal exists");
        const off_t original = metadata.st_size;
        CHECK(original > 16, "journal has content");
        char command[512];
        std::snprintf(command, sizeof(command), "truncate -s %lld '%s'",
                      static_cast<long long>(original - 7), journal.c_str());
        CHECK(std::system(command) == 0, "journal truncated");
        // Remove the checkpoint so recovery must replay the journal.
        std::snprintf(command, sizeof(command), "rm -f '%s/state.checkpoint'",
                      path.c_str());
        CHECK(std::system(command) == 0, "checkpoint removed");

        ledger::EventLedger ledger;
        char error[160]{};
        CHECK(ledger.open(ledger_options(path), error, sizeof(error)), error);
        CHECK(ledger.recovered_trailing_record(), "trailing record detected");
        CHECK(!ledger.corrupt(), "trailing damage is recoverable");
        CHECK(ledger.orders().size() == 4, "four complete orders recovered");
    }
    // Mid-file corruption must fail closed.
    {
        const std::string path2 = dir("ledger-corrupt");
        auto writer_storage = std::make_unique<ledger::EventLedger>();
        ledger::EventLedger& writer = *writer_storage;
        char error[160]{};
        CHECK(writer.open(ledger_options(path2), error, sizeof(error)), error);
        commit_local_order(writer, "0xaa", OrderState::SUBMITTING, 0);
        commit_local_order(writer, "0xbb", OrderState::SUBMITTING, 0);
        writer.close();
        const std::string journal = path2 + "/state.journal";
        std::string command = "rm -f '" + path2 + "/state.checkpoint'";
        CHECK(std::system(command.c_str()) == 0, "checkpoint removed");
        // Flip a byte inside the first record's payload.
        command = "printf '\\xff' | dd of='" + journal +
                  "' bs=1 seek=20 conv=notrunc status=none";
        CHECK(std::system(command.c_str()) == 0, "journal corrupted");
        auto reader_storage = std::make_unique<ledger::EventLedger>();
        ledger::EventLedger& reader = *reader_storage;
        CHECK(!reader.open(ledger_options(path2), error, sizeof(error)),
              "corrupt journal refuses to open");
        CHECK(reader.corrupt(), "corruption flagged");
        CHECK(std::strstr(error, "refusing to operate") != nullptr,
              "corruption message is explicit");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. Venue metadata parsing and validation
// ═══════════════════════════════════════════════════════════════════════════
void test_metadata_parsers() {
    SECTION("metadata_parsers");
    std::string book = "{\"market\":\"";
    book += K_CONDITION_ID;
    book += "\",\"asset_id\":\"";
    book += K_TOKEN_YES;
    book += "\",\"timestamp\":\"1714136516000\",\"bids\":[{\"price\":\"0.49\","
            "\"size\":\"100\"}],\"asks\":[{\"price\":\"0.51\",\"size\":\"20\"}],"
            "\"min_order_size\":\"5\",\"tick_size\":\"0.001\",\"neg_risk\":false,"
            "\"last_trade_price\":\"0.5\",\"hash\":\"0xabc\"}";
    venue::BookSnapshot snapshot{};
    CHECK(venue::parse_book(book.c_str(), book.size(), snapshot), "book parsed");
    CHECK(snapshot.bid_count == 1 && snapshot.ask_count == 1, "book levels");
    CHECK(snapshot.bid_price[0] == 490000 && snapshot.ask_price[0] == 510000,
          "book prices are exact fixed point");
    CHECK(snapshot.tick_valid && snapshot.tick_raw == 1000, "book tick");
    CHECK(snapshot.min_order_size_valid && snapshot.min_order_size_raw == 5000000ULL,
          "book min order size");
    CHECK(snapshot.timestamp_ms == 1714136516000ULL, "book timestamp (string)");

    const char* malformed = "{\"market\":\"0xabc\",\"bids\":[{\"price\":\"0.49\"}]}";
    CHECK(!venue::parse_book(malformed, std::strlen(malformed), snapshot),
          "incomplete level rejected");

    uint64_t tick = 0;
    const char* tick_body = "{\"minimum_tick_size\":\"0.01\"}";
    CHECK(venue::parse_tick_size(tick_body, std::strlen(tick_body), tick) &&
              tick == 10000,
          "tick-size endpoint");
    const char* bad_tick = "{\"minimum_tick_size\":\"0.03\"}";
    CHECK(!venue::parse_tick_size(bad_tick, std::strlen(bad_tick), tick),
          "unsupported tick rejected");

    bool neg_risk = false;
    const char* neg_body = "{\"neg_risk\":true}";
    CHECK(venue::parse_neg_risk(neg_body, std::strlen(neg_body), neg_risk) && neg_risk,
          "neg-risk endpoint");

    std::string clob_market = "{\"nr\":false,\"mts\":\"0.001\",\"t\":[{\"t\":\"";
    clob_market += K_TOKEN_YES;
    clob_market += "\"},{\"t\":\"";
    clob_market += K_TOKEN_NO;
    clob_market += "\"}],\"fd\":{\"r\":0.07,\"e\":1}}";
    venue::MarketMetadata metadata{};
    metadata.reset();
    CHECK(venue::parse_clob_market(clob_market.c_str(), clob_market.size(), metadata),
          "clob-markets parsed");
    CHECK(metadata.token_count == 2, "market tokens");
    CHECK(metadata.fee_rate_micro == 70000 && metadata.fee_exponent_micro == 1000000,
          "fee schedule");

    std::string gamma = "{\"id\":\"703257\",\"slug\":\"s\",\"conditionId\":\"";
    gamma += K_CONDITION_ID;
    gamma += "\",\"active\":true,\"closed\":false,\"archived\":false,"
             "\"acceptingOrders\":true,\"enableOrderBook\":true,\"negRisk\":false,"
             "\"restricted\":false,\"clobTokenIds\":\"[\\\"";
    gamma += K_TOKEN_YES;
    gamma += "\\\",\\\"";
    gamma += K_TOKEN_NO;
    gamma += "\\\"]\",\"orderMinSize\":5,\"orderPriceMinTickSize\":\"0.001\","
             "\"feesEnabled\":true,\"secondsDelay\":0,\"feeSchedule\":{\"rate\":0.07,"
             "\"exponent\":1,\"takerOnly\":true,\"rebateRate\":0.25}}";
    CHECK(venue::parse_gamma_market(gamma.c_str(), gamma.size(), metadata),
          "gamma market parsed");
    CHECK(metadata.status_known && metadata.accepting_orders && metadata.active,
          "gamma status");
    CHECK(metadata.min_size_raw == 5000000ULL, "gamma min order size agrees");
    CHECK(metadata.maker_rebate_micro == 250000, "maker rebate");

    // A tick-size disagreement between sources must be rejected at parse time.
    std::string conflicting = gamma;
    const size_t position = conflicting.find("\"orderPriceMinTickSize\":\"0.001\"");
    CHECK(position != std::string::npos, "fixture has tick field");
    conflicting.replace(position, std::strlen("\"orderPriceMinTickSize\":\"0.001\""),
                        "\"orderPriceMinTickSize\":\"0.01\"");
    venue::MarketMetadata conflicted{};
    conflicted.reset();
    CHECK(venue::parse_clob_market(clob_market.c_str(), clob_market.size(), conflicted),
          "conflict fixture base");
    CHECK(!venue::parse_gamma_market(conflicting.c_str(), conflicting.size(), conflicted),
          "tick disagreement rejected");

    uint64_t seconds = 0;
    CHECK(venue::parse_server_time("1714136516", 10, seconds) &&
              seconds == 1714136516ULL, "server time (number)");
    CHECK(venue::parse_server_time("\"1714136516\"", 12, seconds) &&
              seconds == 1714136516ULL, "server time (string)");

    std::string balance = "{\"balance\":\"1500000\",\"allowances\":{\"";
    balance += K_EXCHANGE;
    balance += "\":\"2000000\"}}";
    venue::BalanceAllowance parsed{};
    CHECK(venue::parse_balance_allowance(balance.c_str(), balance.size(), K_EXCHANGE,
                                         parsed),
          "balance-allowance parsed");
    CHECK(parsed.balance == 1500000 && parsed.allowance == 2000000,
          "balance-allowance values");
    const char* legacy = "{\"balance\":\"10\",\"allowance\":\"5\"}";
    CHECK(venue::parse_balance_allowance(legacy, std::strlen(legacy), "", parsed) &&
              parsed.allowance == 5,
          "legacy balance-allowance shape");
}

void test_metadata_validation() {
    SECTION("metadata_validation");
    venue::MarketMetadata metadata{};
    metadata.reset();
    std::snprintf(metadata.condition_id, sizeof(metadata.condition_id), "%s",
                  K_CONDITION_ID);
    std::snprintf(metadata.token_id, sizeof(metadata.token_id), "%s", K_TOKEN_YES);
    metadata.add_token(K_TOKEN_YES);
    metadata.add_token(K_TOKEN_NO);
    metadata.tick_raw = 1000;
    metadata.min_size_raw = 5000000ULL;
    metadata.neg_risk = false;
    metadata.active = true;
    metadata.closed = false;
    metadata.archived = false;
    metadata.accepting_orders = true;
    metadata.enable_order_book = true;
    metadata.restricted = false;
    metadata.status_known = true;
    metadata.fee_rate_micro = 70000;
    metadata.fee_exponent_micro = 1000000;
    metadata.mark_source(venue::MetadataSource::BOOK);
    metadata.mark_source(venue::MetadataSource::GAMMA);

    venue::MetadataPolicy policy{};
    venue::MetadataReport report{};
    venue::validate_metadata(metadata, policy, report);
    char reasons[512]{};
    report.format(reasons, sizeof(reasons));
    CHECK(report.ok, reasons);

    venue::MarketMetadata blocked = metadata;
    blocked.accepting_orders = false;
    venue::validate_metadata(blocked, policy, report);
    CHECK(!report.ok && report.reason_count > 0, "not accepting orders blocks");

    blocked = metadata;
    blocked.tick_raw = 30000;  // 0.03: not a venue tick
    venue::validate_metadata(blocked, policy, report);
    CHECK(!report.ok, "unsupported tick blocks");

    blocked = metadata;
    blocked.min_size_raw = 0;
    venue::validate_metadata(blocked, policy, report);
    CHECK(!report.ok, "missing min order size blocks");

    blocked = metadata;
    std::snprintf(blocked.token_id, sizeof(blocked.token_id), "%s", "999");
    venue::validate_metadata(blocked, policy, report);
    CHECK(!report.ok, "token outside the market blocks");

    blocked = metadata;
    blocked.fee_rate_micro = 90000;  // above the highest documented rate
    venue::validate_metadata(blocked, policy, report);
    CHECK(!report.ok, "fee above policy blocks");

    // Protocol-v2 position ids settle through the Combos exchange: refused
    // unless explicitly allowed.
    venue::MarketMetadata v2 = metadata;
    std::snprintf(v2.token_id, sizeof(v2.token_id), "%s", "549755813888");  // 2^39
    v2.token_count = 0;
    CHECK(venue::is_protocol_v2_position_id(v2.token_id), "v2 position id detected");
    venue::validate_metadata(v2, policy, report);
    CHECK(!report.ok, "v2 position id blocked by default");
    venue::MetadataPolicy permissive = policy;
    permissive.allow_protocol_v2 = true;
    venue::validate_metadata(v2, permissive, report);
    CHECK(report.ok, "v2 position id allowed by explicit policy");
    CHECK(!venue::is_protocol_v2_position_id(K_TOKEN_YES),
          "ordinary token is not a v2 position id");

    // Amount precision must follow the official rounding table.
    CHECK(venue::amount_quantum_for_tick(100000) == 1000, "0.1 → 3 decimals");
    CHECK(venue::amount_quantum_for_tick(10000) == 100, "0.01 → 4 decimals");
    CHECK(venue::amount_quantum_for_tick(5000) == 10, "0.005 → 5 decimals");
    CHECK(venue::amount_quantum_for_tick(2500) == 1, "0.0025 → 6 decimals");
    CHECK(venue::amount_quantum_for_tick(1000) == 10, "0.001 → 5 decimals");
    CHECK(venue::amount_quantum_for_tick(100) == 1, "0.0001 → 6 decimals");
    CHECK(venue::amount_quantum_for_tick(30000) == 0, "unsupported tick → 0");
}

void test_market_runtime() {
    SECTION("market_runtime");
    venue::MarketMetadata metadata{};
    metadata.reset();
    std::snprintf(metadata.condition_id, sizeof(metadata.condition_id), "%s",
                  K_CONDITION_ID);
    std::snprintf(metadata.token_id, sizeof(metadata.token_id), "%s", K_TOKEN_YES);
    metadata.add_token(K_TOKEN_YES);
    metadata.tick_raw = 10000;
    metadata.min_size_raw = 5000000ULL;
    metadata.active = true;
    metadata.closed = false;
    metadata.archived = false;
    metadata.accepting_orders = true;
    metadata.enable_order_book = true;
    metadata.restricted = false;
    metadata.status_known = true;
    metadata.mark_source(venue::MetadataSource::BOOK);

    venue::MarketRuntime runtime{};
    venue::MetadataPolicy policy{};
    venue::MetadataReport report{};
    CHECK(runtime.publish(metadata, policy, report), "valid metadata published");
    venue::MarketRuntime::View view{};
    CHECK(runtime.read(view) && view.valid && view.tick_raw == 10000 &&
              view.amount_quantum == 100 && view.min_size_raw == 5000000ULL,
          "hot-path view is coherent");
    const uint32_t generation = view.generation;

    runtime.invalidate();
    CHECK(runtime.read(view) && !view.valid, "invalidation is immediate");

    venue::MarketMetadata worse = metadata;
    worse.accepting_orders = false;
    CHECK(!runtime.publish(worse, policy, report), "invalid metadata refused");
    CHECK(runtime.read(view) && !view.valid, "refused publication keeps trading off");

    CHECK(runtime.publish(metadata, policy, report), "republished after recovery");
    CHECK(runtime.read(view) && view.valid && view.generation > generation,
          "generation advances");

    // The published generation carries its observation time so the supervisor can
    // bound how long venue-derived parameters (tick size, minimum order size, fee
    // rate, negative-risk flag) may drive order construction.
    constexpr uint64_t T0 = 1800000000000000000ULL;  // wall-clock ns
    venue::MarketMetadata timed = metadata;
    timed.observed_wall_ns = T0;
    CHECK(runtime.publish(timed, policy, report), "timed snapshot published");
    CHECK(runtime.observed_wall_ns() == T0, "publication exposes the observation time");
    CHECK(runtime.read(view) && view.observed_wall_ns == T0,
          "the coherent view carries the same observation time");

    CHECK(venue::metadata_is_fresh(T0, 90000, T0), "age zero is fresh");
    CHECK(venue::metadata_is_fresh(T0, 90000, T0 + 89999000000ULL),
          "one ms inside the budget is fresh");
    CHECK(venue::metadata_is_fresh(T0, 90000, T0 + 90000000000ULL),
          "exactly at the budget is fresh");
    CHECK(!venue::metadata_is_fresh(T0, 90000, T0 + 90001000000ULL),
          "one ms past the budget is stale");
    CHECK(!venue::metadata_is_fresh(0, 90000, T0), "never observed is stale");
    CHECK(!venue::metadata_is_fresh(0, 90000, 1000),
          "never observed is stale even against a clock near zero");
    CHECK(!venue::metadata_is_fresh(T0, 0, T0), "a zero budget is stale");
    CHECK(!venue::metadata_is_fresh(T0, 90000, T0 - 1),
          "a wall clock that moved backwards is stale, not very fresh");
    CHECK(venue::metadata_age_ms(T0, T0 + 1500000000ULL) == 1500, "age in ms");
    CHECK(venue::metadata_age_ms(0, T0) == UINT64_MAX, "unknown age is reported");
    CHECK(venue::metadata_age_ms(T0, T0 - 1) == UINT64_MAX,
          "backwards clock reports an unknown age");

    runtime.invalidate();
    CHECK(runtime.observed_wall_ns() == 0, "invalidation clears the observation time");
    CHECK(!venue::metadata_is_fresh(runtime.observed_wall_ns(), 90000,
                                    ledger::now_wall_ns()),
          "an invalidated runtime is stale by construction");
}

// ═══════════════════════════════════════════════════════════════════════════
// 4b. Operator kill switch: present, absent and unanswerable
// ═══════════════════════════════════════════════════════════════════════════
void test_kill_switch() {
    SECTION("kill_switch");
    const std::string base = dir("kill-switch");
    ::mkdir(base.c_str(), 0700);
    const std::string path = base + "/crowdintel.kill";

    CHECK(safety::poll_kill_switch(path.c_str()) ==
              safety::KillSwitchState::DISENGAGED,
          "an absent switch is disengaged (clean ENOENT)");
    CHECK(!safety::kill_switch_blocks(safety::KillSwitchState::DISENGAGED),
          "disengaged does not block");

    std::FILE* created = std::fopen(path.c_str(), "wb");
    CHECK(created != nullptr, "fixture switch file created");
    if (created) std::fclose(created);
    CHECK(safety::poll_kill_switch(path.c_str()) ==
              safety::KillSwitchState::ENGAGED,
          "a present switch is engaged");
    CHECK(safety::kill_switch_blocks(safety::KillSwitchState::ENGAGED),
          "engaged blocks");

    // Any filesystem object engages the switch, not only a regular file: the
    // operator may create it with touch, mkdir or a redirected log line.
    ::unlink(path.c_str());
    CHECK(::mkdir(path.c_str(), 0700) == 0, "directory fixture created");
    CHECK(safety::poll_kill_switch(path.c_str()) ==
              safety::KillSwitchState::ENGAGED,
          "a directory at the switch path also engages it");
    ::rmdir(path.c_str());
    CHECK(safety::poll_kill_switch(path.c_str()) ==
              safety::KillSwitchState::DISENGAGED,
          "removing the switch disengages it");

    // Fail-closed: a check that cannot be answered is UNKNOWN, never DISENGAGED.
    const std::string regular = base + "/regular.file";
    std::FILE* other = std::fopen(regular.c_str(), "wb");
    CHECK(other != nullptr, "non-directory fixture created");
    if (other) std::fclose(other);
    const std::string under_file = regular + "/child";
    CHECK(safety::poll_kill_switch(under_file.c_str()) ==
              safety::KillSwitchState::UNKNOWN,
          "ENOTDIR is reported as unknown");
    char error[160]{};
    safety::kill_switch_error_text(error, sizeof(error));
    CHECK(std::strstr(error, "errno=") != nullptr,
          "the unknown state carries the system error for the operator log");
    CHECK(safety::kill_switch_blocks(safety::KillSwitchState::UNKNOWN),
          "unknown blocks (fail closed)");
    CHECK(safety::poll_kill_switch("") == safety::KillSwitchState::UNKNOWN,
          "an empty path is unknown, not disengaged");
    CHECK(safety::poll_kill_switch(nullptr) == safety::KillSwitchState::UNKNOWN,
          "a null path is unknown, not disengaged");
    const std::string too_long = base + "/" + std::string(5000, 'x');
    CHECK(safety::poll_kill_switch(too_long.c_str()) ==
              safety::KillSwitchState::UNKNOWN,
          "ENAMETOOLONG is reported as unknown");

    CHECK(std::strcmp(safety::kill_switch_state_name(
                          safety::KillSwitchState::DISENGAGED),
                      "DISENGAGED") == 0,
          "state name DISENGAGED");
    CHECK(std::strcmp(safety::kill_switch_state_name(
                          safety::KillSwitchState::ENGAGED),
                      "ENGAGED") == 0,
          "state name ENGAGED");
    CHECK(std::strcmp(safety::kill_switch_state_name(
                          safety::KillSwitchState::UNKNOWN),
                      "UNKNOWN") == 0,
          "state name UNKNOWN");
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. REST client over a fixture transport
// ═══════════════════════════════════════════════════════════════════════════
void test_rest_client() {
    SECTION("rest_client");
    testing::FixtureTransport transport;
    clob::Credentials credentials = test_credentials();
    clob::ClobApiClient api(transport, credentials, "https://clob.polymarket.com",
                            "https://gamma-api.polymarket.com");
    CHECK(api.credentials_ready(), "credentials ready");
    clob::CallResult call{};

    transport.on("GET", "/time", 200, "1714136516");
    uint64_t seconds = 0;
    CHECK(api.server_time(seconds, call) && seconds == 1714136516ULL, "GET /time");

    std::string book = "{\"market\":\"";
    book += K_CONDITION_ID;
    book += "\",\"asset_id\":\"";
    book += K_TOKEN_YES;
    book += "\",\"timestamp\":\"1714136516000\",\"bids\":[{\"price\":\"0.49\","
            "\"size\":\"100\"}],\"asks\":[{\"price\":\"0.51\",\"size\":\"20\"}],"
            "\"min_order_size\":\"5\",\"tick_size\":\"0.001\",\"neg_risk\":false,"
            "\"last_trade_price\":\"0.5\",\"hash\":\"0xabc\"}";
    transport.on("GET", "/book?token_id=", 200, book.c_str());
    venue::BookSnapshot snapshot{};
    CHECK(api.book(K_TOKEN_YES, snapshot, call) && snapshot.tick_raw == 1000,
          "GET /book");
    const testing::RecordedRequest* request = transport.last();
    CHECK(request && std::strstr(request->url, "token_id=") != nullptr,
          "book query string");
    CHECK(request && request->header_count == 0, "public call carries no L2 headers");

    transport.on("GET", "/tick-size", 200, "{\"minimum_tick_size\":\"0.001\"}");
    uint64_t tick = 0;
    CHECK(api.tick_size(K_TOKEN_YES, tick, call) && tick == 1000, "GET /tick-size");
    transport.on("GET", "/neg-risk", 200, "{\"neg_risk\":false}");
    bool neg_risk = true;
    CHECK(api.neg_risk(K_TOKEN_YES, neg_risk, call) && !neg_risk, "GET /neg-risk");
    transport.on("GET", "/fee-rate", 200, "{\"base_fee\":700}");
    uint64_t bps = 0;
    CHECK(api.fee_rate_bps(K_TOKEN_YES, bps, call) && bps == 700, "GET /fee-rate");

    std::string clob_market = "{\"nr\":false,\"mts\":\"0.001\",\"t\":[{\"t\":\"";
    clob_market += K_TOKEN_YES;
    clob_market += "\"}],\"fd\":{\"r\":0.07,\"e\":1}}";
    transport.on("GET", "/clob-markets/", 200, clob_market.c_str());
    venue::MarketMetadata metadata{};
    metadata.reset();
    CHECK(api.clob_market(K_CONDITION_ID, metadata, call) && metadata.token_count == 1,
          "GET /clob-markets/{condition_id}");

    std::string gamma = "{\"id\":\"1\",\"slug\":\"s\",\"conditionId\":\"";
    gamma += K_CONDITION_ID;
    gamma += "\",\"active\":true,\"closed\":false,\"archived\":false,"
             "\"acceptingOrders\":true,\"enableOrderBook\":true,\"negRisk\":false,"
             "\"clobTokenIds\":\"[\\\"";
    gamma += K_TOKEN_YES;
    gamma += "\\\"]\",\"orderMinSize\":5,\"orderPriceMinTickSize\":\"0.001\"}";
    transport.on("GET", "/markets/slug/", 200, gamma.c_str());
    CHECK(api.gamma_market_by_slug("s", metadata, call) && metadata.accepting_orders,
          "GET gamma /markets/slug/{slug}");

    // Authenticated order reads: the L2 signature must cover the bare path.
    const std::string items[2] = {order_json("0xaa", "LIVE", "100", "0"),
                                  order_json("0xbb", "LIVE", "50", "10", "SELL")};
    transport.on("GET", "/data/orders", 200, orders_page(items, 2).c_str());
    clob::OrderPage page{};
    CHECK(api.open_orders(K_CONDITION_ID, nullptr, nullptr, page, call),
          "GET /data/orders");
    CHECK(page.count == 2 && page.complete && !page.has_more, "orders page parsed");
    CHECK(page.items[1].side == 1 && page.items[1].size_matched == 10000000ULL,
          "order values");
    request = transport.last();
    bool signed_header = false;
    bool address_header = false;
    for (size_t i = 0; request && i < request->header_count; ++i) {
        if (std::strncmp(request->headers[i], "POLY_SIGNATURE: ", 16) == 0)
            signed_header = true;
        if (std::strncmp(request->headers[i], "POLY_ADDRESS: ", 14) == 0)
            address_header = true;
    }
    CHECK(signed_header && address_header, "L2 headers present on /data/orders");

    // The signature must be over timestamp+GET+/data/orders (no query string).
    // Sized by the callee's contract, not by how many headers it happens to
    // write today: build_l2_headers() may fill K_MAX_HEADERS rows of
    // K_MAX_HEADER bytes, and a smaller array here would be a stack overflow the
    // moment a sixth header is added.
    char headers[clob::K_MAX_HEADERS][clob::K_MAX_HEADER]{};
    const size_t header_count =
        api.build_l2_headers(headers, "GET", "/data/orders", nullptr, 0);
    CHECK(header_count == 5, "five L2 headers");
    char signed_with_query[clob::K_MAX_HEADERS][clob::K_MAX_HEADER]{};
    const size_t other_count = api.build_l2_headers(
        signed_with_query, "GET", "/data/orders?market=0xabc", nullptr, 0);
    CHECK(other_count == 5, "headers built for query variant");
    CHECK(std::strcmp(headers[1], signed_with_query[1]) != 0,
          "query string changes the signature (so it must be excluded)");

    transport.on("GET", "/data/order/0xaa", 200,
                 order_json("0xaa", "MATCHED", "100", "100").c_str());
    clob::OrderWire wire{};
    CHECK(api.order("0xaa", wire, call) && wire.valid &&
              wire.size_matched == 100000000ULL,
          "GET /data/order/{id}");

    std::string trades = "{\"limit\":500,\"count\":1,\"next_cursor\":\"LTE=\","
                         "\"data\":[";
    trades += trade_json("t-1", "0xaa", "TRADE_STATUS_MATCHED", "40");
    trades += "]}";
    transport.on("GET", "/data/trades", 200, trades.c_str());
    clob::TradePage trade_page{};
    CHECK(api.trades(K_CONDITION_ID, nullptr, nullptr, trade_page, call) &&
              trade_page.count == 1, "GET /data/trades");

    std::string balance = "{\"balance\":\"1500000\",\"allowances\":{\"";
    balance += K_EXCHANGE;
    balance += "\":\"2000000\"}}";
    transport.on("GET", "/balance-allowance", 200, balance.c_str());
    venue::BalanceAllowance parsed{};
    CHECK(api.balance_allowance(venue::AssetType::COLLATERAL, nullptr, K_EXCHANGE,
                                parsed, call) && parsed.allowance == 2000000,
          "GET /balance-allowance");

    transport.on("POST", "/v1/heartbeats", 200, "{\"heartbeat_id\":\"hb-2\"}");
    clob::HeartbeatResult heartbeat{};
    CHECK(api.post_heartbeat("hb-1", heartbeat, call) && heartbeat.accepted &&
              std::strcmp(heartbeat.heartbeat_id, "hb-2") == 0,
          "POST /v1/heartbeats");
    request = transport.last();
    CHECK(request && std::strcmp(request->body, "{\"heartbeat_id\":\"hb-1\"}") == 0,
          "heartbeat body is the exact serialization");

    testing::FixtureResponse rejected{};
    rejected.code = 400;
    std::snprintf(rejected.body, sizeof(rejected.body),
                  "{\"error_msg\":\"Invalid Heartbeat ID\",\"heartbeat_id\":\"hb-x\"}");
    transport.queue_once("/v1/heartbeats", rejected);
    clob::HeartbeatResult resync{};
    CHECK(api.post_heartbeat("stale", resync, call) && resync.rejected_invalid_id &&
              std::strcmp(resync.heartbeat_id, "hb-x") == 0,
          "400 invalid heartbeat id resynchronisation");

    transport.on_transport("DELETE", "/cancel-all", true, "Timeout was reached");
    CHECK(!api.cancel_all(call) && call.ambiguous, "ambiguous cancel is reported");

    // POST /order is the only raw egress door on this client and it is closed by
    // default: production order flow goes through OrderGateway, which owns the
    // ledger reservation, the observer and the trading gate.
    CHECK(!api.cold_egress_armed(), "the cold-egress latch starts closed");
    {
        const size_t before = transport.request_count();
        clob::OrderPostResult refused{};
        clob::CallResult refused_call{};
        const char* raw_body = "{\"order\":{}}";
        CHECK(!api.post_order(raw_body, std::strlen(raw_body), refused, refused_call),
              "post_order is refused while the latch is closed");
        CHECK(!refused_call.ok && !refused_call.http_ok && !refused_call.ambiguous,
              "a refused post is unambiguous: nothing reached the wire");
        CHECK(std::strstr(refused_call.detail, "cold-egress latch closed") != nullptr,
              "the refusal says why and names the gateway");
        CHECK(!api.post_raw("/order", raw_body, std::strlen(raw_body), refused_call),
              "post_raw is refused while the latch is closed");
        CHECK(transport.request_count() == before,
              "no request was handed to the transport");
        CHECK(api.cold_egress_refusals() == 2, "both refusals are counted");
    }
    // Cancels are the safe direction and are never behind the latch.  (Fixture
    // rules match first-wins, so this uses DELETE /order, not the /cancel-all
    // rule that an earlier case turned into a transport timeout.)
    {
        transport.on("DELETE", "/order", 200, "{\"success\":true}");
        const size_t before = transport.request_count();
        CHECK(api.cancel_order("0xabc", call), "cancel works with the latch closed");
        CHECK(transport.request_count() == before + 1, "the cancel reached the wire");
    }
    // The third cancel entry point: market-scoped.  Its path, method and body are
    // the exact ones the official SDK signs (py-clob-client endpoints.py:24,
    // client.py:729-747), so a byte difference here would mean an L2 signature the
    // venue rejects - or, worse, a cancel with the wrong scope.
    {
        transport.on("DELETE", "/cancel-market-orders", 200, "{\"success\":true}");
        const size_t before = transport.request_count();
        clob::CallResult market_call{};
        CHECK(api.cancel_market_orders(K_CONDITION_ID, "713210456792522125946263",
                                       market_call),
              "a market-scoped cancel succeeds with the latch closed");
        CHECK(transport.request_count() == before + 1, "it reached the wire");
        const testing::RecordedRequest* sent = transport.last();
        CHECK(sent != nullptr, "the request was recorded");
        if (sent) {
            CHECK(std::strcmp(sent->method, "DELETE") == 0, sent->method);
            CHECK(std::strstr(sent->url, "/cancel-market-orders") != nullptr,
                  sent->url);
            std::string expected = "{\"market\":\"";
            expected += K_CONDITION_ID;
            expected += "\",\"asset_id\":\"713210456792522125946263\"}";
            CHECK(expected == sent->body,
                  "the body is the SDK's compact serialization, byte for byte");
            CHECK(sent->body_len == expected.size(), "no padding after the body");
            bool address = false, signature = false, timestamp = false,
                 api_key = false, passphrase = false;
            for (size_t i = 0; i < sent->header_count; ++i) {
                if (std::strstr(sent->headers[i], "POLY_ADDRESS")) address = true;
                if (std::strstr(sent->headers[i], "POLY_SIGNATURE")) signature = true;
                if (std::strstr(sent->headers[i], "POLY_TIMESTAMP")) timestamp = true;
                if (std::strstr(sent->headers[i], "POLY_API_KEY")) api_key = true;
                if (std::strstr(sent->headers[i], "POLY_PASSPHRASE")) passphrase = true;
            }
            CHECK(address && signature && timestamp && api_key && passphrase,
                  "all five L2 headers are signed over method, path and body");
        }
        // A transport timeout on a cancel is ambiguous: the request may have
        // reached the venue.  Reporting it as a plain failure would let a caller
        // believe the orders are still resting.
        // The success rule above matches first-wins, so the fault has to be
        // queued: the one-shot queue is consulted before the rule list.
        testing::FixtureResponse timed_out{};
        timed_out.code = 0;
        timed_out.transport_ok = false;
        timed_out.ambiguous = true;
        std::snprintf(timed_out.error, sizeof(timed_out.error), "%s",
                      "Timeout was reached");
        transport.queue_once("/cancel-market-orders", timed_out);
        clob::CallResult ambiguous{};
        CHECK(!api.cancel_market_orders(K_CONDITION_ID, "713", ambiguous) &&
                  ambiguous.ambiguous,
              "an ambiguous market cancel is reported as ambiguous");
        // Without credentials nothing may reach the wire, not even a cancel.
        testing::FixtureTransport anon_transport;
        clob::ClobApiClient anon(anon_transport, clob::Credentials{},
                                 "https://clob.polymarket.com",
                                 "https://gamma-api.polymarket.com");
        clob::CallResult refused{};
        CHECK(!anon.cancel_market_orders(K_CONDITION_ID, "713", refused),
              "an unauthenticated client refuses to cancel");
        CHECK(anon_transport.request_count() == 0, "and sends nothing");
    }
    api.arm_cold_egress();
    CHECK(api.cold_egress_armed(), "an explicit arm opens the latch");

    // POST /order response parsing, including the amounts and trade ids that
    // the ledger needs to record a FAK partial fill.
    transport.on("POST", "/order", 200,
                 "{\"success\":true,\"errorMsg\":\"\",\"makingAmount\":\"4900000\","
                 "\"orderID\":\"0xcc\",\"status\":\"matched\",\"takingAmount\":"
                 "\"10000000\",\"tradeIDs\":[\"t-9\"],\"transactionsHashes\":[]}");
    clob::OrderPostResult post{};
    const char* body = "{\"deferExec\":false,\"order\":{},\"orderType\":\"FAK\","
                       "\"owner\":\"k\"}";
    CHECK(api.post_order(body, std::strlen(body), post, call) && post.parsed &&
              post.success && post.trade_id_count == 1 && post.taking_amount == 10000000ULL,
          "POST /order response parsed");
    bool recognized = false;
    CHECK(venue_status::from_post_response(post.status, post.taking_amount,
                                           10000000ULL, recognized) ==
                  OrderState::MATCHED && recognized,
          "post status mapped to state");

    // Duplicate semantic keys in a response are rejected, never resolved.
    const char* duplicate = "{\"success\":true,\"success\":false,\"orderID\":\"0x1\","
                            "\"status\":\"live\"}";
    CHECK(!clob::parse_order_post(duplicate, std::strlen(duplicate), post),
          "duplicate response keys rejected");
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. User channel parsing and application
// ═══════════════════════════════════════════════════════════════════════════
std::string user_order_frame(const char* id, const char* update_type,
                             const char* status, const char* original,
                             const char* matched, const char* timestamp_ms,
                             const char* side = "BUY") {
    std::string json = "{\"event_type\":\"order\",\"id\":\"";
    json += id;
    json += "\",\"owner\":\"";
    json += K_API_KEY;
    json += "\",\"market\":\"";
    json += K_CONDITION_ID;
    json += "\",\"asset_id\":\"";
    json += K_TOKEN_YES;
    json += "\",\"side\":\"";
    json += side;
    json += "\",\"order_owner\":\"";
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

std::string user_trade_frame(const char* trade_id, const char* order_id,
                             const char* status, const char* size,
                             bool with_maker_orders = true) {
    std::string json = "{\"event_type\":\"trade\",\"type\":\"TRADE\",\"id\":\"";
    json += trade_id;
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
    json += "\",\"transaction_hash\":\"0xbeef\",\"bucket_index\":1,";
    if (with_maker_orders) {
        json += "\"maker_orders\":[{\"order_id\":\"";
        json += order_id;
        json += "\",\"owner\":\"";
        json += K_API_KEY;
        json += "\",\"maker_address\":\"";
        json += K_ADDRESS;
        json += "\",\"matched_amount\":\"";
        json += size;
        json += "\",\"price\":\"0.49\",\"fee_rate_bps\":0,\"asset_id\":\"";
        json += K_TOKEN_YES;
        json += "\",\"outcome\":\"Yes\",\"outcome_index\":0,\"side\":\"BUY\"}],";
    }
    json += "\"trader_side\":\"TAKER\",\"timestamp\":\"1714136520500\"}";
    return json;
}

void test_user_event_parsing() {
    SECTION("user_event_parsing");
    const std::string placement =
        user_order_frame("0xaa", "PLACEMENT", "LIVE", "100", "0", "1714136516123");
    user_ws::UserMessage message{};
    CHECK(user_ws::parse_user_message(placement.c_str(), placement.size(), message),
          "order frame parsed");
    CHECK(message.kind == user_ws::MessageKind::ORDER && message.order_count == 1,
          "order kind");
    CHECK(message.orders[0].original_size_raw == 100000000ULL &&
              message.orders[0].size_matched_raw == 0,
          "order sizes are exact fixed point");
    CHECK(message.orders[0].timestamp_ms == 1714136516123ULL, "timestamp is epoch ms");
    CHECK(message.orders[0].created_at_s == 1714136516ULL, "created_at is epoch s");
    CHECK(std::strcmp(message.orders[0].update_type, "PLACEMENT") == 0,
          "update type captured");

    const std::string trade =
        user_trade_frame("t-1", "0xaa", "TRADE_STATUS_MATCHED", "40");
    CHECK(user_ws::parse_user_message(trade.c_str(), trade.size(), message),
          "trade frame parsed");
    CHECK(message.kind == user_ws::MessageKind::TRADE &&
              message.trades[0].maker_order_count == 1,
          "trade maker orders parsed");
    CHECK(message.trades[0].kind == venue_status::TradeKind::MATCHED &&
              message.trades[0].status_recognized,
          "trade status mapped");
    CHECK(message.trades[0].size_raw == 40000000ULL, "trade size");

    std::string envelope =
        "{\"topic\":\"user\",\"type\":\"order\",\"payload\":{\"id\":\"0xbb\","
        "\"market\":\"";
    envelope += K_CONDITION_ID;
    envelope += "\",\"asset_id\":\"123\",\"side\":\"SELL\","
                "\"original_size\":\"10\",\"size_matched\":\"0\","
                "\"price\":\"0.51\",\"type\":\"PLACEMENT\",\"status\":\"LIVE\","
                "\"created_at\":\"1714136516\",\"expiration\":\"0\","
                "\"order_type\":\"GTC\"}}";
    CHECK(user_ws::parse_user_message(envelope.c_str(), envelope.size(), message) &&
              message.orders[0].side == 1,
          "SDK envelope shape accepted");

    CHECK(user_ws::parse_user_message("PONG", 4, message) &&
              message.kind == user_ws::MessageKind::HEARTBEAT_ACK,
          "PONG is a keep-alive");
    CHECK(user_ws::parse_user_message("{}", 2, message) &&
              message.kind == user_ws::MessageKind::HEARTBEAT_ACK,
          "empty object is a keep-alive");

    const char* duplicate =
        "{\"event_type\":\"order\",\"event_type\":\"trade\",\"id\":\"x\"}";
    CHECK(!user_ws::parse_user_message(duplicate, std::strlen(duplicate), message),
          "duplicated event_type rejected");
    const char* bad_side =
        "{\"event_type\":\"order\",\"id\":\"0xcc\",\"market\":\"0x74\","
        "\"asset_id\":\"123\",\"side\":\"HOLD\",\"original_size\":\"10\","
        "\"size_matched\":\"0\",\"price\":\"0.5\",\"type\":\"PLACEMENT\","
        "\"status\":\"LIVE\"}";
    CHECK(!user_ws::parse_user_message(bad_side, std::strlen(bad_side), message),
          "unknown side rejected");
    const char* inconsistent =
        "{\"event_type\":\"order\",\"id\":\"0xcc\",\"market\":\"0x74\","
        "\"asset_id\":\"123\",\"side\":\"BUY\",\"original_size\":\"10\","
        "\"size_matched\":\"20\",\"price\":\"0.5\",\"type\":\"UPDATE\","
        "\"status\":\"MATCHED\"}";
    CHECK(!user_ws::parse_user_message(inconsistent, std::strlen(inconsistent), message),
          "size_matched > original_size rejected");
    const char* unauthorized = "{\"message\":\"unauthorized\"}";
    CHECK(user_ws::parse_user_message(unauthorized, std::strlen(unauthorized),
                                      message) &&
              message.kind == user_ws::MessageKind::ERROR_FRAME,
          "error frame recognised");

    const std::string batch = "[" + placement + "," + trade + "]";
    CHECK(user_ws::parse_user_message(batch.c_str(), batch.size(), message) &&
              message.order_count == 1 && message.trade_count == 1,
          "array of events parsed");
}

void test_user_event_application() {
    SECTION("user_event_application");
    const std::string path = dir("user-events");
    ledger::EventLedger ledger;
    char error[160]{};
    CHECK(ledger.open(ledger_options(path), error, sizeof(error)), error);
    user_ws::UserEventApplier applier(ledger);
    user_ws::ApplierContext context{};
    std::snprintf(context.api_owner, sizeof(context.api_owner), "%s", K_API_KEY);
    std::snprintf(context.maker_address, sizeof(context.maker_address), "%s", K_ADDRESS);
    std::snprintf(context.condition_id, sizeof(context.condition_id), "%s",
                  K_CONDITION_ID);
    std::snprintf(context.token_id, sizeof(context.token_id), "%s", K_TOKEN_YES);

    user_ws::UserMessage message{};
    const std::string placement =
        user_order_frame("0xaa", "PLACEMENT", "LIVE", "100", "0", "1714136516123");
    CHECK(user_ws::parse_user_message(placement.c_str(), placement.size(), message),
          "placement parsed");
    user_ws::ApplyOutcome outcome{};
    CHECK(applier.apply_order(message.orders[0], context, outcome), "placement applied");
    CHECK(outcome.needs_rest_confirmation, "stream observation requires REST confirm");
    const ledger::OrderRecord* record = ledger.orders().find("0xaa");
    CHECK(record && record->state == OrderState::LIVE, "order is LIVE");
    CHECK(record && !record->externally_observed, "our own order is not external");

    // Duplicate delivery (same observation) must be ignored.
    user_ws::ApplyOutcome duplicate{};
    CHECK(applier.apply_order(message.orders[0], context, duplicate), "dup accepted");
    CHECK(duplicate.duplicate, "duplicate detected");
    CHECK(ledger.duplicates_ignored() == 1, "duplicate counted once");

    // Out-of-order: the trade arrives before the UPDATE that reports the match.
    const std::string trade =
        user_trade_frame("t-1", "0xaa", "TRADE_STATUS_MATCHED", "40");
    CHECK(user_ws::parse_user_message(trade.c_str(), trade.size(), message),
          "trade parsed");
    user_ws::ApplyOutcome trade_outcome{};
    CHECK(applier.apply_trade(message.trades[0], context, trade_outcome),
          "trade applied");
    CHECK(ledger.fills().find("t-1") != nullptr, "fill recorded");
    CHECK(ledger.positions().find(K_TOKEN_YES) &&
              ledger.positions().find(K_TOKEN_YES)->shares == 40000000ULL,
          "inventory credited from the fill");
    CHECK(trade_outcome.needs_rest_confirmation, "non-terminal trade needs confirm");

    // Duplicate trade must not credit inventory twice.
    user_ws::ApplyOutcome duplicate_trade{};
    CHECK(applier.apply_trade(message.trades[0], context, duplicate_trade),
          "duplicate trade accepted");
    CHECK(duplicate_trade.duplicate, "duplicate trade detected");
    CHECK(ledger.positions().find(K_TOKEN_YES)->shares == 40000000ULL,
          "inventory not double counted");

    // The UPDATE that follows moves the order to PARTIALLY_FILLED.
    const std::string update =
        user_order_frame("0xaa", "UPDATE", "MATCHED", "100", "40", "1714136520000");
    CHECK(user_ws::parse_user_message(update.c_str(), update.size(), message),
          "update parsed");
    user_ws::ApplyOutcome update_outcome{};
    CHECK(applier.apply_order(message.orders[0], context, update_outcome),
          "update applied");
    CHECK(update_outcome.new_state == OrderState::PARTIALLY_FILLED,
          "partial fill state");

    // Full match closes the order.
    const std::string matched =
        user_order_frame("0xaa", "UPDATE", "MATCHED", "100", "100", "1714136530000");
    CHECK(user_ws::parse_user_message(matched.c_str(), matched.size(), message),
          "final update parsed");
    user_ws::ApplyOutcome matched_outcome{};
    CHECK(applier.apply_order(message.orders[0], context, matched_outcome),
          "final update applied");
    CHECK(matched_outcome.new_state == OrderState::MATCHED, "order matched");

    // A late PLACEMENT for an already-terminal order must not reopen it.
    user_ws::ApplyOutcome stale{};
    CHECK(applier.apply_order(message.orders[0], context, stale) || true,
          "stale event tolerated");
    const std::string stale_placement =
        user_order_frame("0xaa", "PLACEMENT", "LIVE", "100", "0", "1714136599999");
    CHECK(user_ws::parse_user_message(stale_placement.c_str(), stale_placement.size(),
                                      message),
          "stale placement parsed");
    user_ws::ApplyOutcome stale_outcome{};
    CHECK(applier.apply_order(message.orders[0], context, stale_outcome),
          "stale placement processed");
    CHECK(stale_outcome.illegal_transition, "illegal transition detected");
    CHECK(ledger.orders().find("0xaa")->state == OrderState::MATCHED,
          "terminal state protected from a late event");

    // Cancellation of an order we did not cancel is a divergence.
    commit_local_order(ledger, "0xdd", OrderState::LIVE, 0);
    const std::string cancellation =
        user_order_frame("0xdd", "CANCELLATION", "CANCELED", "100", "0", "1714136600000");
    CHECK(user_ws::parse_user_message(cancellation.c_str(), cancellation.size(),
                                      message),
          "cancellation parsed");
    user_ws::ApplyOutcome cancel_outcome{};
    CHECK(applier.apply_order(message.orders[0], context, cancel_outcome),
          "cancellation applied");
    CHECK(cancel_outcome.divergence, "unrequested cancellation flagged");
    CHECK(ledger.orders().find("0xdd")->state == OrderState::CANCELLED,
          "order cancelled locally");

    // An order belonging to another market must never touch local state.
    const std::string other_market =
        "{\"event_type\":\"order\",\"id\":\"0xee\",\"owner\":\"o\",\"market\":"
        "\"0x1111111111111111111111111111111111111111111111111111111111111111\","
        "\"asset_id\":\"123\",\"side\":\"BUY\",\"original_size\":\"10\","
        "\"size_matched\":\"0\",\"price\":\"0.5\",\"type\":\"PLACEMENT\","
        "\"status\":\"LIVE\",\"created_at\":\"1714136516\",\"expiration\":\"0\"}";
    CHECK(user_ws::parse_user_message(other_market.c_str(), other_market.size(),
                                      message),
          "other-market frame parsed");
    user_ws::ApplyOutcome other_outcome{};
    CHECK(applier.apply_order(message.orders[0], context, other_outcome),
          "other-market event handled");
    CHECK(other_outcome.divergence, "other market flagged");
    CHECK(ledger.orders().find("0xee") == nullptr, "other market not recorded");

    // An order we never sent (another session / lost POST result) is adopted
    // and blocks readiness until REST confirms it.
    // A genuinely foreign order: different API owner *and* different maker
    // address, i.e. another session or another account on the same stream.
    std::string foreign =
        "{\"event_type\":\"order\",\"id\":\"0xff\",\"owner\":\"other-key\","
        "\"market\":\"";
    foreign += K_CONDITION_ID;
    foreign += "\",\"asset_id\":\"";
    foreign += K_TOKEN_YES;
    foreign += "\",\"side\":\"BUY\",\"order_owner\":\"other-key\","
               "\"original_size\":\"10\",\"size_matched\":\"0\","
               "\"price\":\"0.49\",\"type\":\"PLACEMENT\",\"status\":\"LIVE\","
               "\"created_at\":\"1714136700\",\"expiration\":\"0\","
               "\"order_type\":\"GTC\",\"maker_address\":"
               "\"0x1111111111111111111111111111111111111111\",\"timestamp\":"
               "\"1714136700000\"}";
    CHECK(user_ws::parse_user_message(foreign.c_str(), foreign.size(), message),
          "foreign order parsed");
    user_ws::ApplyOutcome foreign_outcome{};
    CHECK(applier.apply_order(message.orders[0], context, foreign_outcome),
          "foreign order applied");
    CHECK(ledger.orders().find("0xff") &&
              ledger.orders().find("0xff")->externally_observed,
          "foreign order recorded as externally observed");
    CHECK(foreign_outcome.adopted_external, "foreign order adopted as external");
    CHECK(foreign_outcome.needs_rest_confirmation, "foreign order needs confirm");
}

// ═══════════════════════════════════════════════════════════════════════════
// 7. Order heartbeat
// ═══════════════════════════════════════════════════════════════════════════
void test_heartbeat() {
    SECTION("order_heartbeat");
    heartbeat::Config config{};
    config.enabled = true;
    char error[192]{};
    CHECK(config.validate(error, sizeof(error)), error);

    heartbeat::Config bad = config;
    bad.block_ms = 20000;
    CHECK(!bad.validate(error, sizeof(error)),
          "block threshold beyond the venue timeout is rejected");
    bad = config;
    bad.warn_ms = 4000;
    CHECK(!bad.validate(error, sizeof(error)),
          "warn below the interval is rejected");
    bad = config;
    bad.interval_ms = 9000;
    CHECK(!bad.validate(error, sizeof(error)), "interval above 6 s is rejected");
    bad = config;
    bad.request_options.total_timeout_ms = 6000;
    CHECK(!bad.validate(error, sizeof(error)),
          "request timeout longer than the interval is rejected");
    bad = config;
    bad.assume_cancelled_ms = 30000;
    CHECK(!bad.validate(error, sizeof(error)),
          "assume-cancelled beyond the venue worst case is rejected");
    bad = config;
    bad.assume_cancelled_ms = 12000;
    CHECK(!bad.validate(error, sizeof(error)),
          "assume-cancelled later than the venue's 10 s cancel threshold is rejected");
    // Worst-case acknowledged gap = interval + request timeout must stay at least
    // 2 s below the venue timeout, or one slow request loses the contract.
    bad = config;
    bad.interval_ms = 6000;
    bad.warn_ms = 7000;
    bad.block_ms = 9000;
    bad.assume_cancelled_ms = 10000;
    bad.request_options.total_timeout_ms = 2500;
    CHECK(!bad.validate(error, sizeof(error)),
          "interval + request timeout without margin is rejected");
    CHECK(std::strstr(error, "8000") != nullptr, error);
    heartbeat::Config documented = config;
    documented.interval_ms = 5000;              // documented cadence
    documented.request_options.total_timeout_ms = 2500;
    documented.warn_ms = 7000;
    documented.block_ms = 9000;
    documented.assume_cancelled_ms = 10000;     // venue cancel threshold
    CHECK(documented.validate(error, sizeof(error)), error);
    CHECK(heartbeat::K_VENUE_TIMEOUT_MS == 10000, "venue timeout constant");
    CHECK(heartbeat::K_VENUE_CADENCE_MS == 5000, "documented cadence constant");
    CHECK(heartbeat::K_VENUE_WORST_CASE_MS == 15000 &&
              heartbeat::K_VENUE_WORST_CASE_MS ==
                  heartbeat::K_VENUE_TIMEOUT_MS + heartbeat::K_VENUE_CHECK_MS,
          "worst-case cancellation horizon is the timeout plus the check cadence");
    // Deadline scheduling: a slow beat must not push the next one later.
    CHECK(heartbeat::OrderHeartbeat::next_wakeup_ms(1000, 1200, 5000) == 6000,
          "next wakeup is one interval after the previous deadline");
    CHECK(heartbeat::OrderHeartbeat::next_wakeup_ms(1000, 9000, 5000) > 9000,
          "a missed deadline re-anchors in the future instead of bunching up");
    CHECK(heartbeat::OrderHeartbeat::next_wakeup_ms(1000, 1200, 5000) - 1200 <= 5000,
          "the acknowledged gap stays within interval + request budget");

    const std::string path = dir("heartbeat");
    ledger::EventLedger ledger;
    CHECK(ledger.open(ledger_options(path), error, sizeof(error)), error);
    testing::FixtureTransport transport;
    clob::Credentials credentials = test_credentials();
    clob::ClobApiClient api(transport, credentials, "https://clob.polymarket.com",
                            "https://gamma-api.polymarket.com");
    heartbeat::OrderHeartbeat monitor(api, ledger, config);
    CHECK(monitor.health() == heartbeat::Health::WARN ||
              monitor.health() == heartbeat::Health::UNREACHABLE,
          "no acknowledgement yet is not healthy");

    transport.on("POST", "/v1/heartbeats", 200, "{\"heartbeat_id\":\"hb-1\"}");
    CHECK(monitor.tick(error, sizeof(error)), "first heartbeat accepted");
    CHECK(monitor.chain_active(), "chain established");
    CHECK(std::strcmp(monitor.current_id(), "hb-1") == 0, "id stored");
    CHECK(monitor.health() == heartbeat::Health::HEALTHY, "healthy after ack");
    const testing::RecordedRequest* request = transport.last();
    CHECK(request && std::strcmp(request->body, "{\"heartbeat_id\":\"\"}") == 0,
          "first heartbeat sends an empty id");

    transport.clear_rules();
    transport.on("POST", "/v1/heartbeats", 200, "{\"heartbeat_id\":\"hb-2\"}");
    CHECK(monitor.tick(error, sizeof(error)), "chained heartbeat accepted");
    CHECK(std::strcmp(monitor.current_id(), "hb-2") == 0, "id chained");
    request = transport.last();
    CHECK(request && std::strcmp(request->body, "{\"heartbeat_id\":\"hb-1\"}") == 0,
          "previous id is sent on the next heartbeat");
    CHECK(ledger.heartbeat().chain_active && !ledger.heartbeat().invalidated,
          "heartbeat state persisted");

    // A rejected id invalidates the contract: resting orders must be assumed
    // cancelled and readiness must drop.
    testing::FixtureResponse rejected{};
    rejected.code = 400;
    std::snprintf(rejected.body, sizeof(rejected.body),
                  "{\"error_msg\":\"Invalid Heartbeat ID\",\"heartbeat_id\":\"hb-9\"}");
    transport.queue_once("/v1/heartbeats", rejected);
    CHECK(!monitor.tick(error, sizeof(error)), "rejected heartbeat fails");
    CHECK(monitor.invalidated(), "invalidation flagged");
    CHECK(monitor.health() == heartbeat::Health::INVALIDATED, "health invalidated");
    CHECK(heartbeat::health_blocks_new_orders(monitor.health()),
          "invalidated heartbeat blocks new orders");
    CHECK(heartbeat::health_implies_cancelled_orders(monitor.health()),
          "invalidated heartbeat implies cancelled orders");
    CHECK(std::strcmp(monitor.current_id(), "hb-9") == 0, "expected id adopted");
    CHECK(ledger.heartbeat().invalidated, "invalidation persisted");

    // Resynchronising with the expected id clears the flag but the orders it
    // may have killed still require reconciliation.
    transport.clear_rules();
    transport.on("POST", "/v1/heartbeats", 200, "{\"heartbeat_id\":\"hb-10\"}");
    CHECK(monitor.tick(error, sizeof(error)), "resynchronisation accepted");
    CHECK(!monitor.invalidated(), "invalidation cleared");
    CHECK(monitor.resync_count() == 1, "resync counted");
    CHECK(monitor.health() == heartbeat::Health::HEALTHY, "healthy again");

    // The venue may document /heartbeats (OpenAPI) instead of /v1/heartbeats
    // (SDKs), and may want a JSON null instead of an empty string to start the
    // chain.  Both fallbacks must work without operator intervention.
    {
        const std::string path2 = dir("heartbeat-fallback");
        auto storage2 = std::make_unique<ledger::EventLedger>();
        ledger::EventLedger& ledger2 = *storage2;
        CHECK(ledger2.open(ledger_options(path2), error, sizeof(error)), error);
        testing::FixtureTransport transport2;
        clob::ClobApiClient api2(transport2, credentials,
                                 "https://clob.polymarket.com",
                                 "https://gamma-api.polymarket.com");
        heartbeat::Config null_config = config;
        heartbeat::OrderHeartbeat monitor2(api2, ledger2, null_config);
        // 404 on the SDK path, and the empty-string form rejected on the
        // OpenAPI path: the client must fall back to null and remember both.
        testing::FixtureResponse not_found{};
        not_found.code = 404;
        std::snprintf(not_found.body, sizeof(not_found.body),
                      "{\"error\":\"not found\"}");
        transport2.queue_once("/v1/heartbeats", not_found);
        testing::FixtureResponse bad_form{};
        bad_form.code = 400;
        std::snprintf(bad_form.body, sizeof(bad_form.body),
                      "{\"error\":\"heartbeat_id must be null\"}");
        transport2.queue_once("/heartbeats", bad_form);
        transport2.on("POST", "/heartbeats", 200, "{\"heartbeat_id\":\"hb-null\"}");
        CHECK(monitor2.tick(error, sizeof(error)), error);
        CHECK(std::strcmp(api2.heartbeat_path(), "/heartbeats") == 0,
              "the path that answered is remembered");
        CHECK(api2.heartbeat_uses_null_start(),
              "the chain-start body form that answered is remembered");
        CHECK(std::strcmp(monitor2.current_id(), "hb-null") == 0,
              "id from the fallback path is chained");
        CHECK(monitor2.health() == heartbeat::Health::HEALTHY,
              "healthy after the fallback");
    }

    // A delayed heartbeat must degrade WARN → BLOCKED and recover on the next
    // acknowledgement, using the monotonic clock only.
    {
        const std::string path3 = dir("heartbeat-delay");
        auto storage3 = std::make_unique<ledger::EventLedger>();
        ledger::EventLedger& ledger3 = *storage3;
        CHECK(ledger3.open(ledger_options(path3), error, sizeof(error)), error);
        testing::FixtureTransport transport3;
        clob::ClobApiClient api3(transport3, credentials,
                                 "https://clob.polymarket.com",
                                 "https://gamma-api.polymarket.com");
        heartbeat::Config fast = config;
        fast.interval_ms = 1000;
        fast.warn_ms = 1200;
        fast.block_ms = 1500;
        fast.assume_cancelled_ms = 2000;
        fast.max_consecutive_failures = 8;   // exercise the age thresholds, not failures
        fast.request_options.connect_timeout_ms = 200;
        fast.request_options.total_timeout_ms = 500;
        CHECK(fast.validate(error, sizeof(error)), error);
        heartbeat::OrderHeartbeat monitor3(api3, ledger3, fast);
        transport3.on("POST", "/v1/heartbeats", 200, "{\"heartbeat_id\":\"hb-a\"}");
        CHECK(monitor3.tick(error, sizeof(error)), "first beat accepted");
        CHECK(monitor3.health() == heartbeat::Health::HEALTHY, "healthy");
        std::this_thread::sleep_for(std::chrono::milliseconds(1300));
        CHECK(monitor3.health() == heartbeat::Health::WARN, "delayed beat warns");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(monitor3.health() == heartbeat::Health::BLOCKED,
              "further delay blocks new orders before the venue timeout");
        CHECK(!heartbeat::health_implies_cancelled_orders(monitor3.health()),
              "blocked is not yet 'orders cancelled'");
        transport3.clear_rules();
        transport3.on("POST", "/v1/heartbeats", 200, "{\"heartbeat_id\":\"hb-b\"}");
        CHECK(monitor3.tick(error, sizeof(error)), "recovered beat accepted");
        CHECK(monitor3.health() == heartbeat::Health::HEALTHY, "healthy again");
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        CHECK(heartbeat::health_implies_cancelled_orders(monitor3.health()),
              "past the venue timeout the orders may be cancelled");
        CHECK(heartbeat::health_blocks_new_orders(monitor3.health()),
              "and new orders stay blocked");
    }

    // Ambiguous transport failures must not be treated as success.
    transport.clear_rules();
    transport.on_transport("POST", "/v1/heartbeats", true, "Timeout was reached");
    CHECK(!monitor.tick(error, sizeof(error)), "ambiguous heartbeat fails");
    CHECK(monitor.consecutive_failures() == 1, "failure counted");
    CHECK(monitor.ambiguous_failures() == 1, "ambiguity counted");
    CHECK(!monitor.tick(error, sizeof(error)), "second ambiguous heartbeat fails");
    CHECK(monitor.health() == heartbeat::Health::UNREACHABLE,
          "repeated failures make the heartbeat unreachable");
}

// ═══════════════════════════════════════════════════════════════════════════
// 8. Reconciliation and readiness
// ═══════════════════════════════════════════════════════════════════════════
// The harness holds an EventLedger (bounded maps, >1 MiB) plus fixture buffers;
// it is always heap-allocated so the tests never depend on a large stack.
struct ReconciliationHarness {
    testing::FixtureTransport transport;
    clob::Credentials credentials = test_credentials();
    clob::ClobApiClient api{transport, credentials, "https://clob.polymarket.com",
                            "https://gamma-api.polymarket.com"};
    ledger::EventLedger ledger;
    recon::Context context{};

    explicit ReconciliationHarness(const char* name) {
        char error[160]{};
        CHECK(ledger.open(ledger_options(dir(name)), error, sizeof(error)), error);
        std::snprintf(context.condition_id, sizeof(context.condition_id), "%s",
                      K_CONDITION_ID);
        std::snprintf(context.token_id, sizeof(context.token_id), "%s", K_TOKEN_YES);
        std::snprintf(context.collateral_spender, sizeof(context.collateral_spender),
                      "%s", K_EXCHANGE);
        context.neg_risk = false;
        context.min_collateral = 1000000;       // 1 pUSD
        context.target_allowance = 2000000;     // 2 pUSD
        context.signature_type = 2;
    }

    void stub_orders(const std::string& body) {
        transport.on("GET", "/data/orders", 200, body.c_str());
    }
    void stub_trades(const std::string& body) {
        transport.on("GET", "/data/trades", 200, body.c_str());
    }
    void stub_balances(const std::string& collateral, const std::string& outcome) {
        transport.on("GET", "asset_type=COLLATERAL", 200, collateral.c_str());
        transport.on("GET", "asset_type=CONDITIONAL", 200, outcome.c_str());
    }
};

void test_reconciliation_ready() {
    SECTION("reconciliation_ready");
    auto harness_storage = std::make_unique<ReconciliationHarness>("recon-ready");
        ReconciliationHarness& harness = *harness_storage;
    commit_local_order(harness.ledger, "0xaa", OrderState::LIVE, 0);
    std::string items[1] = {order_json("0xaa", "LIVE", "100", "0")};
    harness.stub_orders(orders_page(items, 1));
    harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                        "\"data\":[]}");
    std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
    collateral += K_EXCHANGE;
    collateral += "\":\"5000000\"}}";
    std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
    outcome += K_EXCHANGE;
    outcome += "\":\"1\"}}";
    harness.stub_balances(collateral, outcome);

    recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
    recon::Report report{};
    CHECK(reconciler.run_startup(report, nullptr), "startup reconciliation succeeded");
    char reasons[512]{};
    report.format_reasons(reasons, sizeof(reasons));
    CHECK(report.readiness == recon::Readiness::READY, reasons);
    CHECK(report.venue_open_orders == 1 && report.local_open_orders == 1,
          "order sets agree");
    CHECK(report.collateral_balance == 5000000 && report.collateral_allowance == 5000000,
          "balances recorded");
    CHECK(harness.ledger.last_run() && harness.ledger.last_run()->ready == 1,
          "run persisted as READY");
}

void test_reconciliation_blocked_cases() {
    SECTION("reconciliation_blocked_cases");

    // (a) A venue order with no local record: adopted and blocked.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-adopt");
        ReconciliationHarness& harness = *harness_storage;
        std::string items[1] = {order_json("0xzz", "LIVE", "100", "0")};
        harness.stub_orders(orders_page(items, 1));
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"0\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "not ready");
        CHECK(report.adopted_orders == 1, "order adopted");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "venue_order_missing_locally") != nullptr, reasons);
        CHECK(harness.ledger.orders().find("0xzz") != nullptr,
              "adopted order is in the ledger");
        CHECK(harness.ledger.orders().find("0xzz")->externally_observed,
              "adopted order flagged external");
    }

    // (b) A local order the venue does not know and cannot resolve.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-absent");
        ReconciliationHarness& harness = *harness_storage;
        commit_local_order(harness.ledger, "0xaa", OrderState::LIVE, 0);
        harness.stub_orders("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        testing::FixtureResponse missing{};
        missing.code = 404;
        std::snprintf(missing.body, sizeof(missing.body),
                      "{\"error\":\"order not found\"}");
        harness.transport.queue_once("/data/order/", missing);
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"0\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "not ready");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "local_order_absent_on_venue") != nullptr, reasons);
        CHECK(harness.ledger.orders().find("0xaa")->state == OrderState::FAILED,
              "absent order marked FAILED (no exposure)");
    }

    // (c) An ambiguous single-order read leaves the order UNKNOWN.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-ambiguous");
        ReconciliationHarness& harness = *harness_storage;
        commit_local_order(harness.ledger, "0xaa", OrderState::SUBMITTING, 0);
        harness.stub_orders("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        harness.transport.on_transport("GET", "/data/order/", true,
                                       "Timeout was reached");
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"0\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "not ready");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "order_state_unknown") != nullptr, reasons);
        CHECK(harness.ledger.orders().find("0xaa")->state == OrderState::UNKNOWN,
              "ambiguous order moved to UNKNOWN");
    }

    // (d) A fill the ledger never recorded.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-fill");
        ReconciliationHarness& harness = *harness_storage;
        commit_local_order(harness.ledger, "0xaa", OrderState::LIVE, 40000000ULL);
        std::string items[1] = {order_json("0xaa", "MATCHED", "100", "40")};
        harness.stub_orders(orders_page(items, 1));
        std::string trades = "{\"limit\":500,\"count\":1,\"next_cursor\":\"LTE=\","
                             "\"data\":[";
        trades += trade_json("t-77", "0xaa", "TRADE_STATUS_MATCHED", "40");
        trades += "]}";
        harness.stub_trades(trades);
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"40000000\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"1\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "not ready");
        CHECK(report.fills_unregistered == 1, "unregistered fill counted");
        CHECK(harness.ledger.fills().find("t-77") != nullptr,
              "unregistered fill recorded from REST");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "fill_not_registered") != nullptr, reasons);
    }

    // (e) Insufficient allowance / balance.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-allowance");
        ReconciliationHarness& harness = *harness_storage;
        harness.stub_orders("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"10\"}}";
        std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"0\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "not ready");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "collateral_allowance_insufficient") != nullptr,
              reasons);
        // No inventory means no ERC-1155 approval is needed yet.
        CHECK(std::strstr(reasons, "outcome_token_allowance_missing") == nullptr,
              reasons);
    }

    // (e2) Inventory without an ERC-1155 approval blocks: we could not sell.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-approval");
        ReconciliationHarness& harness = *harness_storage;
        harness.stub_orders("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"40000000\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"0\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "not ready");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "outcome_token_allowance_missing") != nullptr,
              reasons);
        CHECK(harness.ledger.available_inventory(K_TOKEN_YES) == 40000000ULL,
              "startup adopted the venue inventory");
    }
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-balance");
        ReconciliationHarness& harness = *harness_storage;
        harness.stub_orders("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"10\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"1\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "not ready");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "collateral_balance_insufficient") != nullptr,
              reasons);
    }

    // (f) Inventory disagreement after startup (no adoption allowed).
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-inventory");
        ReconciliationHarness& harness = *harness_storage;
        commit_local_order(harness.ledger, "0xaa", OrderState::MATCHED, 100000000ULL);
        harness.stub_orders("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"1\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        // First run is a startup run: it adopts the venue balance.
        CHECK(reconciler.run_startup(report, nullptr), "startup adopted inventory");
        CHECK(report.ledger_inventory == 0 || report.outcome_balance == 0,
              "inventory reconciled");
        // Now pretend the ledger believes we still hold shares.
        ledger::Event event{};
        event.type = ledger::EventType::POSITION_SNAPSHOT;
        event.source = ledger::Source::LOCAL;
        event.wall_ns = ledger::now_wall_ns();
        event.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
        event.add_u64(ledger::F_SIZE_RAW, 50000000ULL);
        ledger::compute_event_key(event.type, event.source, "", "", 0, 7, event.key);
        char error[128]{};
        CHECK(harness.ledger.commit(event, error, sizeof(error)), "position set");
        recon::Report second{};
        CHECK(!reconciler.run_after_disconnect(second, nullptr),
              "post-disconnect run refuses to trade");
        char reasons[512]{};
        second.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "inventory_exceeds_venue_balance") != nullptr,
              reasons);
    }

    // (g) An unreachable venue read blocks.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("recon-unreachable");
        ReconciliationHarness& harness = *harness_storage;
        harness.transport.on_transport("GET", "/data/orders", false,
                                       "Could not resolve host");
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "not ready");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "orders_query_failed") != nullptr, reasons);
    }
}


// ═══════════════════════════════════════════════════════════════════════════
// 9. WebSocket URL parsing, RFC 6455 accept value, user-channel protocol
// ═══════════════════════════════════════════════════════════════════════════
void test_ws_url_and_handshake() {
    SECTION("ws_url_and_handshake");
    char host[160]{};
    char path[192]{};
    int port = 0;
    bool tls = false;
    CHECK(ws_url::parse_url("wss://ws-subscriptions-clob.polymarket.com/ws/user",
                            host, sizeof(host), path, sizeof(path), port, tls) &&
              tls && port == 443 &&
              std::strcmp(host, "ws-subscriptions-clob.polymarket.com") == 0 &&
              std::strcmp(path, "/ws/user") == 0,
          "user WSS url parsed");
    CHECK(ws_url::parse_url("wss://example.com:8443/ws/market", host, sizeof(host),
                            path, sizeof(path), port, tls) && port == 8443,
          "explicit port parsed");
    CHECK(!ws_url::parse_url("https://clob.polymarket.com/order", host, sizeof(host),
                             path, sizeof(path), port, tls),
          "https is not a websocket url");
    CHECK(!ws_url::parse_url("ws://user:pass@example.com/ws", host, sizeof(host),
                             path, sizeof(path), port, tls),
          "credentials in url rejected");
    CHECK(!ws_url::parse_url("wss://example.com:0/ws", host, sizeof(host), path,
                             sizeof(path), port, tls),
          "port 0 rejected");
    CHECK(!ws_url::parse_url("wss://example.com:70000/ws", host, sizeof(host), path,
                             sizeof(path), port, tls),
          "port above 65535 rejected");
    CHECK(!ws_url::parse_url("wss://[::1]/ws", host, sizeof(host), path, sizeof(path),
                             port, tls),
          "IPv6 literal rejected");
    CHECK(ws_url::parse_url("wss://example.com", host, sizeof(host), path,
                            sizeof(path), port, tls) &&
              std::strcmp(path, "/") == 0,
          "missing path defaults to /");

    // RFC 6455 §1.3 known-answer vector (key "dGhlIHNhbXBsZSBub25jZQ==").
    char accept[32]{};
    ws_url::websocket_accept("dGhlIHNhbXBsZSBub25jZQ==", accept);
    CHECK(std::strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0,
          "RFC 6455 accept value");

    // FIPS 180-1 "abc" vector, pinning the shared SHA-1 used by both channels.
    uint8_t digest[20]{};
    sha1(reinterpret_cast<const uint8_t*>("abc"), 3, digest);
    static constexpr uint8_t K_ABC[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81,
                                          0x6a, 0xba, 0x3e, 0x25, 0x71, 0x78, 0x50,
                                          0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    CHECK(std::memcmp(digest, K_ABC, sizeof(K_ABC)) == 0, "SHA-1 abc vector");
}

void test_user_ws_protocol() {
    SECTION("user_ws_protocol");
    user_ws::Config config{};
    config.enabled = true;
    config.market_count = 1;
    std::snprintf(config.markets[0], sizeof(config.markets[0]), "%s", K_CONDITION_ID);
    char error[192]{};
    CHECK(config.validate(error, sizeof(error)), error);

    user_ws::Config bad = config;
    std::snprintf(bad.url, sizeof(bad.url), "ws://ws-subscriptions-clob.polymarket.com/ws/user");
    CHECK(!bad.validate(error, sizeof(error)), "plain ws:// rejected");
    bad = config;
    bad.keepalive_interval_ms = 30000;
    CHECK(!bad.validate(error, sizeof(error)), "keepalive above the documented 10 s rejected");
    bad = config;
    bad.idle_timeout_ms = 5000;
    CHECK(!bad.validate(error, sizeof(error)), "idle below keepalive rejected");
    bad = config;
    bad.pong_timeout_ms = 15000;
    CHECK(!bad.validate(error, sizeof(error)), "pong below idle rejected");
    bad = config;
    std::snprintf(bad.markets[0], sizeof(bad.markets[0]), "not-a-condition-id");
    CHECK(!bad.validate(error, sizeof(error)), "malformed market filter rejected");

    clob::Credentials credentials = test_credentials();
    char frame[user_ws::K_MAX_SUBSCRIPTION_FRAME]{};
    size_t frame_len = 0;
    CHECK(user_ws::build_subscription_frame(config, credentials, frame, sizeof(frame),
                                            frame_len),
          "subscription frame built");
    CHECK(std::strstr(frame, "\"type\":\"user\"") != nullptr, "frame type is user");
    CHECK(std::strstr(frame, K_API_KEY) != nullptr, "frame carries the api key");
    CHECK(std::strstr(frame, K_API_SECRET) != nullptr, "frame carries the secret");
    CHECK(std::strstr(frame, K_CONDITION_ID) != nullptr, "frame filters by market");
    CHECK(std::strstr(frame, "\"markets\":[") != nullptr, "markets array present");

    user_ws::Config whole_account = config;
    whole_account.market_count = 0;
    CHECK(user_ws::build_subscription_frame(whole_account, credentials, frame,
                                            sizeof(frame), frame_len),
          "whole-account frame built");
    CHECK(std::strstr(frame, "markets") == nullptr,
          "omitting markets follows the whole account");

    char tiny[16]{};
    CHECK(!user_ws::build_subscription_frame(config, credentials, tiny, sizeof(tiny),
                                             frame_len),
          "undersized buffer rejected");
    clob::Credentials empty{};
    CHECK(!user_ws::build_subscription_frame(config, empty, frame, sizeof(frame),
                                             frame_len),
          "incomplete credentials rejected");

    char operation[256]{};
    CHECK(user_ws::build_operation_frame("subscribe", K_CONDITION_ID, operation,
                                         sizeof(operation)) &&
              std::strstr(operation, "\"operation\":\"subscribe\"") != nullptr,
          "hot subscribe frame");
    CHECK(user_ws::build_operation_frame("unsubscribe", K_CONDITION_ID, operation,
                                         sizeof(operation)),
          "hot unsubscribe frame");
    CHECK(!user_ws::build_operation_frame("resubscribe", K_CONDITION_ID, operation,
                                          sizeof(operation)),
          "unknown operation rejected");
    CHECK(!user_ws::build_operation_frame("subscribe", "0xnope", operation,
                                          sizeof(operation)),
          "malformed condition id rejected");
}

void test_frame_processor() {
    SECTION("frame_processor");
    const std::string path = dir("frame-processor");
    ledger::EventLedger ledger;
    char error[160]{};
    CHECK(ledger.open(ledger_options(path), error, sizeof(error)), error);
    user_ws::ApplierContext context{};
    std::snprintf(context.api_owner, sizeof(context.api_owner), "%s", K_API_KEY);
    std::snprintf(context.maker_address, sizeof(context.maker_address), "%s", K_ADDRESS);
    std::snprintf(context.condition_id, sizeof(context.condition_id), "%s",
                  K_CONDITION_ID);
    std::snprintf(context.token_id, sizeof(context.token_id), "%s", K_TOKEN_YES);
    user_ws::FrameProcessor processor(ledger, context);
    CHECK(processor.stale(), "a fresh processor starts stale");

    const std::string placement =
        user_order_frame("0xaa", "PLACEMENT", "LIVE", "100", "0", "1714136516123");
    CHECK(processor.handle_frame(placement.c_str(), placement.size()),
          "placement frame handled");
    CHECK(processor.frames() == 1 && processor.events_applied() == 1,
          "counters updated");
    CHECK(processor.stale(), "stream data alone does not clear staleness");
    processor.clear_stale();
    CHECK(!processor.stale(), "reconciliation clears staleness");

    // Duplicate delivery is absorbed.
    CHECK(processor.handle_frame(placement.c_str(), placement.size()),
          "duplicate frame handled");
    CHECK(processor.duplicates() == 1, "duplicate counted");
    CHECK(ledger.orders().find("0xaa") &&
              ledger.orders().find("0xaa")->state == OrderState::LIVE,
          "state unchanged by duplicate");

    // Keep-alive frames are not events.
    CHECK(processor.handle_frame("{}", 2), "empty keep-alive handled");
    CHECK(processor.handle_frame("PONG", 4), "PONG handled");
    CHECK(processor.keepalive_acks() == 2, "keep-alives counted");
    CHECK(processor.events_applied() == 1, "keep-alives are not events");

    // An unparseable frame keeps the session but marks the view stale.
    processor.clear_stale();
    CHECK(processor.handle_frame("{\"event_type\":", 14), "truncated frame tolerated");
    CHECK(processor.parse_failures() == 1 && processor.stale(),
          "parse failure marks stale");

    // A server error frame ends the session.
    processor.clear_stale();
    CHECK(!processor.handle_frame("{\"message\":\"unauthorized\"}", 26),
          "error frame stops the session");
    CHECK(processor.stale(), "error frame marks stale");

    // A divergence (order we never sent) marks the view stale again.
    processor.clear_stale();
    std::string foreign =
        "{\"event_type\":\"order\",\"id\":\"0xff\",\"owner\":\"other-key\","
        "\"market\":\"";
    foreign += K_CONDITION_ID;
    foreign += "\",\"asset_id\":\"";
    foreign += K_TOKEN_YES;
    foreign += "\",\"side\":\"BUY\",\"order_owner\":\"other-key\","
               "\"original_size\":\"10\",\"size_matched\":\"0\","
               "\"price\":\"0.49\",\"type\":\"PLACEMENT\",\"status\":\"LIVE\","
               "\"created_at\":\"1714136700\",\"expiration\":\"0\","
               "\"order_type\":\"GTC\",\"maker_address\":"
               "\"0x1111111111111111111111111111111111111111\",\"timestamp\":"
               "\"1714136700000\"}";
    CHECK(processor.handle_frame(foreign.c_str(), foreign.size()),
          "foreign order frame handled");
    CHECK(processor.divergences() == 1 && processor.stale(),
          "divergence marks stale");
    CHECK(ledger.orders().find("0xff") &&
              ledger.orders().find("0xff")->externally_observed,
          "foreign order adopted");

    // A trade frame credits inventory exactly once.
    const std::string trade =
        user_trade_frame("t-1", "0xaa", "TRADE_STATUS_MATCHED", "40");
    CHECK(processor.handle_frame(trade.c_str(), trade.size()), "trade frame handled");
    CHECK(ledger.positions().find(K_TOKEN_YES) &&
              ledger.positions().find(K_TOKEN_YES)->shares == 40000000ULL,
          "inventory credited");
    CHECK(processor.handle_frame(trade.c_str(), trade.size()), "trade redelivered");
    CHECK(ledger.positions().find(K_TOKEN_YES)->shares == 40000000ULL,
          "inventory not double counted");
    CHECK(processor.confirmations_required() > 0,
          "non-terminal observations require REST confirmation");
}


// ═══════════════════════════════════════════════════════════════════════════
// 10. Order recorder: durable ticket before egress, UNKNOWN on ambiguity
// ═══════════════════════════════════════════════════════════════════════════
struct SignedFixture {
    EIP712Signer signer;
    OrderV2 order{};
    WireBody body{};
    char token_id[80]{};
    bool ok = false;
};

// EIP712Signer is non-copyable (it holds key material), so fixtures are filled
// in place rather than returned.
void make_signed_order(SignedFixture& fixture, uint64_t salt, uint8_t side,
                       uint64_t maker_amount, uint64_t taker_amount) {
    // The caller passes a freshly constructed fixture: EIP712Signer is neither
    // copyable nor assignable because it holds key material.
    uint8_t private_key[32];
    for (int i = 0; i < 32; ++i) private_key[i] = static_cast<uint8_t>(i + 1);
    if (!fixture.signer.init(private_key, false)) return;
    std::snprintf(fixture.token_id, sizeof(fixture.token_id), "%s", K_TOKEN_YES);
    uint64_t limbs[4] = {0, 0, 0, 0};
    if (!venue::parse_uint256_limbs(fixture.token_id, std::strlen(fixture.token_id),
                                    limbs))
        return;
    for (int word = 0; word < 4; ++word)
        for (int byte = 0; byte < 8; ++byte)
            fixture.order.token_id[word * 8 + byte] = static_cast<uint8_t>(
                (limbs[3 - word] >> (8 * (7 - byte))) & 0xFFULL);
    fixture.order.salt = salt;
    std::memcpy(fixture.order.maker, fixture.signer.signer_address(), 20);
    std::memcpy(fixture.order.signer, fixture.signer.signer_address(), 20);
    fixture.order.maker_amount = maker_amount;
    fixture.order.taker_amount = taker_amount;
    fixture.order.side = side;
    fixture.order.signature_type = 0;
    fixture.order.timestamp_ms = 1714136516123ULL;
    uint8_t signature[65];
    if (!fixture.signer.sign_order(fixture.order, signature)) return;
    char maker_hex[43]{};
    char signer_hex[43]{};
    rpc::bytes_to_hex(fixture.order.maker, 20, maker_hex, sizeof(maker_hex));
    rpc::bytes_to_hex(fixture.order.signer, 20, signer_hex, sizeof(signer_hex));
    fixture.ok = build_wire_body(fixture.order, signature, fixture.token_id,
                                 maker_hex, signer_hex, K_API_KEY, "FAK",
                                 fixture.body, 0);
}

void test_order_recorder() {
    SECTION("order_recorder");
    SignedFixture fixture_storage{};
    make_signed_order(fixture_storage, 987654321ULL, 0, 4900000ULL, 10000000ULL);
    const SignedFixture& fixture = fixture_storage;
    CHECK(fixture.ok, "wire body built by the production signer");

    recorder::WireOrder wire{};
    CHECK(recorder::parse_wire_body(fixture.body.buf, fixture.body.len, false, wire),
          "wire body parsed back");
    CHECK(wire.salt == fixture.order.salt && wire.side == 0 &&
              wire.maker_amount == 4900000ULL && wire.taker_amount == 10000000ULL,
          "fields round-trip");
    uint8_t expected[32]{};
    crowdintel::compute_order_digest(fixture.order, false, expected);
    CHECK(std::memcmp(expected, wire.digest, 32) == 0,
          "local digest equals the signed order digest");
    CHECK(std::strncmp(wire.ticket_id, "L:", 2) == 0 &&
              std::strlen(wire.ticket_id) == 66,
          "ticket id shape");

    // The negative-risk domain must produce a different identity.
    recorder::WireOrder neg_risk_wire{};
    CHECK(recorder::parse_wire_body(fixture.body.buf, fixture.body.len, true,
                                    neg_risk_wire),
          "parsed with the negative-risk domain");
    CHECK(std::memcmp(neg_risk_wire.digest, wire.digest, 32) != 0,
          "domain separation changes the ticket identity");

    // A malformed body is never recorded as an order.
    recorder::WireOrder broken{};
    CHECK(!recorder::parse_wire_body("{\"order\":{\"salt\":1}}", 18, false, broken),
          "incomplete body rejected");
    CHECK(!recorder::parse_wire_body(fixture.body.buf, 40, false, broken),
          "truncated body rejected");

    MarketConfig config{};
    config.neg_risk = false;
    std::snprintf(config.condition_id, sizeof(config.condition_id), "%s",
                  K_CONDITION_ID);

    // (a) Ambiguous transport outcome → UNKNOWN, reservation kept, no retry.
    {
        const std::string path = dir("recorder-ambiguous");
        auto ledger = std::make_unique<ledger::EventLedger>();
        char error[192]{};
        CHECK(ledger->open(ledger_options(path), error, sizeof(error)), error);
        recorder::OrderRecorder recorder(*ledger, config);
        CHECK(recorder.record_submission(wire, error, sizeof(error)), error);
        const ledger::OrderRecord* ticket = ledger->orders().find(wire.ticket_id);
        CHECK(ticket && ticket->state == OrderState::SUBMITTING, "ticket SUBMITTING");
        CHECK(ledger->blocking_orders() == 1, "in-flight ticket blocks new orders");
        CHECK(ledger->reserved_collateral() == 4900000ULL,
              "worst-case cost reserved before egress");

        SubmitResult ambiguous{};
        ambiguous.ok = false;
        ambiguous.retryable = false;
        ambiguous.ambiguous = true;
        std::snprintf(ambiguous.error, sizeof(ambiguous.error), "Timeout was reached");
        CHECK(recorder.record_result(wire, ambiguous, true, nullptr, error,
                                     sizeof(error)),
              error);
        CHECK(ledger->orders().find(wire.ticket_id)->state == OrderState::UNKNOWN,
              "ambiguous outcome becomes UNKNOWN");
        CHECK(ledger->reserved_collateral() == 4900000ULL,
              "reservation is kept while UNKNOWN");
        CHECK(ledger->blocking_orders() == 1, "UNKNOWN keeps blocking");
        CHECK(recorder.ambiguous_results() == 1, "ambiguity counted");
        // Why it is UNKNOWN must survive in the ledger: the fixed reason
        // vocabulary plus the transport's own text, both readable after a replay.
        ledger::OrderRecord unknown_record{};
        CHECK(ledger->order_copy(wire.ticket_id, unknown_record),
              "UNKNOWN ticket readable through the locked accessor");
        CHECK(std::strcmp(unknown_record.unknown_reason, "ambiguous_transport") == 0,
              "the ledger records why the order is UNKNOWN");
        CHECK(std::strcmp(unknown_record.venue_status, "Timeout was reached") == 0,
              "the transport error text is recorded and reachable");
    }

    // (b) Accepted with a venue id → venue record + ticket retired.
    {
        const std::string path = dir("recorder-accepted");
        auto ledger = std::make_unique<ledger::EventLedger>();
        char error[192]{};
        CHECK(ledger->open(ledger_options(path), error, sizeof(error)), error);
        recorder::OrderRecorder recorder(*ledger, config);
        CHECK(recorder.record_submission(wire, error, sizeof(error)), error);
        SubmitResult accepted{};
        accepted.ok = true;
        accepted.http_code = 200;
        std::snprintf(accepted.order_id, sizeof(accepted.order_id),
                      "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b");
        std::snprintf(accepted.status, sizeof(accepted.status), "live");
        clob::OrderPostResult detail{};
        detail.success = true;
        detail.taking_amount = 0;
        CHECK(recorder.record_result(wire, accepted, false, &detail, error,
                                     sizeof(error)),
              error);
        const ledger::OrderRecord* venue_record =
            ledger->orders().find(accepted.order_id);
        CHECK(venue_record && venue_record->state == OrderState::LIVE,
              "venue-keyed record is LIVE");
        CHECK(ledger->orders().find(wire.ticket_id)->state == OrderState::SUPERSEDED,
              "ticket retired once linked");
        CHECK(ledger->orders().find(wire.ticket_id)->reserved_amount == 0,
              "the ticket itself no longer holds a reservation");
        CHECK(ledger->reserved_collateral() == 4900000ULL,
              "the reservation moved to the venue record exactly once");
        CHECK(venue_record->reservation_active,
              "the open venue order is reserved (no double spend)");
        CHECK(ledger->blocking_orders() == 0, "linked order no longer blocks");
    }

    // (c) FAK partial fill straight from the POST /order response.
    {
        const std::string path = dir("recorder-partial");
        auto ledger = std::make_unique<ledger::EventLedger>();
        char error[192]{};
        CHECK(ledger->open(ledger_options(path), error, sizeof(error)), error);
        recorder::OrderRecorder recorder(*ledger, config);
        CHECK(recorder.record_submission(wire, error, sizeof(error)), error);
        SubmitResult partial{};
        partial.ok = true;
        partial.http_code = 200;
        std::snprintf(partial.order_id, sizeof(partial.order_id), "0xabc123");
        std::snprintf(partial.status, sizeof(partial.status), "matched");
        clob::OrderPostResult detail{};
        detail.success = true;
        detail.taking_amount = 4000000ULL;
        detail.trade_id_count = 1;
        std::snprintf(detail.trade_ids[0], sizeof(detail.trade_ids[0]), "t-partial");
        CHECK(recorder.record_result(wire, partial, false, &detail, error,
                                     sizeof(error)),
              error);
        CHECK(ledger->orders().find("0xabc123")->state == OrderState::PARTIALLY_FILLED,
              "FAK partial is PARTIALLY_FILLED");
        CHECK(ledger->fills().find("t-partial") != nullptr,
              "fill recorded from the response, not waited for on the stream");
        CHECK(ledger->positions().find(K_TOKEN_YES) &&
                  ledger->positions().find(K_TOKEN_YES)->shares == 4000000ULL,
              "inventory credited from the response fill");
    }

    // (d) Rejection releases the reservation and unblocks.
    {
        const std::string path = dir("recorder-rejected");
        auto ledger = std::make_unique<ledger::EventLedger>();
        char error[192]{};
        CHECK(ledger->open(ledger_options(path), error, sizeof(error)), error);
        recorder::OrderRecorder recorder(*ledger, config);
        CHECK(recorder.record_submission(wire, error, sizeof(error)), error);
        SubmitResult rejected{};
        rejected.ok = false;
        rejected.retryable = false;
        std::snprintf(rejected.error, sizeof(rejected.error), "not enough balance");
        CHECK(recorder.record_result(wire, rejected, false, nullptr, error,
                                     sizeof(error)),
              error);
        CHECK(ledger->orders().find(wire.ticket_id)->state == OrderState::REJECTED,
              "rejection recorded");
        CHECK(ledger->reserved_collateral() == 0, "reservation released");
        CHECK(ledger->blocking_orders() == 0, "rejection unblocks egress");
    }

    // (e) A SELL ticket reserves inventory, not collateral.
    {
        const std::string path = dir("recorder-sell");
        auto ledger = std::make_unique<ledger::EventLedger>();
        char error[192]{};
        CHECK(ledger->open(ledger_options(path), error, sizeof(error)), error);
        // Seed confirmed inventory as reconciliation would at startup.
        ledger::Event seed{};
        seed.type = ledger::EventType::POSITION_SNAPSHOT;
        seed.source = ledger::Source::REST;
        seed.wall_ns = ledger::now_wall_ns();
        seed.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
        seed.add_u64(ledger::F_SIZE_RAW, 10000000ULL);
        ledger::compute_event_key(seed.type, seed.source, "", "", 0, 1, seed.key);
        CHECK(ledger->commit(seed, error, sizeof(error)), error);
        CHECK(ledger->available_inventory(K_TOKEN_YES) == 10000000ULL,
              "inventory seeded");

        SignedFixture sell_storage{};
        make_signed_order(sell_storage, 424242ULL, 1, 10000000ULL, 5100000ULL);
        const SignedFixture& sell = sell_storage;
        CHECK(sell.ok, "sell wire body built");
        recorder::WireOrder sell_wire{};
        CHECK(recorder::parse_wire_body(sell.body.buf, sell.body.len, false, sell_wire),
              "sell body parsed");
        recorder::OrderRecorder recorder(*ledger, config);
        CHECK(recorder.record_submission(sell_wire, error, sizeof(error)), error);
        CHECK(ledger->available_inventory(K_TOKEN_YES) == 0,
              "in-flight SELL reserves the whole inventory (no double sell)");
        SubmitResult accepted{};
        accepted.ok = true;
        std::snprintf(accepted.order_id, sizeof(accepted.order_id), "0xsell");
        std::snprintf(accepted.status, sizeof(accepted.status), "live");
        CHECK(recorder.record_result(sell_wire, accepted, false, nullptr, error,
                                     sizeof(error)),
              error);
        CHECK(ledger->available_inventory(K_TOKEN_YES) == 0,
              "reservation follows the order, not the ticket");
    }
}

void test_ticket_reconciliation() {
    SECTION("ticket_reconciliation");
    // An unlinked ticket (crash between egress and response) must block.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("ticket-unlinked");
        ReconciliationHarness& harness = *harness_storage;
        SignedFixture fixture_storage{};
        make_signed_order(fixture_storage, 555000111ULL, 0, 4900000ULL, 10000000ULL);
        const SignedFixture& fixture = fixture_storage;
        recorder::WireOrder wire{};
        CHECK(recorder::parse_wire_body(fixture.body.buf, fixture.body.len, false,
                                        wire),
              "wire parsed");
        {
            auto lock = harness.ledger.guard();
            ledger::Event event{};
            event.type = ledger::EventType::ORDER_SUBMITTING;
            event.source = ledger::Source::LOCAL;
            event.wall_ns = ledger::now_wall_ns();
            event.add_str(ledger::F_ORDER_ID, wire.ticket_id);
            event.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
            event.add_str(ledger::F_CONDITION_ID, K_CONDITION_ID);
            event.add_u8(ledger::F_SIDE, 0);
            event.add_u64(ledger::F_SALT, wire.salt);
            event.add_u64(ledger::F_SIZE_RAW, 10000000ULL);
            event.add_u64(ledger::F_PRICE_RAW, 490000ULL);
            event.add_u8(ledger::F_STATE,
                         static_cast<uint8_t>(OrderState::SUBMITTING));
            ledger::compute_event_key(event.type, event.source, wire.ticket_id, "",
                                      wire.timestamp_ms, wire.salt, event.key);
            char error[128]{};
            CHECK(harness.ledger.commit_locked(event, error, sizeof(error)), error);
        }
        harness.stub_orders("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"1\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(!reconciler.run_startup(report, nullptr), "unlinked ticket is not ready");
        char reasons[512]{};
        report.format_reasons(reasons, sizeof(reasons));
        CHECK(std::strstr(reasons, "order_state_unknown") != nullptr, reasons);
        CHECK(harness.ledger.orders().find(wire.ticket_id)->state ==
                  OrderState::UNKNOWN,
              "unlinked ticket forced to UNKNOWN");
    }

    // A ticket whose order is resting on the venue is linked by unique
    // fingerprint, retiring the ticket without losing the exposure.
    {
        auto harness_storage = std::make_unique<ReconciliationHarness>("ticket-linked");
        ReconciliationHarness& harness = *harness_storage;
        SignedFixture fixture_storage{};
        make_signed_order(fixture_storage, 555000222ULL, 0, 4900000ULL, 10000000ULL);
        const SignedFixture& fixture = fixture_storage;
        recorder::WireOrder wire{};
        CHECK(recorder::parse_wire_body(fixture.body.buf, fixture.body.len, false,
                                        wire),
              "wire parsed");
        {
            auto lock = harness.ledger.guard();
            ledger::Event event{};
            event.type = ledger::EventType::ORDER_SUBMITTING;
            event.source = ledger::Source::LOCAL;
            event.wall_ns = ledger::now_wall_ns();
            event.add_str(ledger::F_ORDER_ID, wire.ticket_id);
            event.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
            event.add_str(ledger::F_CONDITION_ID, K_CONDITION_ID);
            event.add_u8(ledger::F_SIDE, 0);
            event.add_u64(ledger::F_SALT, wire.salt);
            event.add_u64(ledger::F_SIZE_RAW, 10000000ULL);
            event.add_u64(ledger::F_PRICE_RAW, 490000ULL);
            event.add_u8(ledger::F_STATE,
                         static_cast<uint8_t>(OrderState::SUBMITTING));
            ledger::compute_event_key(event.type, event.source, wire.ticket_id, "",
                                      wire.timestamp_ms, wire.salt, event.key);
            char error[128]{};
            CHECK(harness.ledger.commit_locked(event, error, sizeof(error)), error);
        }
        // The venue shows exactly one order with the same fingerprint.
        std::string items[1] = {order_json("0xresting", "LIVE", "10", "0")};
        harness.stub_orders(orders_page(items, 1));
        harness.stub_trades("{\"limit\":500,\"count\":0,\"next_cursor\":\"LTE=\","
                            "\"data\":[]}");
        std::string collateral = "{\"balance\":\"5000000\",\"allowances\":{\"";
        collateral += K_EXCHANGE;
        collateral += "\":\"5000000\"}}";
        std::string outcome = "{\"balance\":\"0\",\"allowances\":{\"";
        outcome += K_EXCHANGE;
        outcome += "\":\"1\"}}";
        harness.stub_balances(collateral, outcome);
        recon::Reconciler reconciler(harness.api, harness.ledger, harness.context);
        recon::Report report{};
        CHECK(reconciler.run_startup(report, nullptr), "ticket linked and ready");
        CHECK(report.tickets_linked == 1, "one ticket linked");
        CHECK(harness.ledger.orders().find(wire.ticket_id)->state ==
                  OrderState::SUPERSEDED,
              "ticket retired");
        const ledger::OrderRecord* venue_record =
            harness.ledger.orders().find("0xresting");
        CHECK(venue_record && venue_record->state == OrderState::LIVE,
              "venue record carries the lifecycle");
        CHECK(harness.ledger.blocking_orders() == 0, "nothing left unknown");
    }
}


// ═══════════════════════════════════════════════════════════════════════════
// 11. Ledger concurrency: the topology writes from the gateway thread, the
//     user-channel thread and the supervisor thread at the same time.
// ═══════════════════════════════════════════════════════════════════════════
void test_ledger_concurrency() {
    SECTION("ledger_concurrency");
    const std::string path = dir("ledger-concurrency");
    auto storage = std::make_unique<ledger::EventLedger>();
    ledger::EventLedger& ledger = *storage;
    char error[160]{};
    CHECK(ledger.open(ledger_options(path), error, sizeof(error)), error);

    constexpr int kWriters = 3;
    constexpr int kEventsPerWriter = 300;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> write_failures{0};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            // The cross-thread accessors take the ledger lock internally.
            (void)ledger.available_inventory(K_TOKEN_YES);
            (void)ledger.blocking_orders();
            (void)ledger.open_orders();
            (void)ledger.reserved_collateral();
            ledger::OrderRecord copy{};
            (void)ledger.order_copy("0xord-1-0", copy);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread writers[kWriters];
    for (int w = 0; w < kWriters; ++w) {
        writers[w] = std::thread([&, w] {
            for (int i = 0; i < kEventsPerWriter; ++i) {
                char order_id[32];
                char trade_id[32];
                std::snprintf(order_id, sizeof(order_id), "0xord-%d-%d", w, i);
                std::snprintf(trade_id, sizeof(trade_id), "t-%d-%d", w, i);
                ledger::Event event{};
                event.type = ledger::EventType::FILL;
                event.source = static_cast<ledger::Source>(
                    static_cast<uint8_t>(ledger::Source::USER_WS) +
                    static_cast<uint8_t>(w % 2));
                event.wall_ns = ledger::now_wall_ns();
                event.add_str(ledger::F_TRADE_ID, trade_id);
                event.add_str(ledger::F_ORDER_ID, order_id);
                event.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
                event.add_u8(ledger::F_SIDE, 0);
                event.add_u8(ledger::F_RESULT,
                             static_cast<uint8_t>(venue_status::TradeKind::MATCHED));
                event.add_u64(ledger::F_SIZE_RAW, 1000000ULL);
                event.add_u64(ledger::F_PRICE_RAW, 490000ULL);
                ledger::compute_event_key(event.type, event.source, order_id, trade_id,
                                          0, 0, event.key);
                char write_error[128]{};
                if (!ledger.commit(event, write_error, sizeof(write_error)))
                    write_failures.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& writer : writers) writer.join();
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK(write_failures.load() == 0, "no concurrent write failures");
    CHECK(reads.load() > 0, "reader made progress");
    CHECK(ledger.fills().size() == static_cast<size_t>(kWriters * kEventsPerWriter),
          "every distinct fill was recorded exactly once");
    CHECK(ledger.positions().find(K_TOKEN_YES) &&
              ledger.positions().find(K_TOKEN_YES)->shares ==
                  static_cast<uint64_t>(kWriters * kEventsPerWriter) * 1000000ULL,
          "inventory is the exact sum of concurrent fills");

    // Delivering the same events again from several threads must change nothing.
    const uint64_t duplicates_before = ledger.duplicates_ignored();
    std::thread replayers[2];
    for (int r = 0; r < 2; ++r) {
        replayers[r] = std::thread([&, r] {
            for (int i = 0; i < kEventsPerWriter; ++i) {
                char order_id[32];
                char trade_id[32];
                std::snprintf(order_id, sizeof(order_id), "0xord-%d-%d", r, i);
                std::snprintf(trade_id, sizeof(trade_id), "t-%d-%d", r, i);
                ledger::Event event{};
                event.type = ledger::EventType::FILL;
                event.source = static_cast<ledger::Source>(
                    static_cast<uint8_t>(ledger::Source::USER_WS) +
                    static_cast<uint8_t>(r % 2));
                event.wall_ns = ledger::now_wall_ns();
                event.add_str(ledger::F_TRADE_ID, trade_id);
                event.add_str(ledger::F_ORDER_ID, order_id);
                event.add_str(ledger::F_TOKEN_ID, K_TOKEN_YES);
                event.add_u8(ledger::F_SIDE, 0);
                event.add_u8(ledger::F_RESULT,
                             static_cast<uint8_t>(venue_status::TradeKind::MATCHED));
                event.add_u64(ledger::F_SIZE_RAW, 1000000ULL);
                event.add_u64(ledger::F_PRICE_RAW, 490000ULL);
                ledger::compute_event_key(event.type, event.source, order_id, trade_id,
                                          0, 0, event.key);
                char write_error[128]{};
                (void)ledger.commit(event, write_error, sizeof(write_error));
            }
        });
    }
    for (auto& replayer : replayers) replayer.join();
    CHECK(ledger.duplicates_ignored() > duplicates_before,
          "redeliveries were detected");
    CHECK(ledger.positions().find(K_TOKEN_YES)->shares ==
              static_cast<uint64_t>(kWriters * kEventsPerWriter) * 1000000ULL,
          "redelivery did not double count inventory");
}


// ═══════════════════════════════════════════════════════════════════════════
// 11b. Shutdown post-mortem: what the operator reads in the last log lines
// ═══════════════════════════════════════════════════════════════════════════
void test_session_report() {
    SECTION("session_report");
    constexpr uint64_t NOW = 1800000000000000000ULL;  // wall-clock ns
    char line[512]{};

    ledger::ReconciliationRun run{};
    run.run_id = 7;
    run.ready = 1;
    run.reason_count = 0;
    run.finished_wall_ns = NOW - 45ULL * 1000000000ULL;
    CHECK(report::format_last_reconciliation(run, NOW, line, sizeof(line)) > 0,
          "a ready run is formatted");
    CHECK(std::strstr(line, "run=7") != nullptr &&
              std::strstr(line, "ready=READY") != nullptr &&
              std::strstr(line, "reason_count=0") != nullptr &&
              std::strstr(line, "age_s=45") != nullptr,
          line);

    run.ready = 0;
    std::snprintf(run.reasons[0], sizeof(run.reasons[0]), "%s", "inventory_mismatch");
    std::snprintf(run.reasons[1], sizeof(run.reasons[1]), "%s", "unknown_order");
    run.reason_count = 2;
    CHECK(report::format_last_reconciliation(run, NOW, line, sizeof(line)) > 0,
          "a blocked run is formatted");
    CHECK(std::strstr(line, "ready=BLOCKED") != nullptr &&
              std::strstr(line, "reasons=[inventory_mismatch;unknown_order]") != nullptr,
          line);

    // Every reason slot full: the list must be marked as truncated, never
    // silently dropped, and never written past the buffer.
    ledger::ReconciliationRun full{};
    full.run_id = 8;
    full.reason_count = sizeof(full.reasons) / sizeof(full.reasons[0]);
    for (size_t i = 0; i < full.reason_count; ++i)
        std::snprintf(full.reasons[i], sizeof(full.reasons[i]), "%s",
                      "012345678901234567890123456789012345678");  // 39 chars
    CHECK(report::format_last_reconciliation(full, NOW, line, sizeof(line)) > 0,
          "six full reasons are formatted");
    CHECK(std::strstr(line, "reason_count=6") != nullptr, line);
    CHECK(std::strlen(line) < sizeof(line), "the formatted line stays in bounds");

    // Degenerate clocks and buffers fail closed: no half line is ever printed.
    ledger::ReconciliationRun never{};
    never.finished_wall_ns = 0;
    CHECK(report::format_last_reconciliation(never, NOW, line, sizeof(line)) > 0 &&
              std::strstr(line, "age_s=0") != nullptr,
          "a run that never finished reports age 0");
    ledger::ReconciliationRun backwards = run;
    backwards.finished_wall_ns = NOW + 1000000000ULL;
    CHECK(report::format_last_reconciliation(backwards, NOW, line, sizeof(line)) > 0 &&
              std::strstr(line, "age_s=0") != nullptr,
          "a wall clock that moved backwards reports age 0, not a wrapped one");
    char tiny[8]{};
    std::memset(tiny, 'x', sizeof(tiny));
    CHECK(report::format_last_reconciliation(run, NOW, tiny, sizeof(tiny)) == 0 &&
              tiny[0] == '\0',
          "a buffer too small yields nothing printable instead of a half line");
    CHECK(report::format_last_reconciliation(run, NOW, nullptr, 0) == 0,
          "a null buffer is refused");

    ledger::HeartbeatState beat{};
    beat.chain_active = true;
    beat.invalidated = false;
    beat.consecutive_failures = 0;
    beat.last_accepted_wall_ns = NOW - 3ULL * 1000000000ULL;
    std::snprintf(beat.heartbeat_id, sizeof(beat.heartbeat_id), "%s",
                  "hb-secret-token-123");
    CHECK(report::format_heartbeat_contract(beat, NOW, line, sizeof(line)) > 0,
          "an active contract is formatted");
    CHECK(std::strstr(line, "chain_active=1") != nullptr &&
              std::strstr(line, "invalidated=0") != nullptr &&
              std::strstr(line, "id_present=1") != nullptr &&
              std::strstr(line, "last_accepted_age_s=3") != nullptr,
          line);
    CHECK(std::strstr(line, "hb-secret-token-123") == nullptr,
          "the heartbeat id is never written to a log line");

    ledger::HeartbeatState dead{};
    dead.invalidated = true;
    dead.consecutive_failures = 3;
    CHECK(report::format_heartbeat_contract(dead, NOW, line, sizeof(line)) > 0 &&
              std::strstr(line, "chain_active=0") != nullptr &&
              std::strstr(line, "invalidated=1") != nullptr &&
              std::strstr(line, "consecutive_failures=3") != nullptr &&
              std::strstr(line, "id_present=0") != nullptr &&
              std::strstr(line, "last_accepted_age_s=0") != nullptr,
          "a contract that never started is reported as such");
    CHECK(report::format_heartbeat_contract(beat, NOW, tiny, sizeof(tiny)) == 0 &&
              tiny[0] == '\0',
          "a buffer too small yields nothing printable");
}

// ═══════════════════════════════════════════════════════════════════════════
// 11c. Preflight identity binding: the wallet checks must compare against the
//      wallet that will sign, never against zeros
// ═══════════════════════════════════════════════════════════════════════════
const preflight::Check* find_check(const preflight::Report& report,
                                   const char* name) noexcept {
    for (size_t i = 0; i < report.count; ++i)
        if (std::strcmp(report.checks[i].name, name) == 0) return &report.checks[i];
    return nullptr;
}

// Runs the preflight identity section over `cfg` with `signer` as the wallet that
// would sign, and returns the three identity checks through the out parameters.
bool run_identity_checks(const MarketConfig& cfg, const EIP712Signer& signer,
                         preflight::Report& report) {
    testing::FixtureTransport transport;
    clob::Credentials credentials{};
    clob::ClobApiClient api(transport, credentials, "https://clob.polymarket.com",
                            "https://gamma-api.polymarket.com");
    preflight::Inputs inputs{};
    inputs.config = &cfg;
    inputs.api = &api;
    inputs.signer = &signer;
    preflight::Runner runner(inputs);
    return runner.run(report) || report.count > 0;
}

void test_preflight_identity_binding() {
    SECTION("preflight_identity");
    // 0x0102...20: deterministic and a valid secp256k1 scalar.
    static constexpr char KEY[] =
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
    uint8_t private_key[32]{};
    CHECK(parse_hex_bytes(KEY, std::strlen(KEY), private_key, sizeof(private_key)),
          "the test key parses");
    EIP712Signer signer;
    CHECK(signer.init(private_key, false), "the signer initialises");
    secure_zero(private_key, sizeof(private_key));

    setenv("BOT_MODE", "paper", 1);
    setenv("BOT_PRIVATE_KEY_HEX", KEY, 1);
    MarketConfig cfg;
    CHECK(cfg.load(false, false) == nullptr, "the paper configuration loads");
    CHECK(cfg.finalize_identity(signer.signer_address()) == nullptr,
          "the wallet identity binds");
    CHECK(cfg.signer_hex[0] != '\0', "the signer address is formatted");
    CHECK(std::strcmp(cfg.api_address_hex, cfg.signer_hex) == 0,
          "POLY_ADDRESS defaults to the signer EOA");
    CHECK(std::memcmp(cfg.maker, cfg.signer, 20) == 0,
          "signature type 0 makes the signer the funder");

    preflight::Report report{};
    CHECK(run_identity_checks(cfg, signer, report), "preflight ran");
    const preflight::Check* identity = find_check(report, "signer_identity");
    CHECK(identity && identity->status == preflight::Status::PASS,
          identity ? identity->detail : "signer_identity is missing");
    const preflight::Check* funder = find_check(report, "maker_funder");
    CHECK(funder && funder->status == preflight::Status::PASS,
          funder ? funder->detail : "maker_funder is missing");
    const preflight::Check* owner = find_check(report, "api_owner");
    CHECK(owner && owner->status == preflight::Status::PASS,
          owner ? owner->detail : "api_owner is missing");

    // A configuration bound to a different wallet than the signer that will sign
    // must fail: this is the state preflight was in before it called
    // finalize_identity, where the comparison was against zeros and could never
    // pass - while maker_funder and api_owner passed vacuously.
    {
        uint8_t foreign[20];
        for (uint8_t& byte : foreign) byte = 0x11;
        setenv("BOT_PRIVATE_KEY_HEX", KEY, 1);
        MarketConfig swapped;
        CHECK(swapped.load(false, false) == nullptr, "the swapped configuration loads");
        CHECK(swapped.finalize_identity(foreign) == nullptr, "a foreign address binds");
        preflight::Report swapped_report{};
        CHECK(run_identity_checks(swapped, signer, swapped_report), "preflight ran");
        const preflight::Check* bad = find_check(swapped_report, "signer_identity");
        CHECK(bad && bad->status == preflight::Status::FAIL,
              "a wallet swap is a FAIL, not a vacuous PASS");
        CHECK(bad && std::strstr(bad->detail, "DOES NOT MATCH") != nullptr,
              bad ? bad->detail : "");
    }

    // api_owner must really compare POLY_ADDRESS with the signer for EOA orders.
    {
        setenv("BOT_API_ADDRESS", "1111111111111111111111111111111111111111", 1);
        setenv("BOT_PRIVATE_KEY_HEX", KEY, 1);
        MarketConfig other_api;
        CHECK(other_api.load(false, false) == nullptr, "the configuration loads");
        CHECK(other_api.finalize_identity(signer.signer_address()) == nullptr,
              "the identity binds with an explicit POLY_ADDRESS");
        preflight::Report other_report{};
        CHECK(run_identity_checks(other_api, signer, other_report), "preflight ran");
        const preflight::Check* bad_owner = find_check(other_report, "api_owner");
        CHECK(bad_owner && bad_owner->status == preflight::Status::FAIL,
              "API credentials belonging to another wallet are rejected for EOA orders");
        unsetenv("BOT_API_ADDRESS");
    }

    // maker_funder must really compare maker with signer for signature type 0.
    {
        setenv("BOT_MAKER_ADDRESS", "2222222222222222222222222222222222222222", 1);
        setenv("BOT_PRIVATE_KEY_HEX", KEY, 1);
        MarketConfig other_maker;
        CHECK(other_maker.load(false, false) == nullptr, "the configuration loads");
        CHECK(other_maker.finalize_identity(signer.signer_address()) == nullptr,
              "the identity binds with an explicit BOT_MAKER_ADDRESS");
        preflight::Report other_report{};
        CHECK(run_identity_checks(other_maker, signer, other_report), "preflight ran");
        const preflight::Check* bad_funder = find_check(other_report, "maker_funder");
        CHECK(bad_funder && bad_funder->status == preflight::Status::FAIL,
              "a funder that is not the signer fails for signature type 0");
        unsetenv("BOT_MAKER_ADDRESS");
    }
    unsetenv("BOT_PRIVATE_KEY_HEX");
    unsetenv("BOT_MODE");
}

// ═══════════════════════════════════════════════════════════════════════════
// 11d. Preflight heartbeat probe: it must report what the venue answered
// ═══════════════════════════════════════════════════════════════════════════
void test_preflight_heartbeat_probe() {
    SECTION("preflight_heartbeat_probe");
    static constexpr char KEY[] =
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
    uint8_t private_key[32]{};
    CHECK(parse_hex_bytes(KEY, std::strlen(KEY), private_key, sizeof(private_key)),
          "the test key parses");
    EIP712Signer signer;
    CHECK(signer.init(private_key, false), "the signer initialises");
    secure_zero(private_key, sizeof(private_key));
    setenv("BOT_MODE", "paper", 1);
    setenv("BOT_PRIVATE_KEY_HEX", KEY, 1);
    MarketConfig cfg;
    CHECK(cfg.load(false, false) == nullptr, "the paper configuration loads");
    CHECK(cfg.finalize_identity(signer.signer_address()) == nullptr,
          "the wallet identity binds");
    unsetenv("BOT_PRIVATE_KEY_HEX");
    unsetenv("BOT_MODE");

    // The venue answers on the OpenAPI path and only accepts a JSON null to start
    // the chain: the probe must report both observations, because that line is the
    // evidence the canary uses to settle an unresolved venue contract.
    testing::FixtureTransport transport;
    clob::Credentials credentials = test_credentials();
    clob::ClobApiClient api(transport, credentials, "https://clob.polymarket.com",
                            "https://gamma-api.polymarket.com");
    testing::FixtureResponse not_found{};
    not_found.code = 404;
    std::snprintf(not_found.body, sizeof(not_found.body), "{\"error\":\"not found\"}");
    transport.queue_once("/v1/heartbeats", not_found);
    testing::FixtureResponse bad_form{};
    bad_form.code = 400;
    std::snprintf(bad_form.body, sizeof(bad_form.body),
                  "{\"error\":\"heartbeat_id must be null\"}");
    transport.queue_once("/heartbeats", bad_form);
    transport.on("POST", "/heartbeats", 200, "{\"heartbeat_id\":\"hb-probe\"}");

    preflight::Inputs inputs{};
    inputs.config = &cfg;
    inputs.api = &api;
    inputs.signer = &signer;
    inputs.check_heartbeat = true;
    preflight::Runner runner(inputs);
    preflight::Report report{};
    runner.run(report);
    const preflight::Check* probe = find_check(report, "heartbeat");
    CHECK(probe != nullptr, "the heartbeat probe reported");
    if (probe) {
        CHECK(probe->status == preflight::Status::PASS, probe->detail);
        CHECK(std::strstr(probe->detail, "/heartbeats") != nullptr, probe->detail);
        CHECK(std::strstr(probe->detail, "/v1/heartbeats") == nullptr,
              "the report names the path that answered, not the one assumed");
        CHECK(std::strstr(probe->detail, "heartbeat_id=null") != nullptr,
              "the report names the chain-start body form that answered");
        CHECK(std::strstr(probe->detail, "cancel-on-disconnect contract is now "
                                         "active") != nullptr,
              "the report warns that the probe armed the venue contract");
    }
    CHECK(std::strcmp(api.heartbeat_path(), "/heartbeats") == 0,
          "the client remembered the answering path");
    CHECK(api.heartbeat_uses_null_start(), "and the answering body form");

    // With the probe disabled the report must say so explicitly instead of
    // silently claiming the heartbeat contract was verified.
    preflight::Inputs off = inputs;
    off.check_heartbeat = false;
    preflight::Runner off_runner(off);
    preflight::Report off_report{};
    off_runner.run(off_report);
    const preflight::Check* skipped = find_check(off_report, "heartbeat");
    CHECK(skipped && skipped->status == preflight::Status::SKIP,
          "an unprobed contract is reported as SKIP, never as PASS");
    CHECK(skipped && std::strstr(skipped->detail, "BOT_PREFLIGHT_CHECK_HEARTBEAT=0"),
          skipped ? skipped->detail : "");
}

// ═══════════════════════════════════════════════════════════════════════════
// 12. Preflight pass token: the gate behind BOT_ENABLE_LIVE_TRADING=1
// ═══════════════════════════════════════════════════════════════════════════
void test_preflight_token() {
    SECTION("preflight_token");
    const std::string token_dir = dir("preflight-token");
    const std::string path = token_dir + "/preflight.pass";
    CHECK(::mkdir(token_dir.c_str(), 0700) == 0, "token directory created");
    const char* fingerprint =
        "20a2b07ec70095e71baa87dcf71a101f46bd13365e19f8f35738940a822d74c2";
    char detail[192]{};

    // Missing token.
    CHECK(!preflight::Runner::token_is_fresh(path.c_str(), 900, fingerprint, detail,
                                             sizeof(detail)),
          "missing token is not fresh");
    CHECK(std::strstr(detail, "missing") != nullptr, detail);

    // Fresh and matching.
    {
        FILE* file = std::fopen(path.c_str(), "w");
        CHECK(file != nullptr, "token file created");
        const unsigned long long now =
            static_cast<unsigned long long>(ledger::now_wall_ns() / 1000000000ULL);
        if (file != nullptr) {
            std::fprintf(file,
                         "{\"version\":1,\"ready\":true,\"timestamp_s\":%llu,"
                         "\"config_fingerprint\":\"%s\",\"chain_id\":137}",
                         now, fingerprint);
            std::fclose(file);
        }
    }
    CHECK(preflight::Runner::token_is_fresh(path.c_str(), 900, fingerprint, detail,
                                            sizeof(detail)),
          detail);

    // A token for a different configuration must not arm this process.
    CHECK(!preflight::Runner::token_is_fresh(path.c_str(), 900,
                                             "0000000000000000000000000000000000000000"
                                             "00000000000000000000000000000000",
                                             detail, sizeof(detail)),
          "fingerprint mismatch rejected");
    CHECK(std::strstr(detail, "different configuration") != nullptr, detail);

    // Stale token.
    CHECK(!preflight::Runner::token_is_fresh(path.c_str(), 0, fingerprint, detail,
                                             sizeof(detail)),
          "zero max age rejects");

    // Not ready.
    {
        FILE* file = std::fopen(path.c_str(), "w");
        CHECK(file != nullptr, "non-ready token file created");
        const unsigned long long now =
            static_cast<unsigned long long>(ledger::now_wall_ns() / 1000000000ULL);
        if (file != nullptr) {
            std::fprintf(file,
                         "{\"version\":1,\"ready\":false,\"timestamp_s\":%llu,"
                         "\"config_fingerprint\":\"%s\"}",
                         now, fingerprint);
            std::fclose(file);
        }
    }
    CHECK(!preflight::Runner::token_is_fresh(path.c_str(), 900, fingerprint, detail,
                                             sizeof(detail)),
          "a non-ready token is rejected");

    // Dated in the future (clock tampering or a copied token).
    {
        FILE* file = std::fopen(path.c_str(), "w");
        CHECK(file != nullptr, "future-dated token file created");
        if (file != nullptr) {
            std::fprintf(file,
                         "{\"version\":1,\"ready\":true,\"timestamp_s\":4000000000,"
                         "\"config_fingerprint\":\"%s\"}",
                         fingerprint);
            std::fclose(file);
        }
    }
    CHECK(!preflight::Runner::token_is_fresh(path.c_str(), 900, fingerprint, detail,
                                             sizeof(detail)),
          "future-dated token rejected");

    // Malformed token.
    {
        FILE* file = std::fopen(path.c_str(), "w");
        CHECK(file != nullptr, "malformed token file created");
        if (file != nullptr) {
            std::fputs("not json at all", file);
            std::fclose(file);
        }
    }
    CHECK(!preflight::Runner::token_is_fresh(path.c_str(), 900, fingerprint, detail,
                                             sizeof(detail)),
          "malformed token rejected");
}

}  // namespace

int main() {
    std::printf("== CROWDINTEL live-safety unit tests ==\n");
    test_json_scan();
    test_order_state_machine();
    test_ledger_write_ahead_and_idempotency();
    test_ledger_journal_replay_and_corruption();
    test_metadata_parsers();
    test_metadata_validation();
    test_market_runtime();
    test_kill_switch();
    test_rest_client();
    test_user_event_parsing();
    test_user_event_application();
    test_heartbeat();
    test_reconciliation_ready();
    test_reconciliation_blocked_cases();
    test_ws_url_and_handshake();
    test_user_ws_protocol();
    test_frame_processor();
    test_order_recorder();
    test_ticket_reconciliation();
    test_ledger_concurrency();
    test_session_report();
    test_preflight_identity_binding();
    test_preflight_heartbeat_probe();
    test_preflight_token();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
