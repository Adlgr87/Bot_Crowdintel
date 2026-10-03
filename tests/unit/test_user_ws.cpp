// Phase 3 unit tests: user-data channel parsing, transport codec, and the
// event→ledger applier.
//
// Fixtures marked "spec" are copied verbatim from the Polymarket AsyncAPI User
// Channel document (docs.polymarket.com/asyncapi-user.json); nothing here
// invents field names.

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../../alpha/crowdintel/alpha_parser.hpp"
#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/src/execution_engine.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/mock_client.hpp"
#include "../../core/src/order_ledger.hpp"
#include "../../core/src/presigned_pool.hpp"
#include "../../core/src/user_event_applier.hpp"
#include "../../core/src/user_ws_client.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__);      \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

constexpr const char* kCondition =
    "0xbd31dc8a20211944f6b70f31557f1001557b59905b7738480ca09bd4532f84af";
constexpr const char* kAsset =
    "52114319501245915516055106046884209969926127482827954674443846427813813222426";
constexpr const char* kVenueOrder =
    "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b";

// ── spec fixtures ────────────────────────────────────────────────────────────

const char* kOrderPlacement = R"({
  "event_type": "order",
  "id": "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b",
  "owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
  "market": "0xbd31dc8a20211944f6b70f31557f1001557b59905b7738480ca09bd4532f84af",
  "asset_id": "52114319501245915516055106046884209969926127482827954674443846427813813222426",
  "side": "SELL",
  "order_owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
  "original_size": "10",
  "size_matched": "0",
  "price": "0.57",
  "associate_trades": null,
  "outcome": "YES",
  "type": "PLACEMENT",
  "created_at": "1672290687",
  "expiration": "1234567",
  "order_type": "GTD",
  "status": "LIVE",
  "maker_address": "0x1234...",
  "timestamp": "1672290687"
})";

const char* kOrderCancellation = R"({
  "event_type": "order",
  "id": "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b",
  "owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
  "market": "0xbd31dc8a20211944f6b70f31557f1001557b59905b7738480ca09bd4532f84af",
  "asset_id": "52114319501245915516055106046884209969926127482827954674443846427813813222426",
  "side": "SELL",
  "original_size": "10",
  "size_matched": "5",
  "price": "0.57",
  "associate_trades": ["trade-id-1"],
  "outcome": "YES",
  "type": "CANCELLATION",
  "created_at": "1672290687",
  "expiration": "1234567",
  "order_type": "GTD",
  "status": "CANCELED",
  "maker_address": "0x1234...",
  "timestamp": "1672295000"
})";

const char* kTradeMatched = R"({
  "event_type": "trade",
  "type": "TRADE",
  "id": "28c4d2eb-bbea-40e7-a9f0-b2fdb56b2c2e",
  "taker_order_id": "0x06bc63e346ed4ceddce9efd6b3af37c8f8f440c92fe7da6b2d0f9e4ccbc50c42",
  "market": "0xbd31dc8a20211944f6b70f31557f1001557b59905b7738480ca09bd4532f84af",
  "asset_id": "52114319501245915516055106046884209969926127482827954674443846427813813222426",
  "side": "BUY",
  "size": "10",
  "price": "0.57",
  "fee_rate_bps": "0",
  "status": "MATCHED",
  "match_time": "1672290701",
  "last_update": "1672290701",
  "outcome": "YES",
  "owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
  "trade_owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
  "maker_address": "0x1234...",
  "transaction_hash": "",
  "bucket_index": 0,
  "maker_orders": [
    {
      "order_id": "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b",
      "owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
      "maker_address": "0x5678...",
      "matched_amount": "10",
      "price": "0.57",
      "fee_rate_bps": "0",
      "asset_id": "52114319501245915516055106046884209969926127482827954674443846427813813222426",
      "outcome": "YES",
      "side": "SELL"
    }
  ],
  "trader_side": "TAKER",
  "timestamp": "1672290701"
})";

const char* kTradeConfirmed = R"({
  "event_type": "trade",
  "type": "TRADE",
  "id": "28c4d2eb-bbea-40e7-a9f0-b2fdb56b2c2e",
  "taker_order_id": "0x06bc63e346ed4ceddce9efd6b3af37c8f8f440c92fe7da6b2d0f9e4ccbc50c42",
  "market": "0xbd31dc8a20211944f6b70f31557f1001557b59905b7738480ca09bd4532f84af",
  "asset_id": "52114319501245915516055106046884209969926127482827954674443846427813813222426",
  "side": "BUY",
  "size": "10",
  "price": "0.57",
  "fee_rate_bps": "0",
  "status": "CONFIRMED",
  "match_time": "1672290701",
  "last_update": "1672291000",
  "outcome": "YES",
  "owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
  "trade_owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
  "maker_address": "0x1234...",
  "transaction_hash": "0xabc123...",
  "bucket_index": 88,
  "maker_orders": [
    {
      "order_id": "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b",
      "owner": "9180014b-33c8-9240-a14b-bdca11c0a465",
      "maker_address": "0x5678...",
      "matched_amount": "10",
      "price": "0.57",
      "fee_rate_bps": "0",
      "asset_id": "52114319501245915516055106046884209969926127482827954674443846427813813222426",
      "outcome": "YES",
      "side": "SELL"
    }
  ],
  "trader_side": "TAKER",
  "timestamp": "1672291000"
})";

std::string temp_dir() {
    char templ[] = "/tmp/crowdintel-userws-XXXXXX";
    const char* dir = ::mkdtemp(templ);
    return dir ? std::string(dir) : std::string();
}

std::string replace_once(std::string text, const std::string& from,
                         const std::string& to) {
    const size_t at = text.find(from);
    if (at == std::string::npos) return text;
    return text.replace(at, from.size(), to);
}

// ── parsing ──────────────────────────────────────────────────────────────────

void test_parse_order_event() {
    std::printf("parse_order_event\n");
    UserEvent event;
    CHECK(user_ws::parse_message(kOrderPlacement, std::strlen(kOrderPlacement),
                                 &event, 1) == 1,
          "placement event parses");
    CHECK(event.kind == UserEventKind::kOrder &&
              std::strcmp(event.venue_order_id, kVenueOrder) == 0,
          "order id");
    CHECK(event.order_type == UserOrderType::kPlacement &&
              event.order_status == UserOrderStatus::kLive,
          "placement/live");
    CHECK(event.side == UserSide::kSell, "side");
    CHECK(event.original_fixed6 == 10000000 && event.size_matched_fixed6 == 0,
          "sizes");
    CHECK(event.price_fixed6 == 570000, "price");
    CHECK(event.timestamp_ms == 1672290687, "timestamp");
    CHECK(std::strcmp(event.market, kCondition) == 0 &&
              std::strcmp(event.asset_id, kAsset) == 0,
          "market and asset");

    CHECK(user_ws::parse_message(kOrderCancellation,
                                 std::strlen(kOrderCancellation), &event, 1) == 1,
          "cancellation parses");
    CHECK(event.order_type == UserOrderType::kCancellation &&
              event.order_status == UserOrderStatus::kCanceled &&
              event.size_matched_fixed6 == 5000000,
          "cancellation carries the partial fill");

    // UPDATE with a partial fill (spec shape, value changed).
    std::string updated = kOrderPlacement;
    updated = replace_once(updated, "\"type\": \"PLACEMENT\"", "\"type\": \"UPDATE\"");
    updated = replace_once(updated, "\"size_matched\": \"0\"",
                           "\"size_matched\": \"4\"");
    CHECK(user_ws::parse_message(updated.c_str(), updated.size(), &event, 1) == 1 &&
              event.order_type == UserOrderType::kUpdate &&
              event.size_matched_fixed6 == 4000000,
          "update with partial fill");
}

void test_parse_trade_event() {
    std::printf("parse_trade_event\n");
    UserEvent event;
    CHECK(user_ws::parse_message(kTradeMatched, std::strlen(kTradeMatched), &event,
                                 1) == 1,
          "matched trade parses");
    CHECK(event.kind == UserEventKind::kTrade &&
              std::strcmp(event.trade_id,
                          "28c4d2eb-bbea-40e7-a9f0-b2fdb56b2c2e") == 0,
          "trade id");
    CHECK(std::strcmp(event.venue_order_id,
                      "0x06bc63e346ed4ceddce9efd6b3af37c8f8f440c92fe7da6b2d0f9e4ccbc50c42") == 0,
          "taker order id");
    CHECK(event.trade_status == UserTradeStatus::kMatched &&
              event.trade_size_fixed6 == 10000000,
          "matched size");
    CHECK(event.has_trader_side && event.trader_side == UserTraderSide::kTaker,
          "trader side");
    CHECK(event.maker_count == 1 &&
              std::strcmp(event.makers[0].order_id, kVenueOrder) == 0 &&
              event.makers[0].matched_fixed6 == 10000000,
          "maker fill parsed");

    CHECK(user_ws::parse_message(kTradeConfirmed, std::strlen(kTradeConfirmed),
                                 &event, 1) == 1,
          "confirmed trade parses");
    CHECK(event.trade_status == UserTradeStatus::kConfirmed &&
              std::strcmp(event.transaction_hash, "0xabc123...") == 0,
          "confirmation carries the transaction hash");
}

void test_parse_arrays_and_rejections() {
    std::printf("parse_arrays_and_rejections\n");
    const std::string batched =
        std::string("[") + kOrderPlacement + "," + kOrderCancellation + "]";
    UserEvent events[4];
    CHECK(user_ws::parse_message(batched.c_str(), batched.size(), events, 4) == 2 &&
              events[0].order_type == UserOrderType::kPlacement &&
              events[1].order_type == UserOrderType::kCancellation,
          "array payload with two events");
    // More events than the caller can hold must not truncate silently.
    CHECK(user_ws::parse_message(batched.c_str(), batched.size(), events, 1) == 0,
          "array overflow rejected");

    UserEvent event;
    const char* missing_field = R"({"event_type":"order","id":"0x1"})";
    CHECK(user_ws::parse_message(missing_field, std::strlen(missing_field), &event,
                                 1) == 0,
          "missing required fields rejected");
    const std::string duplicated = replace_once(
        kOrderPlacement,
        "\"price\": \"0.57\",",
        "\"price\": \"0.57\", \"price\": \"0.58\",");
    CHECK(user_ws::parse_message(duplicated.c_str(), duplicated.size(), &event,
                                 1) == 0,
          "duplicate field rejected");
    const char* unknown_event = R"({"event_type":"heartbeat"})";
    CHECK(user_ws::parse_message(unknown_event, std::strlen(unknown_event), &event,
                                 1) == 0,
          "non-event payload rejected");
    const std::string broken = replace_once(kOrderPlacement, "}", "");
    CHECK(user_ws::parse_message(broken.c_str(), broken.size(), &event, 1) == 0,
          "malformed JSON rejected");

    // Unknown enum values survive as kUnknown so the applier can treat them as
    // a divergence instead of guessing.
    const std::string unknown_status = replace_once(
        kOrderPlacement, "\"status\": \"LIVE\"", "\"status\": \"FROZEN\"");
    CHECK(user_ws::parse_message(unknown_status.c_str(), unknown_status.size(),
                                 &event, 1) == 1 &&
              event.order_status == UserOrderStatus::kUnknown,
          "unknown order status preserved");
    const std::string missing_status = replace_once(
        kOrderPlacement, "\"status\": \"LIVE\",", "");
    CHECK(user_ws::parse_message(missing_status.c_str(), missing_status.size(),
                                 &event, 1) == 1 &&
              event.order_status == UserOrderStatus::kUnknown,
          "absent status preserved as unknown");
    std::string trade_unknown = kTradeMatched;
    trade_unknown = replace_once(trade_unknown, "\"status\": \"MATCHED\"",
                                 "\"status\": \"PENDING_SETTLEMENT\"");
    CHECK(user_ws::parse_message(trade_unknown.c_str(), trade_unknown.size(),
                                 &event, 1) == 1 &&
              event.trade_status == UserTradeStatus::kUnknown,
          "unknown trade status preserved");
    std::string side_unknown =
        replace_once(kOrderPlacement, "\"side\": \"SELL\"", "\"side\": \"SHORT\"");
    CHECK(user_ws::parse_message(side_unknown.c_str(), side_unknown.size(), &event,
                                 1) == 1 &&
              event.side == UserSide::kUnknown,
          "unknown side preserved");

    // Nine maker orders exceed the bounded table: refuse instead of dropping
    // one of our own fills.
    std::string many = kTradeMatched;
    std::string makers;
    for (int i = 0; i < 9; ++i) {
        if (i) makers += ",";
        makers += "{\"order_id\":\"0x" + std::to_string(i) +
                  "\",\"owner\":\"9180014b-33c8-9240-a14b-bdca11c0a465\","
                  "\"matched_amount\":\"1\",\"price\":\"0.57\",\"asset_id\":\"" +
                  std::string(kAsset) + "\"}";
    }
    const size_t begin = many.find("\"maker_orders\": [");
    const size_t end = many.find(']', begin);
    many = many.substr(0, begin) + "\"maker_orders\": [" + makers + "]" +
           many.substr(end + 1);
    CHECK(user_ws::parse_message(many.c_str(), many.size(), &event, 1) == 0,
          "maker overflow rejected");
}

void test_subscription_payload() {
    std::printf("subscription_payload\n");
    char payload[768];
    size_t length = 0;
    CHECK(user_ws::build_subscription("key-uuid", "secret-value", "pass-123",
                                      kCondition, payload, sizeof(payload),
                                      length),
          "subscription builds");
    const std::string expected =
        std::string("{\"auth\":{\"apiKey\":\"key-uuid\",\"secret\":\"secret-value\","
                    "\"passphrase\":\"pass-123\"},\"type\":\"user\",\"markets\":[\"") +
        kCondition + "\"]}";
    CHECK(std::string(payload, length) == expected, "subscription matches spec");
    CHECK(user_ws::json_safe_value("abc-123_."), "safe value accepted");
    CHECK(!user_ws::json_safe_value("bad\"quote"), "embedded quote rejected");
    CHECK(!user_ws::json_safe_value("bad\\slash"), "embedded backslash rejected");
    CHECK(!user_ws::json_safe_value("line\nbreak"), "control character rejected");
    CHECK(!user_ws::build_subscription("key", "sec\"ret", "pass", kCondition,
                                       payload, sizeof(payload), length),
          "injectable secret refuses to build");
    CHECK(!user_ws::build_subscription("key", "secret", "pass", "",
                                       payload, sizeof(payload), length),
          "missing condition id refuses to build");

    unsigned char config_ignored[8];
    (void)config_ignored;
    CHECK(user_ws::build_subscription_update("subscribe", kCondition, payload,
                                             sizeof(payload), length) &&
              std::string(payload, length) ==
                  std::string("{\"operation\":\"subscribe\",\"markets\":[\"") +
                      kCondition + "\"]}",
          "subscribe update");
    CHECK(user_ws::build_subscription_update("replace", kCondition, payload,
                                             sizeof(payload), length) == false,
          "unknown operation rejected");
}

void test_user_url_derivation() {
    std::printf("user_url_derivation\n");
    wstransport::Url url;
    CHECK(UserWsClient::derive_user_url(
              "", "wss://ws-subscriptions-clob.polymarket.com/ws/market", url) &&
              std::strcmp(url.path, "/ws/user") == 0 &&
              std::strcmp(url.host, "ws-subscriptions-clob.polymarket.com") == 0 &&
              url.port == 443 && url.tls,
          "market host derives /ws/user");
    CHECK(UserWsClient::derive_user_url(
              "", "wss://ws-subscriptions-clob.polymarket.com/ws/user", url),
          "already-user path accepted");
    CHECK(!UserWsClient::derive_user_url(
              "", "wss://ws-subscriptions-clob.polymarket.com/somewhere", url),
          "unknown path refused");
    CHECK(UserWsClient::derive_user_url(
              "wss://custom.example:9443/ws/user", "wss://ignored/ws/market", url) &&
              std::strcmp(url.host, "custom.example") == 0 && url.port == 9443,
          "explicit host wins");
    CHECK(!UserWsClient::derive_user_url("", "", url), "no host refused");
    CHECK(!UserWsClient::derive_user_url(
              "", "https://ws-subscriptions-clob.polymarket.com/ws/market", url),
          "wrong scheme refused");
}

void test_market_filtering() {
    std::printf("market_filtering\n");
    MarketConfig cfg;
    UserEvent event;
    CHECK(user_ws::parse_message(kOrderPlacement, std::strlen(kOrderPlacement),
                                 &event, 1) == 1,
          "fixture");
    CHECK(!UserWsClient::is_our_market(cfg, event),
          "event dropped while the condition id is unknown");
    std::snprintf(cfg.runtime.condition_id, sizeof(cfg.runtime.condition_id), "%s",
                  kCondition);
    CHECK(UserWsClient::is_our_market(cfg, event), "matching market accepted");
    std::snprintf(cfg.runtime.condition_id, sizeof(cfg.runtime.condition_id),
                  "0x1111111111111111111111111111111111111111111111111111111111111111");
    CHECK(!UserWsClient::is_our_market(cfg, event), "foreign market rejected");
}

// ── transport codec ──────────────────────────────────────────────────────────

void test_frame_codec() {
    std::printf("frame_codec\n");
    char frame[256];
    size_t frame_len = 0;

    // A small client frame is masked and decodable.
    CHECK(wstransport::encode_frame(0x1, "PING", 4, frame, sizeof(frame),
                                    frame_len) && frame_len == 10,
          "PING frame encoded");
    CHECK((static_cast<uint8_t>(frame[0]) & 0x80U) != 0 &&
              (static_cast<uint8_t>(frame[0]) & 0x0FU) == 0x1,
          "FIN + text opcode");
    CHECK((static_cast<uint8_t>(frame[1]) & 0x80U) != 0 &&
              (static_cast<uint8_t>(frame[1]) & 0x7FU) == 4,
          "masked with length 4");
    char unmasked[4];
    for (size_t i = 0; i < 4; ++i)
        unmasked[i] = static_cast<char>(static_cast<uint8_t>(frame[2 + 4 + i]) ^
                                        static_cast<uint8_t>(frame[2 + (i & 3)]));
    CHECK(std::memcmp(unmasked, "PING", 4) == 0, "mask round-trips");

    // 16-bit and 64-bit lengths.
    std::vector<char> big(65536, 'x');
    CHECK(wstransport::encode_frame(0x1, big.data(), big.size(), frame,
                                    sizeof(frame), frame_len) == false,
          "frame larger than the buffer refused");
    std::vector<char> medium(300, 'y');
    std::vector<char> out(4096);
    CHECK(wstransport::encode_frame(0x1, medium.data(), medium.size(), out.data(),
                                    out.size(), frame_len) && frame_len == 4 + 4 + 300,
          "16-bit length frame");
    CHECK(static_cast<uint8_t>(out[1]) == (0x80U | 126U), "126 marker");

    // Server frames are unmasked; decode the header/length forms.
    const char small[] = {static_cast<char>(0x81), static_cast<char>(0x05),
                          'h', 'e', 'l', 'l', 'o'};
    wstransport::Frame decoded;
    CHECK(wstransport::decode_frame(small, sizeof(small), decoded) ==
              wstransport::FrameStatus::kComplete &&
              decoded.opcode == 0x1 && decoded.final &&
              decoded.payload_len == 5 && decoded.consumed == 7,
          "small server frame");
    CHECK(wstransport::decode_frame(small, 3, decoded) ==
              wstransport::FrameStatus::kNeedMore,
          "incomplete frame reports need-more");
    const char masked[] = {static_cast<char>(0x81), static_cast<char>(0x85),
                           1, 2, 3, 4, 'x', 'y', 'z', 'w', 'v'};
    CHECK(wstransport::decode_frame(masked, sizeof(masked), decoded) ==
              wstransport::FrameStatus::kError,
          "masked server frame rejected");
    const char reserved[] = {static_cast<char>(0x91), static_cast<char>(0x02),
                             'h', 'i'};
    CHECK(wstransport::decode_frame(reserved, sizeof(reserved), decoded) ==
              wstransport::FrameStatus::kError,
          "reserved bits rejected");
    const char oversized_control[] = {static_cast<char>(0x89),
                                      static_cast<char>(0x7E),
                                      static_cast<char>(0x00),
                                      static_cast<char>(0x80)};
    CHECK(wstransport::decode_frame(oversized_control, sizeof(oversized_control),
                                    decoded) == wstransport::FrameStatus::kError,
          "control frame over 125 bytes rejected");
    const char noncanonical[] = {static_cast<char>(0x81),
                                 static_cast<char>(0x7E),
                                 static_cast<char>(0x00),
                                 static_cast<char>(0x20)};
    CHECK(wstransport::decode_frame(noncanonical, sizeof(noncanonical), decoded) ==
              wstransport::FrameStatus::kError,
          "non-canonical 16-bit length rejected");
    const char pong[] = {static_cast<char>(0x8A), static_cast<char>(0x04),
                         'P', 'O', 'N', 'G'};
    CHECK(wstransport::decode_frame(pong, sizeof(pong), decoded) ==
              wstransport::FrameStatus::kComplete &&
              decoded.opcode == 0xA && decoded.payload_len == 4,
          "pong frame");
    // 64-bit length form with a small payload is non-canonical.
    const char noncanonical64[] = {static_cast<char>(0x81), static_cast<char>(0x7F),
                                   0, 0, 0, 0, 0, 0, 0, 5, 'a', 'b', 'c', 'd', 'e'};
    CHECK(wstransport::decode_frame(noncanonical64, sizeof(noncanonical64),
                                    decoded) == wstransport::FrameStatus::kError,
          "non-canonical 64-bit length rejected");
    // Fragmentation: continuation frames assemble at the caller level, so the
    // codec must report them faithfully.
    const char cont_head[] = {static_cast<char>(0x01), static_cast<char>(0x02),
                              'h', 'i'};
    CHECK(wstransport::decode_frame(cont_head, sizeof(cont_head), decoded) ==
              wstransport::FrameStatus::kComplete && !decoded.final &&
              decoded.opcode == 0x1,
          "first fragment decoded as non-final");
    const char cont_tail[] = {static_cast<char>(0x80), static_cast<char>(0x03),
                              '!', '!', '!'};
    CHECK(wstransport::decode_frame(cont_tail, sizeof(cont_tail), decoded) ==
              wstransport::FrameStatus::kComplete &&
              decoded.opcode == 0x0 && decoded.final && decoded.payload_len == 3,
          "continuation with FIN decoded");
}

// ── applier ──────────────────────────────────────────────────────────────────

struct ApplierFixture {
    MarketConfig cfg;
    cledger::OrderLedger ledger;
    std::unique_ptr<user_apply::UserEventApplier> applier;
    char error[192]{};
    std::string dir;
    std::string path;

    ~ApplierFixture() {
        if (!path.empty()) ::unlink(path.c_str());
        if (!dir.empty()) ::rmdir(dir.c_str());
    }

    bool init(const char* api_key = "9180014b-33c8-9240-a14b-bdca11c0a465") {
        char templ[] = "/tmp/crowdintel-userws-XXXXXX";
        const char* created = ::mkdtemp(templ);
        if (!created) return false;
        dir = created;
        path = dir + "/orders.journal";
        std::snprintf(cfg.owner_api_key, sizeof(cfg.owner_api_key), "%s", api_key);
        std::snprintf(cfg.runtime.condition_id, sizeof(cfg.runtime.condition_id),
                      "%s", kCondition);
        cledger::OpenOptions options;
        options.fsync_records = false;
        if (!ledger.open(path.c_str(), options, error, sizeof(error)))
            return false;
        applier = std::make_unique<user_apply::UserEventApplier>(cfg);
        applier->attach_ledger(&ledger);
        return true;
    }

    // Journals an acknowledged order so channel events can attribute to it.
    bool seed_order(const char* client_id, const char* venue_id,
                    uint64_t shares_fixed6 = 10000000) {
        cledger::IntentRecord intent{};
        std::snprintf(intent.client_order_id, sizeof(intent.client_order_id),
                      "%s", client_id);
        intent.signal_id = 1;
        intent.market_hash = 0x1;
        intent.price_fixed6 = 570000;
        intent.shares_fixed6 = shares_fixed6;
        intent.notional_fixed6 = 5700000;
        intent.side = K_SIDE_SELL;
        intent.order_type = 1;
        if (!ledger.record_intent(intent, error, sizeof(error))) return false;
        return ledger.record_transition(client_id, cledger::LedgerEvent::kSubmitAck,
                                        cledger::Evidence::kVenueAck, venue_id, 0,
                                        error, sizeof(error))
                   .result == cledger::TransitionResult::kApplied;
    }

    bool parse(const char* json, UserEvent& event) {
        return user_ws::parse_message(json, std::strlen(json), &event, 1) == 1;
    }
};

void test_applier_order_events() {
    std::printf("applier_order_events\n");
    ApplierFixture fixture;
    CHECK(fixture.init(), "fixture");
    CHECK(fixture.seed_order("sig-1", kVenueOrder), "seed order");
    UserEvent event;

    {  // PLACEMENT/LIVE
        CHECK(fixture.parse(kOrderPlacement, event), "placement fixture");
        CHECK(fixture.applier->apply(event, fixture.error, sizeof(fixture.error)) ==
                  user_apply::ApplyResult::kApplied,
              "placement applies");
        CHECK(fixture.ledger.find("sig-1")->state == cledger::OrderState::kLive,
              "order is LIVE");
    }
    {  // CANCELLATION after a partial fill
        CHECK(fixture.parse(kOrderCancellation, event), "cancellation fixture");
        CHECK(fixture.applier->apply(event, fixture.error, sizeof(fixture.error)) ==
                  user_apply::ApplyResult::kApplied,
              "cancellation applies");
        const cledger::OrderSummary* summary = fixture.ledger.find("sig-1");
        CHECK(summary->state == cledger::OrderState::kCanceled &&
                  summary->filled_fixed6 == 5000000,
              "canceled with the venue's cumulative fill");
        CHECK(fixture.ledger.gate_open(), "terminal order opens the gate");
    }
    {  // Contradiction: an order cannot come back to life after cancellation.
        CHECK(fixture.parse(kOrderPlacement, event), "placement fixture");
        CHECK(fixture.applier->apply(event, fixture.error, sizeof(fixture.error)) ==
                  user_apply::ApplyResult::kIllegal,
              "live after cancel is illegal");
        CHECK(!fixture.applier->clean(), "illegal event is a divergence");
    }

    ApplierFixture second;
    CHECK(second.init(), "second fixture");
    CHECK(second.seed_order("sig-2", kVenueOrder, 10000000), "seed order 2");
    {  // Unknown venue order: never guessed, always surfaced.
        UserEvent unknown;
        const std::string foreign = replace_once(
            kOrderPlacement,
            "0xff354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b",
            "0xaa354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b");
        CHECK(second.parse(foreign.c_str(), unknown), "foreign fixture parses");
        CHECK(second.applier->apply(unknown, second.error, sizeof(second.error)) ==
                  user_apply::ApplyResult::kUnattributed,
              "unknown venue order is unattributed");
        CHECK(second.applier->stats().unattributed == 1,
              "unattributed counted");
    }
    {  // Cumulative fill regression must not rewrite history.
        UserEvent regressed;
        std::string text = replace_once(kOrderPlacement, "\"status\": \"LIVE\",",
                                        "\"status\": \"MATCHED\",");
        text = replace_once(text, "\"size_matched\": \"0\"",
                            "\"size_matched\": \"6\"");
        CHECK(second.parse(text.c_str(), regressed), "matched fixture");
        CHECK(second.applier->apply(regressed, second.error, sizeof(second.error)) ==
                  user_apply::ApplyResult::kApplied,
              "partial fill applies");
        text = replace_once(text, "\"size_matched\": \"6\"",
                            "\"size_matched\": \"2\"");
        CHECK(second.parse(text.c_str(), regressed), "regressed fixture");
        CHECK(second.applier->apply(regressed, second.error, sizeof(second.error)) ==
                  user_apply::ApplyResult::kDivergence,
              "fill regression is a divergence");
        CHECK(second.ledger.find("sig-2")->filled_fixed6 == 6000000,
              "journal keeps the higher cumulative fill");
    }
    {  // Full fill through an order update.
        UserEvent filled;
        std::string text = replace_once(kOrderPlacement, "\"status\": \"LIVE\",",
                                        "\"status\": \"MATCHED\",");
        text = replace_once(text, "\"size_matched\": \"0\"",
                            "\"size_matched\": \"10\"");
        CHECK(second.parse(text.c_str(), filled), "fill fixture");
        CHECK(second.applier->apply(filled, second.error, sizeof(second.error)) ==
                  user_apply::ApplyResult::kApplied &&
              second.ledger.find("sig-2")->state == cledger::OrderState::kFilled,
              "full fill reaches FILLED");
    }
}

void test_applier_trade_events() {
    std::printf("applier_trade_events\n");
    ApplierFixture fixture;
    CHECK(fixture.init(), "fixture");
    const char* taker =
        "0x06bc63e346ed4ceddce9efd6b3af37c8f8f440c92fe7da6b2d0f9e4ccbc50c42";
    CHECK(fixture.seed_order("sig-taker", taker, 20000000), "seed taker order");
    UserEvent matched;
    CHECK(fixture.parse(kTradeMatched, matched), "trade fixture");
    matched.trade_size_fixed6 = 5000000;  // half the order
    matched.maker_count = 0;              // taker-only this time

    CHECK(fixture.applier->apply(matched, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kApplied,
          "matched trade increments the fill");
    CHECK(fixture.ledger.find("sig-taker")->filled_fixed6 == 5000000 &&
              fixture.ledger.find("sig-taker")->state ==
                  cledger::OrderState::kPartiallyFilled,
          "partial fill recorded");

    // The same trade id delivered again (any later status) is not a new fill.
    CHECK(fixture.applier->apply(matched, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kNoChange,
          "duplicate MATCHED ignored");
    UserEvent confirmed;
    CHECK(fixture.parse(kTradeConfirmed, confirmed), "confirmed fixture");
    confirmed.maker_count = 0;
    CHECK(fixture.applier->apply(confirmed, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kNoChange,
          "MINED/CONFIRMED of the same trade ignored");
    CHECK(fixture.ledger.find("sig-taker")->filled_fixed6 == 5000000,
          "no double count");
    CHECK(fixture.applier->stats().trade_dedupe_hits >= 2, "dedupe counted");

    // A settled trade we never saw matched: the journal cannot prove the total.
    UserEvent unseen;
    CHECK(fixture.parse(kTradeConfirmed, unseen), "unseen trade fixture");
    std::snprintf(unseen.trade_id, sizeof(unseen.trade_id),
                  "31c4d2eb-bbea-40e7-a9f0-b2fdb56b2c2e");
    unseen.maker_count = 0;
    CHECK(fixture.applier->apply(unseen, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kDivergence,
          "settled-but-unseen trade is a divergence");

    // FAILED invalidates the total: the order returns to UNKNOWN.
    UserEvent failed;
    CHECK(fixture.parse(kTradeMatched, failed), "failed trade fixture");
    std::snprintf(failed.trade_id, sizeof(failed.trade_id),
                  "41c4d2eb-bbea-40e7-a9f0-b2fdb56b2c2e");
    failed.maker_count = 0;
    failed.trade_status = UserTradeStatus::kFailed;
    CHECK(fixture.applier->apply(failed, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kDivergence &&
              fixture.ledger.find("sig-taker")->state == cledger::OrderState::kUnknown,
          "failed trade returns the order to UNKNOWN");

    // RETRYING is informative, not a fill and not a failure.
    UserEvent retrying = failed;
    std::snprintf(retrying.trade_id, sizeof(retrying.trade_id),
                  "51c4d2eb-bbea-40e7-a9f0-b2fdb56b2c2e");
    retrying.trade_status = UserTradeStatus::kRetrying;
    CHECK(fixture.applier->apply(retrying, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kNoChange,
          "retrying trade changes nothing");
    CHECK(fixture.applier->stats().retrying_seen == 1, "retrying counted");

    // Trade for an order we never journaled.
    UserEvent foreign = matched;
    std::snprintf(foreign.venue_order_id, sizeof(foreign.venue_order_id),
                  "0xbbbc63e346ed4ceddce9efd6b3af37c8f8f440c92fe7da6b2d0f9e4ccbc50c42");
    std::snprintf(foreign.trade_id, sizeof(foreign.trade_id),
                  "61c4d2eb-bbea-40e7-a9f0-b2fdb56b2c2e");
    CHECK(fixture.applier->apply(foreign, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kUnattributed,
          "unknown taker order is unattributed");

    CHECK(!fixture.applier->clean(), "divergences keep the applier unclean");
    fixture.applier->reset_after_reconciliation();
    CHECK(fixture.applier->clean() && fixture.applier->stats().applied == 0,
          "reconciliation clears the counters");
}

void test_applier_maker_fills() {
    std::printf("applier_maker_fills\n");
    ApplierFixture fixture;
    CHECK(fixture.init(), "fixture");
    CHECK(fixture.seed_order("sig-maker", kVenueOrder, 10000000), "seed maker");
    UserEvent matched;
    CHECK(fixture.parse(kTradeMatched, matched), "trade fixture");
    // Our key owns the maker order but not the taker.
    const std::string maker_owner = "9180014b-33c8-9240-a14b-bdca11c0a465";
    std::snprintf(matched.owner, sizeof(matched.owner), "%s",
                  "7c9f0e1a-0000-0000-0000-000000000000");
    std::snprintf(matched.makers[0].owner, sizeof(matched.makers[0].owner), "%s",
                  maker_owner.c_str());

    CHECK(fixture.applier->apply(matched, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kApplied,
          "maker fill applies to the resting order");
    CHECK(fixture.applier->stats().maker_fills == 1, "maker fill counted");
    CHECK(fixture.ledger.find("sig-maker")->filled_fixed6 == 10000000 &&
              fixture.ledger.find("sig-maker")->state == cledger::OrderState::kFilled,
          "resting order fully filled");

    // Replayed maker fill for the same trade is deduplicated.
    CHECK(fixture.applier->apply(matched, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kNoChange,
          "replayed maker fill ignored");

    // A trade where neither the taker nor any maker is ours is foreign.
    UserEvent foreign = matched;
    std::snprintf(foreign.trade_id, sizeof(foreign.trade_id),
                  "71c4d2eb-bbea-40e7-a9f0-b2fdb56b2c2e");
    std::snprintf(foreign.makers[0].owner, sizeof(foreign.makers[0].owner), "%s",
                  "00000000-0000-0000-0000-000000000000");
    CHECK(fixture.applier->apply(foreign, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kUnattributed,
          "foreign trade is unattributed");
}

void test_dedupe_overflow_fails_closed() {
    std::printf("dedupe_overflow\n");
    ApplierFixture fixture;
    CHECK(fixture.init(), "fixture");
    CHECK(fixture.seed_order("sig-big", kVenueOrder, 1000000000), "seed order");
    UserEvent matched;
    CHECK(fixture.parse(kTradeMatched, matched), "trade fixture");
    std::snprintf(matched.venue_order_id, sizeof(matched.venue_order_id), "%s",
                  kVenueOrder);
    matched.maker_count = 0;
    matched.trade_size_fixed6 = 1000;  // 0.001 shares per trade

    bool overflow_seen = false;
    for (uint32_t i = 0; i < user_apply::UserEventApplier::kDedupeSlots + 8; ++i) {
        char trade_id[64];
        std::snprintf(trade_id, sizeof(trade_id), "trade-%08u", i);
        std::snprintf(matched.trade_id, sizeof(matched.trade_id), "%s", trade_id);
        matched.timestamp_ms += 1;
        const user_apply::ApplyResult result =
            fixture.applier->apply(matched, fixture.error, sizeof(fixture.error));
        if (result == user_apply::ApplyResult::kDivergence) {
            overflow_seen = true;
            break;
        }
    }
    CHECK(overflow_seen, "overflow blocks instead of double counting");
    CHECK(!fixture.applier->clean(), "overflow is not clean");
    const uint64_t filled_before = fixture.ledger.find("sig-big")->filled_fixed6;
    // After overflow no further increment is applied.
    std::snprintf(matched.trade_id, sizeof(matched.trade_id), "trade-after");
    CHECK(fixture.applier->apply(matched, fixture.error, sizeof(fixture.error)) ==
              user_apply::ApplyResult::kDivergence &&
              fixture.ledger.find("sig-big")->filled_fixed6 == filled_before,
          "increments stop after overflow");
}

void test_applier_requires_ledger_and_key() {
    std::printf("applier_preconditions\n");
    MarketConfig cfg;
    user_apply::UserEventApplier applier(cfg);
    CHECK(!applier.configured(), "unconfigured applier reports so");
    UserEvent event;
    CHECK(user_ws::parse_message(kOrderPlacement, std::strlen(kOrderPlacement),
                                 &event, 1) == 1,
          "fixture");
    CHECK(applier.apply(event, nullptr, 0) == user_apply::ApplyResult::kDivergence,
          "no ledger means no action");
}

// ── end-to-end: engine submits, channel resolves, trading resumes ─────────────

struct ChannelAckClient {
    int calls = 0;
    char next_venue_id[72]{};

    SubmitResult submit(const WireBody&) {
        ++calls;
        SubmitResult result{};
        result.ok = true;
        result.final = true;
        result.http_code = 200;
        std::snprintf(result.status, sizeof(result.status), "live");
        std::snprintf(result.order_id, sizeof(result.order_id), "%s",
                      next_venue_id);
        return result;
    }
};

void test_engine_channel_roundtrip() {
    std::printf("engine_channel_roundtrip\n");
    const std::string dir = temp_dir();
    CHECK(!dir.empty(), "temp dir");
    const std::string path = dir + "/roundtrip.journal";
    char error[192];

    // Engine fixture (mock metadata so no live resolver is involved).
    MarketConfig cfg;
    EIP712Signer signer;
    const char* key_text =
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
    uint8_t key[32];
    CHECK(parse_hex_bytes(key_text, 64, key, sizeof(key)) &&
              signer.init(key, false),
          "signer");
    secure_zero(key, sizeof(key));
    cfg.mode = MarketConfig::Mode::kReplay;
    const char* token =
        "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    std::snprintf(cfg.token_id_dec, sizeof(cfg.token_id_dec), "%s", token);
    CHECK(parse_uint256_dec(token, std::strlen(token), cfg.token_id_be),
          "token");
    CHECK(cfg.finalize_identity(signer.signer_address()) == nullptr, "identity");
    std::snprintf(cfg.owner_api_key, sizeof(cfg.owner_api_key), "%s",
                  "9180014b-33c8-9240-a14b-bdca11c0a465");
    std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "channel-market");
    cfg.market_hash =
        alpha_hash_bytes(cfg.market_slug, std::strlen(cfg.market_slug));
    std::snprintf(cfg.runtime.condition_id, sizeof(cfg.runtime.condition_id),
                  "%s", kCondition);
    cfg.max_order_usd = 10.0;
    cfg.max_exposure_usd = 100.0;
    cfg.max_daily_loss_usd = 100.0;

    OrderBookL2 book;
    book.set_tick_size(10000);
    const Level2Entry bids[] = {{470000, 50000000}};
    const Level2Entry asks[] = {{530000, 50000000}};
    book.set_book(bids, 1, asks, 1);
    SPSC_RingBuffer<AlphaSignal> signals;
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    CHECK(pool.rebuild(470000, 530000, 20000000, 10000), "ladder");

    cledger::OrderLedger ledger;
    cledger::OpenOptions options;
    options.fsync_records = true;
    CHECK(ledger.open(path.c_str(), options, error, sizeof(error)), "ledger");

    ChannelAckClient client;
    std::snprintf(client.next_venue_id, sizeof(client.next_venue_id), "%s",
                  kVenueOrder);
    ExecutionEngine<ChannelAckClient> engine(cfg, book, signals, signer, pool,
                                             client);
    engine.attach_ledger(&ledger);
    user_apply::UserEventApplier applier(cfg);
    applier.attach_ledger(&ledger);
    CHECK(applier.configured(), "applier configured");

    auto push_signal = [&](uint64_t id) {
        AlphaSignal signal{};
        signal.direction_hint = K_SIDE_BUY;
        signal.p_win = 0.75;
        signal.confidence = 0.95;
        signal.q_value = 0.01;
        signal.timestamp_ns = AlphaParser::realtime_ns();
        signal.market_hash = cfg.market_hash;
        signal.signal_id = id;
        return signals.try_push(signal);
    };

    // 1. The engine submits; the venue acknowledges; the journal holds it.
    CHECK(push_signal(101) && engine.run_tick() == TickResult::SUBMITTED,
          "order submitted and acknowledged");
    CHECK(!ledger.gate_open(), "acknowledged order blocks further trading");
    // 2. A second signal cannot trade while the first order is unproven.
    CHECK(push_signal(102) && engine.run_tick() == TickResult::RECONCILE_REQUIRED,
          "engine refuses to trade while the order is unresolved");
    CHECK(client.calls == 1, "no second order reached the venue");

    // 3. The private channel reports the order resting: still not terminal.
    UserEvent live;
    CHECK(user_ws::parse_message(kOrderPlacement, std::strlen(kOrderPlacement),
                                 &live, 1) == 1,
          "channel fixture");
    CHECK(applier.apply(live, error, sizeof(error)) ==
              user_apply::ApplyResult::kApplied,
          "channel event applied");
    CHECK(ledger.find_by_venue(kVenueOrder)->state == cledger::OrderState::kLive,
          "order is LIVE");
    CHECK(!ledger.gate_open(), "a resting order still blocks new risk");

    // 4. The venue cancels it: the journal reaches a terminal state and the
    //    engine can trade again.
    UserEvent canceled;
    CHECK(user_ws::parse_message(kOrderCancellation,
                                 std::strlen(kOrderCancellation), &canceled,
                                 1) == 1,
          "cancellation fixture");
    CHECK(applier.apply(canceled, error, sizeof(error)) ==
              user_apply::ApplyResult::kApplied,
          "cancellation applied");
    CHECK(ledger.find_by_venue(kVenueOrder)->state ==
              cledger::OrderState::kCanceled,
          "order canceled");
    CHECK(ledger.gate_open(), "gate reopens once the order is terminal");
    std::snprintf(client.next_venue_id, sizeof(client.next_venue_id),
                  "0xaa354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b");
    CHECK(push_signal(103) && engine.run_tick() == TickResult::SUBMITTED,
          "trading resumes after the channel resolves the order");
    CHECK(client.calls == 2, "second order submitted");

    // 5. Divergences block trading for good until reconciliation.
    UserEvent stranger;
    CHECK(user_ws::parse_message(kOrderPlacement, std::strlen(kOrderPlacement),
                                 &stranger, 1) == 1,
          "channel fixture");
    std::snprintf(stranger.venue_order_id, sizeof(stranger.venue_order_id),
                  "0xbb354cd7ca7539dfa9c28d90943ab5779a4eac34b9b37a757d7b32bdfb11790b");
    CHECK(applier.apply(stranger, error, sizeof(error)) ==
              user_apply::ApplyResult::kUnattributed,
          "unknown order is a divergence");
    CHECK(!applier.clean(), "divergence keeps the bot paused");

    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

}  // namespace

int main() {
    std::printf("== CROWDINTEL user channel tests ==\n");
    test_parse_order_event();
    test_parse_trade_event();
    test_parse_arrays_and_rejections();
    test_subscription_payload();
    test_user_url_derivation();
    test_market_filtering();
    test_frame_codec();
    test_applier_order_events();
    test_applier_trade_events();
    test_applier_maker_fills();
    test_dedupe_overflow_fails_closed();
    test_applier_requires_ledger_and_key();
    test_engine_channel_roundtrip();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
