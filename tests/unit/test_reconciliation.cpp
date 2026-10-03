// Phase 5 unit tests: REST shapes, the reconciler and the READINESS gate.
//
// Fixtures are the payloads published by the official API reference
// (docs.polymarket.com "Get user orders", "Get single order by ID",
// "Get trades"), so a drifted parser fails here instead of at the venue.
// The GET signature vector was computed independently (Python hmac/hashlib)
// from the documented message: timestamp + "GET" + path (no query string).

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../../core/src/l2_auth.hpp"
#include "../../core/src/order_ledger.hpp"
#include "../../core/src/reconciliation.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__);      \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

constexpr const char* kOwner = "f4f247b7-4ac7-ff29-a152-04fda0a8755a";
constexpr const char* kCondition =
    "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75";
constexpr const char* kVenueId =
    "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b";
constexpr const char* kSecretB64 = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

// The open-order payload from the official reference, adapted only in that
// market/owner match this test's account so the reconciler keeps it.
const char* kOpenOrderJson = R"({
  "id": "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b",
  "status": "LIVE",
  "owner": "f4f247b7-4ac7-ff29-a152-04fda0a8755a",
  "maker_address": "0x1234567890123456789012345678901234567890",
  "market": "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
  "asset_id": "107505882767731489358349912513945399560393482969656700824895970500493757150417",
  "side": "BUY",
  "original_size": "100",
  "size_matched": "0",
  "price": "0.5",
  "outcome": "YES",
  "expiration": "1735689600",
  "order_type": "GTC",
  "associate_trades": [],
  "created_at": 1700000000
})";

void test_get_signature_vector() {
    std::printf("recon_get_signature_vector\n");
    uint8_t secret[l2auth::kSecretMaxBytes];
    size_t secret_len = 0;
    CHECK(l2auth::decode_secret(kSecretB64, secret, sizeof(secret), secret_len),
          "secret decodes");
    char signature[l2auth::kSignatureB64Chars + 1]{};
    size_t signature_len = 0;
    // Independently computed vector for timestamp + "GET" + "/data/orders".
    CHECK(l2auth::sign(secret, secret_len, "1700000000", "GET", "/data/orders",
                       nullptr, 0, signature, sizeof(signature),
                       signature_len) &&
              std::strcmp(signature,
                          "NTt0e8XQzUuVTici5whIU-0NfgRXijqf0FarNw-ik2Q=") == 0,
          "GET signature covers path only");
    CHECK(l2auth::sign(secret, secret_len, "1700000000", "GET",
                       "/auth/ban-status/closed-only", nullptr, 0, signature,
                       sizeof(signature), signature_len) &&
              std::strcmp(signature,
                          "-xPHWSv2gaTSXeSBGRhU6FQ534i9logtcR5YT2wgcNI=") == 0,
          "ban-status signature");
}

void test_open_order_parser() {
    std::printf("recon_open_order_parser\n");
    clob_info::VenueOrder order;
    CHECK(clob_info::parse_open_order(kOpenOrderJson,
                                      std::strlen(kOpenOrderJson), order),
          "reference payload parses");
    CHECK(std::strcmp(order.id, kVenueId) == 0, "id");
    CHECK(order.status == clob_info::VenueOrderStatus::kLive, "status LIVE");
    CHECK(order.side == clob_info::VenueSide::kBuy, "side BUY");
    CHECK(std::strcmp(order.market, kCondition) == 0, "market");
    CHECK(std::strcmp(order.owner, kOwner) == 0, "owner");
    CHECK(std::strcmp(order.order_type, "GTC") == 0, "order type");
    CHECK(std::strcmp(order.expiration, "1735689600") == 0, "expiration");
    CHECK(order.original_size_f6 == 100000000ull, "original_size 100 -> 1e8");
    CHECK(order.size_matched_f6 == 0, "size_matched 0");
    CHECK(order.price_f6 == 500000ull, "price 0.5");
    CHECK(order.created_at == 1700000000ull, "created_at");
    CHECK(order.has_associate_trades, "associate_trades seen");

    // Fractional decimal strings are exact at 1e-6.
    const char* fractional = R"({"id":"0x1","status":"LIVE","owner":"o",
      "maker_address":"0x0","market":"0x1","asset_id":"1","side":"SELL",
      "original_size":"10.5","size_matched":"0.000001","price":"0.000001",
      "expiration":"0","order_type":"GTD","created_at":1,"outcome":"NO"})";
    CHECK(clob_info::parse_open_order(fractional, std::strlen(fractional),
                                      order) &&
              order.original_size_f6 == 10500000ull &&
              order.size_matched_f6 == 1ull && order.price_f6 == 1ull &&
              order.side == clob_info::VenueSide::kSell,
          "fractional sizes are exact");

    // Fail-closed rejections.
    const char* missing = R"({"id":"0x1","status":"LIVE"})";
    CHECK(!clob_info::parse_open_order(missing, std::strlen(missing), order),
          "missing required fields rejected");
    const char* duplicate = R"({
      "id":"0x1","id":"0x2","status":"LIVE","owner":"o","maker_address":"0x0",
      "market":"0x1","asset_id":"1","side":"BUY","original_size":"1",
      "size_matched":"0","price":"0.5","expiration":"0","order_type":"GTC",
      "created_at":1,"outcome":"YES"})";
    CHECK(!clob_info::parse_open_order(duplicate, std::strlen(duplicate), order),
          "duplicate id rejected");
    const char* too_precise = R"({
      "id":"0x1","status":"LIVE","owner":"o","maker_address":"0x0","market":"0x1",
      "asset_id":"1","side":"BUY","original_size":"1.0000001",
      "size_matched":"0","price":"0.5","expiration":"0","order_type":"GTC",
      "created_at":1,"outcome":"YES"})";
    CHECK(!clob_info::parse_open_order(too_precise, std::strlen(too_precise),
                                       order),
          "value not exact at 1e-6 rejected");
    const char* trailing = R"({"id":"0x1"} {"id":"0x2"})";
    CHECK(!clob_info::parse_open_order(trailing, std::strlen(trailing), order),
          "trailing garbage rejected");

    // An unknown status is preserved as unknown, never defaulted.
    const char* unknown_status = R"({
      "id":"0x1","status":"WEIRD","owner":"o","maker_address":"0x0",
      "market":"0x1","asset_id":"1","side":"BUY","original_size":"1",
      "size_matched":"0","price":"0.5","expiration":"0","order_type":"GTC",
      "created_at":1,"outcome":"YES"})";
    CHECK(clob_info::parse_open_order(unknown_status,
                                      std::strlen(unknown_status), order) &&
              order.status == clob_info::VenueOrderStatus::kUnknown,
          "unknown status is not defaulted");
    CHECK(std::strcmp(clob_info::venue_order_status_name(
                          clob_info::VenueOrderStatus::kCanceledMarketResolved),
                      "CANCELED_MARKET_RESOLVED") == 0,
          "status name");
}

void test_envelope_and_trades() {
    std::printf("recon_envelope_and_trades\n");
    const std::string envelope =
        std::string(R"({"limit":100,"next_cursor":"MTAw","count":1,"data":[)") +
        kOpenOrderJson + "]}";
    char cursor[64];
    bool has_cursor = false;
    CHECK(clob_info::page_cursor(envelope.c_str(), envelope.size(), cursor,
                                 sizeof(cursor), has_cursor) &&
              has_cursor && std::strcmp(cursor, "MTAw") == 0,
          "cursor parsed");
    const char* no_cursor = R"({"limit":100,"count":0,"data":[]})";
    has_cursor = true;
    CHECK(clob_info::page_cursor(no_cursor, std::strlen(no_cursor), cursor,
                                 sizeof(cursor), has_cursor) &&
              !has_cursor,
          "missing cursor means last page");
    const char* null_cursor = R"({"next_cursor":null})";
    has_cursor = true;
    CHECK(clob_info::page_cursor(null_cursor, std::strlen(null_cursor), cursor,
                                 sizeof(cursor), has_cursor) &&
              !has_cursor,
          "null cursor means last page");
    const char* bad_cursor = R"({"next_cursor":7})";
    CHECK(!clob_info::page_cursor(bad_cursor, std::strlen(bad_cursor), cursor,
                                  sizeof(cursor), has_cursor),
          "a numeric cursor is malformed, never 'last page'");

    // The venue documents two spellings of the same page envelope
    // ({data,next_cursor,count,limit} and {items,has_more,next_cursor}): both
    // are accepted, and a payload with both arrays at once is ambiguous.
    const std::string items_envelope =
        std::string(R"({"items":[)") + kOpenOrderJson +
        R"(],"has_more":false,"next_cursor":null})";
    size_t items_seen = 0;
    int items_context = 0;
    CHECK(clob_info::for_each_page_element(
              items_envelope.c_str(), items_envelope.size(), items_context,
              [&](const clob_info::Span& element, int&) {
                  clob_info::VenueOrder parsed;
                  if (!clob_info::parse_open_order(element.begin,
                                                   element.size(), parsed))
                      return false;
                  ++items_seen;
                  return true;
              }) &&
              items_seen == 1,
          "items envelope walked");
    bool more_present = false;
    bool more_value = true;
    CHECK(clob_info::has_more_field(items_envelope.c_str(),
                                    items_envelope.size(), more_present,
                                    more_value) &&
              more_present && !more_value,
          "has_more read");
    const char* no_more_field = R"({"data":[]})";
    CHECK(clob_info::has_more_field(no_more_field, std::strlen(no_more_field),
                                    more_present, more_value) &&
              !more_present,
          "absent has_more is legal");
    const char* bad_more = R"({"data":[],"has_more":"yes"})";
    CHECK(!clob_info::has_more_field(bad_more, std::strlen(bad_more),
                                     more_present, more_value),
          "a non-boolean has_more is malformed");
    const char* both_arrays = R"({"data":[],"items":[]})";
    int sink_context = 0;
    CHECK(!clob_info::for_each_page_element(
              both_arrays, std::strlen(both_arrays), sink_context,
              [](const clob_info::Span&, int&) { return true; }),
          "both array spellings at once are ambiguous");

    size_t seen = 0;
    int context = 0;
    CHECK(clob_info::for_each_array_element(
              envelope.c_str(), envelope.size(), "data", context,
              [&](const clob_info::Span& element, int&) {
                  clob_info::VenueOrder parsed;
                  if (!clob_info::parse_open_order(element.begin,
                                                   element.size(), parsed))
                      return false;
                  ++seen;
                  return true;
              }) &&
              seen == 1,
          "envelope element walked");

    const char* closed = R"({"closed_only":true})";
    bool closed_only = false;
    CHECK(clob_info::parse_closed_only(closed, std::strlen(closed),
                                       closed_only) &&
              closed_only,
          "closed_only parsed");
    const char* opened = R"({"closed_only":false})";
    CHECK(clob_info::parse_closed_only(opened, std::strlen(opened),
                                       closed_only) &&
              !closed_only,
          "closed_only false");
    CHECK(!clob_info::parse_closed_only(opened, 5, closed_only),
          "truncated json rejected");

    const char* trade = R"({
      "id": "trade-123",
      "taker_order_id": "0xABCDEF1234567890abcdef1234567890abcdef12",
      "market": "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
      "asset_id": "15871154585880608648532107628464183779895785213830018178010423617714102767076",
      "side": "BUY", "size": "10", "price": "0.5",
      "status": "TRADE_STATUS_CONFIRMED", "match_time": "1700000000",
      "owner": "f4f247b7-4ac7-ff29-a152-04fda0a8755a",
      "trader_side": "TAKER",
      "maker_orders": [
        {"order_id":"0x1111111111111111111111111111111111111111111111111111111111111111",
         "owner":"f4f247b7-4ac7-ff29-a152-04fda0a8755a","matched_amount":"5","price":"0.5"},
        {"order_id":"0x2222222222222222222222222222222222222222222222222222222222222222",
         "owner":"someone-else","matched_amount":"5","price":"0.5"}
      ]
    })";
    clob_info::VenueTrade parsed_trade;
    CHECK(clob_info::parse_trade(trade, std::strlen(trade), parsed_trade, kOwner),
          "trade parses");
    CHECK(parsed_trade.status == clob_info::TradeStatus::kConfirmed, "status");
    CHECK(std::strcmp(parsed_trade.size_text, "10") == 0 &&
              parsed_trade.price_f6 == 500000ull,
          "trade size kept verbatim, price is decimal");
    CHECK(parsed_trade.maker_orders == 2 && parsed_trade.maker_orders_kept == 2 &&
              parsed_trade.maker_orders_ours == 1 &&
              !parsed_trade.maker_orders_truncated,
          "maker attribution");
    CHECK(clob_info::venue_id_equal(
              parsed_trade.taker_order_id,
              "0xabcdef1234567890ABCDEF1234567890abcdef12"),
          "venue ids compare case-insensitively");
    CHECK(!clob_info::venue_id_equal("0x1", "0x01"), "different lengths differ");
}

// ── scripted transport ───────────────────────────────────────────────────────

struct ScriptedTransport {
    static constexpr size_t kBodyCap = 8192;
    struct Route {
        std::string path;
        long http_code = 200;
        std::string body;
        bool transport_ok = true;
    };
    std::vector<Route> routes;
    std::vector<std::string> requests;
    size_t next_route = 0;
    bool usable_ = true;
    std::string last_error_ = "scripted failure";

    bool usable() const noexcept { return usable_; }
    const char* last_error() const noexcept { return last_error_.c_str(); }

    bool get(const char* path, const char* query, long& http_code, char* out,
             size_t out_cap, size_t& out_len) {
        requests.push_back(std::string(path) + "?" + (query ? query : ""));
        if (next_route >= routes.size()) {
            last_error_ = "no scripted route";
            return false;
        }
        const Route& route = routes[next_route++];
        if (route.path != path) {
            last_error_ = "unexpected path: " + std::string(path);
            return false;
        }
        if (!route.transport_ok) return false;
        http_code = route.http_code;
        out_len = route.body.size() < out_cap - 1 ? route.body.size()
                                                  : out_cap - 1;
        std::memcpy(out, route.body.data(), out_len);
        out[out_len] = '\0';
        return true;
    }
};

struct Fixture {
    std::string dir;
    std::string path;
    cledger::OrderLedger ledger;
    MarketConfig cfg;

    Fixture() {
        char templ[] = "/tmp/crowdintel-recon-XXXXXX";
        const char* created = ::mkdtemp(templ);
        dir = created ? created : "/tmp";
        path = dir + "/orders.journal";
        cledger::OpenOptions options;
        options.fsync_records = false;
        char error[192];
        if (!ledger.open(path.c_str(), options, error, sizeof(error)))
            std::printf("FAIL: ledger opens (%s)\n", error);
        std::snprintf(cfg.owner_api_key, sizeof(cfg.owner_api_key), "%s", kOwner);
        std::snprintf(cfg.runtime.condition_id, sizeof(cfg.runtime.condition_id),
                      "%s", kCondition);
    }

    ~Fixture() {
        ledger.close();
        ::unlink(path.c_str());
        ::rmdir(dir.c_str());
    }

    // Journals an intent and returns its client order id.
    std::string add_intent(int n) {
        char error[192];
        cledger::IntentRecord intent{};
        std::snprintf(intent.client_order_id, sizeof(intent.client_order_id),
                      "recon-%d", n);
        intent.signal_id = static_cast<uint64_t>(n);
        intent.market_hash = 42;
        intent.price_fixed6 = 500000;
        intent.shares_fixed6 = 100000000;
        intent.notional_fixed6 = 50000000;
        intent.side = K_SIDE_BUY;
        intent.order_type = 1;
        if (!ledger.record_intent(intent, error, sizeof(error)))
            std::printf("FAIL: intent (%s)\n", error);
        return intent.client_order_id;
    }

    bool ack(const char* client_id, const char* venue_id) {
        char error[192];
        return ledger
                   .record_transition(client_id, cledger::LedgerEvent::kSubmitAck,
                                      cledger::Evidence::kVenueAck, venue_id, 0,
                                      error, sizeof(error))
                   .result == cledger::TransitionResult::kApplied;
    }

    bool live(const char* client_id) {
        char error[192];
        return ledger
                   .record_transition(client_id, cledger::LedgerEvent::kOrderLive,
                                      cledger::Evidence::kVenueChannel, nullptr, 0,
                                      error, sizeof(error))
                   .result == cledger::TransitionResult::kApplied;
    }
};

std::string orders_page(const std::string& orders, const char* cursor) {
    return std::string("{\"limit\":100,\"next_cursor\":") +
           (cursor ? std::string("\"") + cursor + "\"" : std::string("null")) +
           ",\"count\":1,\"data\":[" + orders + "]}";
}

void test_reconciler_ready_and_absent() {
    std::printf("recon_reconciler_ready\n");
    Fixture fixture;
    const std::string client_id = fixture.add_intent(1);
    CHECK(fixture.ack(client_id.c_str(), kVenueId), "ack");
    CHECK(fixture.live(client_id.c_str()), "live");
    CHECK(!fixture.ledger.gate_open(), "live order blocks trading");

    ScriptedTransport transport;
    transport.routes = {
        {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
        {"/data/orders", 200, orders_page(kOpenOrderJson, nullptr)},
        {"/data/trades", 200, R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
    };
    recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
    char error[192] = "stale";
    const recon::Result result = reconciler.run(fixture.ledger, error,
                                                sizeof(error));
    CHECK(result == recon::Result::kReady, "reconciliation is READY");
    CHECK(std::strcmp(error, "") == 0, "no error text");
    CHECK(fixture.ledger.find(client_id.c_str())->state ==
              cledger::OrderState::kLive,
          "order stays LIVE");
    // A resting order is proven but not terminal, so the arming gate stays
    // closed and the readiness reason is explicit about which case it is.
    CHECK(!fixture.ledger.gate_open(), "resting order keeps the gate closed");
    CHECK(reconciler.report().orders_open == 1, "counted as proven open");
    recon::ReadinessInputs gate;
    gate.gate_open = false;
    gate.open_orders = reconciler.report().orders_open;
    CHECK(recon::evaluate_readiness(gate) ==
              recon::ReadinessReason::kOpenOrdersPresent,
          "readiness says open_orders");
    gate.unknown_orders = 1;
    CHECK(recon::evaluate_readiness(gate) ==
              recon::ReadinessReason::kUnreconciledOrders,
          "an unproven order outranks a proven one");
    CHECK(reconciler.report().live_orders == 1 &&
              reconciler.report().orders_present == 1 &&
              reconciler.report().unattributed_trades == 0,
          "counters");
    CHECK(transport.requests.size() == 3, "exactly three requests");

    // The reconcile is idempotent.
    transport.next_route = 0;
    transport.requests.clear();
    CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
              recon::Result::kReady,
          "second run stays READY");
}

void test_reconciler_canceled_and_absent() {
    std::printf("recon_reconciler_canceled_absent\n");
    // A canceled order keeps its partial fill; a 404 proves absence.
    Fixture fixture;
    const std::string canceled = fixture.add_intent(1);
    const std::string missing = fixture.add_intent(2);
    CHECK(fixture.ack(canceled.c_str(), kVenueId), "ack 1");
    CHECK(fixture.ack(missing.c_str(),
                      "0x9999999999999999999999999999999999999999999999999999999999999999"),
          "ack 2");

    std::string canceled_body = kOpenOrderJson;
    // CANCELED with a partial fill: the fill must survive the transition.
    const std::string from = "\"status\": \"LIVE\"";
    const std::string to = "\"status\": \"CANCELED\"";
    canceled_body.replace(canceled_body.find(from), from.size(), to);
    const std::string zero = "\"size_matched\": \"0\"";
    const std::string partial = "\"size_matched\": \"40\"";
    canceled_body.replace(canceled_body.find(zero), zero.size(), partial);

    ScriptedTransport transport;
    transport.routes = {
        {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
        {"/data/orders", 200,
         R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
        {"/data/trades", 200,
         R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
        {"/data/order/" + std::string(kVenueId), 200, canceled_body},
        {"/data/order/0x9999999999999999999999999999999999999999999999999999999999999999",
         404, R"({"error":"Order not found"})"},
    };
    recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
    char error[192];
    CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
              recon::Result::kReady,
          "READY after proving both");
    const cledger::OrderSummary* canceled_summary =
        fixture.ledger.find(canceled.c_str());
    CHECK(canceled_summary->state == cledger::OrderState::kCanceled,
          "canceled at the venue");
    CHECK(canceled_summary->filled_fixed6 == 40000000ull,
          "partial fill preserved through cancellation");
    CHECK(fixture.ledger.find(missing.c_str())->state ==
              cledger::OrderState::kRejected,
          "404 proves the order is not held by the venue");
    CHECK(fixture.ledger.gate_open(), "gate opens once everything is terminal");
}

void test_reconciler_blocks() {
    std::printf("recon_reconciler_blocks\n");
    char error[192] = "";

    // (a) an order the journal never saw
    {
        Fixture fixture;
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
            {"/data/orders", 200, orders_page(kOpenOrderJson, nullptr)},
            {"/data/trades", 200,
             R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kBlocked,
              "untracked live order blocks");
        CHECK(reconciler.report().untracked_live_orders == 1, "counter");
        CHECK(std::strstr(error, "does not track") != nullptr, "reason text");
    }
    // (b) closed-only account
    {
        Fixture fixture;
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":true})"},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kBlocked,
              "closed-only blocks");
        CHECK(reconciler.report().closed_only, "flag recorded");
        CHECK(recon::evaluate_readiness(recon::ReadinessInputs{false, true, true,
                                                              true, true, true,
                                                              true}) ==
                  recon::ReadinessReason::kMetadataUnresolved,
              "readiness priority");
        recon::ReadinessInputs inputs;
        inputs.closed_only = true;
        CHECK(recon::evaluate_readiness(inputs) ==
                  recon::ReadinessReason::kClosedOnlyAccount,
              "readiness sees closed-only");
    }
    // (c) an order that cannot be queried (ambiguous submit, no venue id)
    {
        Fixture fixture;
        const std::string client_id = fixture.add_intent(1);
        char local[192];
        CHECK(fixture.ledger
                  .record_transition(client_id.c_str(),
                                     cledger::LedgerEvent::kSubmitAmbiguous,
                                     cledger::Evidence::kNone, nullptr, 0, local,
                                     sizeof(local))
                  .result == cledger::TransitionResult::kApplied,
              "ambiguous submit");
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
            {"/data/orders", 200,
             R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
            {"/data/trades", 200,
             R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kBlocked,
              "unqueryable order blocks");
        CHECK(reconciler.report().orders_unqueryable == 1, "counter");
        CHECK(fixture.ledger.find(client_id.c_str())->state ==
                  cledger::OrderState::kUnknown,
          "state stays UNKNOWN");
    }
    // (d) an unattributed trade
    {
        Fixture fixture;
        const char* trade = R"({
          "id":"trade-1","taker_order_id":"0x7777777777777777777777777777777777777777777777777777777777777777",
          "market":"0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
          "asset_id":"1","side":"BUY","size":"1","price":"0.5",
          "status":"TRADE_STATUS_CONFIRMED","match_time":"1700000000",
          "owner":"f4f247b7-4ac7-ff29-a152-04fda0a8755a","maker_orders":[]})";
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
            {"/data/orders", 200,
             R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
            {"/data/trades", 200, orders_page(trade, nullptr)},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kBlocked,
              "unattributed trade blocks");
        CHECK(reconciler.report().unattributed_trades == 1, "counter");
    }
    // (e) transport failure: the venue was never asked
    {
        Fixture fixture;
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 0, "", false},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kTransportError,
              "transport failure is not READY");
    }
    // (f) undocumented status
    {
        Fixture fixture;
        const std::string client_id = fixture.add_intent(1);
        CHECK(fixture.ack(client_id.c_str(), kVenueId), "ack");
        std::string weird = kOpenOrderJson;
        const std::string from = "\"status\": \"LIVE\"";
        weird.replace(weird.find(from), from.size(),
                      "\"status\": \"SOMETHING_NEW\"");
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
            {"/data/orders", 200, orders_page(weird, nullptr)},
            {"/data/trades", 200,
             R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
            {"/data/order/" + std::string(kVenueId), 200, weird},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kBlocked,
              "undocumented status blocks");
        // The reconciler never downgrades a state it could not prove: the
        // order stays where the journal left it, and nothing new is asserted.
        CHECK(fixture.ledger.find(client_id.c_str())->state ==
                  cledger::OrderState::kSubmitted,
              "order keeps its journaled state");
    }
    // (g) a terminal journaled order that the venue still lists as live
    {
        Fixture fixture;
        const std::string client_id = fixture.add_intent(1);
        CHECK(fixture.ack(client_id.c_str(), kVenueId), "ack");
        char local[192];
        CHECK(fixture.ledger
                  .record_transition(client_id.c_str(),
                                     cledger::LedgerEvent::kOrderCanceled,
                                     cledger::Evidence::kVenueChannel, nullptr, 0,
                                     local, sizeof(local))
                  .result == cledger::TransitionResult::kApplied,
              "canceled locally");
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
            {"/data/orders", 200, orders_page(kOpenOrderJson, nullptr)},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kBlocked,
              "terminal/live contradiction blocks");
        CHECK(reconciler.report().terminal_conflicts == 1, "counter");
    }
    // (h) live order of another market for the same account
    {
        Fixture fixture;
        std::string other = kOpenOrderJson;
        const std::string from = std::string("\"market\": \"") + kCondition + "\"";
        other.replace(other.find(from), from.size(),
                      "\"market\": \"0x0000000000000000000000000000000000000000000000000000000000000002\"");
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
            {"/data/orders", 200, orders_page(other, nullptr)},
            {"/data/trades", 200,
             R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kBlocked,
              "foreign market blocks");
        CHECK(reconciler.report().foreign_live_orders == 1, "counter");
    }
}

void test_reconciler_pagination_and_present_by_id() {
    std::printf("recon_pagination_and_present_by_id\n");
    Fixture fixture;
    const std::string client_id = fixture.add_intent(1);
    CHECK(fixture.ack(client_id.c_str(), kVenueId), "ack");
    CHECK(fixture.live(client_id.c_str()), "live");

    // Page 1 has a cursor; the order shows up on page 2 with a partial fill.
    std::string partial = kOpenOrderJson;
    const std::string zero = "\"size_matched\": \"0\"";
    partial.replace(partial.find(zero), zero.size(),
                    "\"size_matched\": \"25\"");
    ScriptedTransport transport;
    transport.routes = {
        {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
        {"/data/orders", 200, orders_page("", "MTAw")},
        {"/data/orders", 200, orders_page(partial, nullptr)},
        {"/data/trades", 200,
         R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
    };
    recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
    char error[192];
    CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
              recon::Result::kReady,
          "paginated reconciliation is READY");
    CHECK(reconciler.report().order_pages == 2, "two order pages");
    CHECK(fixture.ledger.find(client_id.c_str())->state ==
              cledger::OrderState::kPartiallyFilled,
          "partial fill state");
    CHECK(fixture.ledger.find(client_id.c_str())->filled_fixed6 == 25000000ull,
          "partial fill recorded");
    CHECK(reconciler.report().orders_open == 1, "still open at the venue");
    // The cursor must travel as a query parameter, never in the signed path.
    CHECK(transport.requests.size() == 4 &&
              transport.requests[2] == "/data/orders?next_cursor=MTAw",
          "cursor passed as query");

    // A fully matched order resolves to FILLED through the by-id route.
    Fixture second;
    const std::string second_id = second.add_intent(1);
    CHECK(second.ack(second_id.c_str(), kVenueId), "ack");
    std::string matched = kOpenOrderJson;
    const std::string live_status = "\"status\": \"LIVE\"";
    matched.replace(matched.find(live_status), live_status.size(),
                    "\"status\": \"MATCHED\"");
    matched.replace(matched.find(zero), zero.size(), "\"size_matched\": \"100\"");
    ScriptedTransport by_id;
    by_id.routes = {
        {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
        {"/data/orders", 200,
         R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
        {"/data/trades", 200,
         R"({"limit":100,"next_cursor":null,"count":0,"data":[]})"},
        {"/data/order/" + std::string(kVenueId), 200, matched},
    };
    recon::Reconciler<ScriptedTransport> second_reconciler(by_id, second.cfg);
    CHECK(second_reconciler.run(second.ledger, error, sizeof(error)) ==
              recon::Result::kReady,
          "by-id reconciliation is READY");
    CHECK(second.ledger.find(second_id.c_str())->state ==
              cledger::OrderState::kFilled,
          "MATCHED resolves to FILLED");
    CHECK(second.ledger.gate_open(), "gate opens");
}

// The two documented spellings of the page envelope must work end to end, and
// a page that claims more pages without a cursor must block instead of being
// silently truncated.
void test_page_envelope_variants() {
    std::printf("recon_page_envelope_variants\n");
    const auto items_page = [](const std::string& orders, const char* cursor,
                               bool has_more) {
        return std::string("{\"items\":[") + orders + "],\"has_more\":" +
               (has_more ? "true" : "false") + ",\"next_cursor\":" +
               (cursor ? std::string("\"") + cursor + "\"" : "null") + "}";
    };
    {
        Fixture fixture;
        const std::string client_id = fixture.add_intent(1);
        CHECK(fixture.ack(client_id.c_str(), kVenueId), "ack");
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
            {"/data/orders", 200, items_page(kOpenOrderJson, "MTAw", true)},
            {"/data/orders", 200, items_page(kOpenOrderJson, nullptr, false)},
            {"/data/trades", 200, R"({"items":[],"has_more":false})"},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        char error[192];
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kReady,
              "items envelope is READY");
        CHECK(reconciler.report().order_pages == 2, "both pages walked");
        CHECK(fixture.ledger.find(client_id.c_str())->state ==
                  cledger::OrderState::kLive,
              "order proven by the items envelope");
    }
    {
        Fixture fixture;
        const std::string client_id = fixture.add_intent(1);
        CHECK(fixture.ack(client_id.c_str(), kVenueId), "ack");
        ScriptedTransport transport;
        transport.routes = {
            {"/auth/ban-status/closed-only", 200, R"({"closed_only":false})"},
            {"/data/orders", 200, items_page("", "MTAw", true)},
            {"/data/orders", 200, R"({"items":[],"has_more":true})"},
        };
        recon::Reconciler<ScriptedTransport> reconciler(transport, fixture.cfg);
        char error[192];
        CHECK(reconciler.run(fixture.ledger, error, sizeof(error)) ==
                  recon::Result::kBlocked,
              "has_more without a cursor blocks");
        CHECK(std::strstr(error, "without a cursor") != nullptr,
              "reason names the missing cursor");
        CHECK(reconciler.report().malformed_payloads == 1, "counted malformed");
        CHECK(fixture.ledger.find(client_id.c_str())->state ==
                  cledger::OrderState::kSubmitted,
              "nothing is assumed from a truncated page");
    }
}

void test_readiness_gate() {
    std::printf("recon_readiness_gate\n");
    recon::ReadinessInputs inputs;
    CHECK(recon::evaluate_readiness(inputs) == recon::ReadinessReason::kReady,
          "all requirements met");
    inputs.gate_open = false;
    inputs.open_orders = 1;
    CHECK(recon::evaluate_readiness(inputs) ==
              recon::ReadinessReason::kOpenOrdersPresent,
          "proven open orders");
    inputs.unknown_orders = 1;
    CHECK(recon::evaluate_readiness(inputs) ==
              recon::ReadinessReason::kUnreconciledOrders,
          "unreconciled orders");
    inputs.unknown_orders = 0;
    inputs.open_orders = 0;
    inputs.gate_open = true;
    inputs.account_state_ok = false;
    CHECK(recon::evaluate_readiness(inputs) ==
              recon::ReadinessReason::kAccountStateUnproven,
          "unproven balance/allowance blocks arming");
    inputs.account_state_ok = true;
    inputs.reconciliation_ok = false;
    CHECK(recon::evaluate_readiness(inputs) ==
              recon::ReadinessReason::kReconciliationFailed,
          "reconciliation failed");
    inputs.reconciliation_ok = true;
    inputs.user_channel_configured = false;
    CHECK(recon::evaluate_readiness(inputs) ==
              recon::ReadinessReason::kUserChannelUnconfigured,
          "user channel");
    inputs.user_channel_configured = true;
    inputs.heartbeat_configured = false;
    CHECK(recon::evaluate_readiness(inputs) ==
              recon::ReadinessReason::kHeartbeatUnconfigured,
          "heartbeat");
    inputs.heartbeat_configured = true;
    inputs.ledger_open = false;
    CHECK(recon::evaluate_readiness(inputs) ==
              recon::ReadinessReason::kLedgerUnavailable,
          "ledger");
    CHECK(std::strcmp(recon::result_name(recon::Result::kTransportError),
                      "TRANSPORT_ERROR") == 0,
          "result names");
}

#if defined(CROWDINTEL_HAVE_NETWORK)
void test_rest_transport_guards() {
    std::printf("recon_rest_transport_guards\n");
    l2auth::Credentials credentials{"api-key", "0x1234567890123456789012345678901234567890",
                                    kSecretB64, "pass"};
    recon::RestTransport transport("https://clob.polymarket.com", credentials, "");
    CHECK(transport.usable(), "transport is usable with a valid secret");
    l2auth::Credentials bad{"api-key", "0x1234567890123456789012345678901234567890",
                            "%%%not-base64%%%", "pass"};
    recon::RestTransport broken("https://clob.polymarket.com", bad, "");
    CHECK(!broken.usable(), "undecodable secret refuses to send");
    long http_code = 0;
    char body[128];
    size_t length = 0;
    CHECK(!broken.get("/data/orders", "", http_code, body, sizeof(body),
                      length),
          "broken transport never sends");
}
#endif

}  // namespace

int main() {
    std::printf("== CROWDINTEL reconciliation tests ==\n");
    test_get_signature_vector();
    test_open_order_parser();
    test_envelope_and_trades();
    test_reconciler_ready_and_absent();
    test_reconciler_canceled_and_absent();
    test_reconciler_blocks();
    test_reconciler_pagination_and_present_by_id();
    test_page_envelope_variants();
    test_readiness_gate();
#if defined(CROWDINTEL_HAVE_NETWORK)
    test_rest_transport_guards();
#endif
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
