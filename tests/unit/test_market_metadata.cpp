// Phase 1 unit tests: dynamic market metadata.
//
// Fixtures mirror the documented response shapes (docs.polymarket.com) and are
// parsed by the same pure functions the live resolver uses. No network access
// is required; the resolver's HTTP layer is exercised separately by
// crowdintel-metadata against the public venue.

#include <cstdio>
#include <cstring>
#include <string>

#include "../../core/src/market_config.hpp"
#include "../../core/src/market_metadata.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__);      \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

constexpr const char* kConditionId =
    "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75";
constexpr const char* kYesToken =
    "107505882767731489358349912513945399560393482969656700824895970500493757150417";
constexpr const char* kNoToken =
    "7305630249804085635496399869905769372294302716159034447326228509068694952392";

const char* kGamma = R"({
  "id":"703257",
  "slug":"aliens-before-2027",
  "conditionId":"0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
  "outcomes":"[\"Yes\", \"No\"]",
  "clobTokenIds":"[\"107505882767731489358349912513945399560393482969656700824895970500493757150417\", \"7305630249804085635496399869905769372294302716159034447326228509068694952392\"]",
  "active":true,"closed":false,"archived":false,"restricted":false,
  "acceptingOrders":true,"enableOrderBook":true,"negRisk":false,
  "orderPriceMinTickSize":0.01,"orderMinSize":5,
  "feeSchedule":{"exponent":1,"rate":0.04,"takerOnly":true,"rebateRate":0.25}
})";

const char* kClobMarkets = R"({
  "gst":null,
  "r":{},
  "t":[{"t":"107505882767731489358349912513945399560393482969656700824895970500493757150417","o":"Yes"},
       {"t":"7305630249804085635496399869905769372294302716159034447326228509068694952392","o":"No"}],
  "mos":5,"mts":0.01,"mbf":0,"tbf":0,"rfqe":false,"ibce":true,
  "fd":{"r":0.04,"e":1,"to":true},"oas":0
})";

const char* kBook = R"({
  "market":"0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75",
  "asset_id":"107505882767731489358349912513945399560393482969656700824895970500493757150417",
  "timestamp":"1759420800","hash":"a1b2c3",
  "bids":[{"price":"0.45","size":"100"}],
  "asks":[{"price":"0.46","size":"150"}],
  "min_order_size":"5","tick_size":"0.01","neg_risk":false,"last_trade_price":"0.45"
})";

struct Fixture {
    crowdintel::GammaMarketView gamma;
    crowdintel::ClobMarketDetailsView clob;
    crowdintel::BookView book;
    uint64_t tick_probe = 10000;
    double fee_probe = 0.0;  // bps, agrees with tbf=0 in the fixture
    int64_t server_time = 1759420800;
    int64_t local_time = 1759420799;
};

bool load_fixture(Fixture& f, const char* gamma_json = kGamma,
                  const char* clob_json = kClobMarkets,
                  const char* book_json = kBook) {
    const bool ok =
        crowdintel::parse_gamma_market(gamma_json, std::strlen(gamma_json), f.gamma) &&
        crowdintel::parse_clob_market_details(clob_json, std::strlen(clob_json), f.clob) &&
        crowdintel::parse_book_summary(book_json, std::strlen(book_json), f.book);
    return ok;
}

crowdintel::MarketResolution resolve_fixture(const Fixture& f,
                                             const char* outcome = "Yes") {
    crowdintel::MarketResolutionInputs in;
    in.gamma = &f.gamma;
    in.clob = &f.clob;
    in.book = &f.book;
    in.tick_size_probe = &f.tick_probe;
    in.fee_rate_probe = &f.fee_probe;
    in.server_time_seconds = &f.server_time;
    in.local_time_seconds = f.local_time;
    in.max_clock_offset_ms = 2000;
    in.requested_outcome = outcome;
    return crowdintel::resolve_market(in);
}

std::string replace_once(std::string text, const std::string& from,
                         const std::string& to) {
    const size_t at = text.find(from);
    if (at == std::string::npos) return text;
    return text.replace(at, from.size(), to);
}

// ── json_field primitives ────────────────────────────────────────────────────

void test_json_field_basics() {
    std::printf("json_field_basics\n");
    const char* doc = R"({"a":"x","b":0.01,"c":true,"d":[1,2],"e":"[\"p\",\"q\"]","n":-3})";
    const size_t len = std::strlen(doc);

    char text[8]{};
    CHECK(json_field::string(doc, len, "a", text, sizeof(text)) &&
          std::strcmp(text, "x") == 0, "string field");
    bool flag = false;
    CHECK(json_field::boolean(doc, len, "c", flag) && flag, "boolean field");
    uint64_t fixed = 0;
    CHECK(json_field::fixed6(doc, len, "b", fixed) && fixed == 10000,
          "fixed6 from number");
    char embedded[2][8]{};
    CHECK(json_field::embedded_string_array<8>(doc, len, "e", &embedded[0][0], 2) == 2 &&
          std::strcmp(embedded[0], "p") == 0 && std::strcmp(embedded[1], "q") == 0,
          "embedded string array");
    double number = 0.0;
    CHECK(json_field::number(doc, len, "n", number) && number == -3.0,
          "negative number");

    // Duplicate keys are ambiguous and must not resolve.
    const char* dup = R"({"b":0.01,"b":0.02})";
    uint64_t dup_fixed = 0;
    CHECK(!json_field::fixed6(dup, std::strlen(dup), "b", dup_fixed),
          "duplicate key rejected");
    // A string is not a number.
    const char* stringly = R"({"b":"0.01"})";
    double numeric = 0.0;
    CHECK(!json_field::number(stringly, std::strlen(stringly), "b", numeric),
          "string rejected by number()");
    CHECK(json_field::fixed6(stringly, std::strlen(stringly), "b", dup_fixed) &&
          dup_fixed == 10000, "string accepted by fixed6()");
    // Non-exact fractions are rejected by the fixed-point helper.
    const char* inexact = R"({"b":"0.0000001"})";
    CHECK(!json_field::fixed6(inexact, std::strlen(inexact), "b", dup_fixed),
          "sub-1e-6 rejected");
    // Escapes other than the documented set fail closed.
    const char* unicode = R"({"a":"\u0041"})";
    char escaped[8]{};
    CHECK(!json_field::string(unicode, std::strlen(unicode), "a", escaped,
                              sizeof(escaped)), "unicode escape rejected");
}

void test_parsers() {
    std::printf("parsers\n");
    Fixture f;
    CHECK(load_fixture(f), "documented fixtures parse");
    CHECK(std::strcmp(f.gamma.condition_id, kConditionId) == 0, "gamma condition id");
    CHECK(f.gamma.token_count == 2 && f.gamma.has_neg_risk && !f.gamma.neg_risk,
          "gamma tokens/neg risk");
    CHECK(std::strcmp(f.gamma.token_ids[1], kNoToken) == 0, "gamma second token");
    CHECK(f.clob.has_tick && f.clob.tick_size == 10000, "clob mts");
    CHECK(f.clob.has_min_size && f.clob.min_order_size == 5000000, "clob mos");
    CHECK(f.clob.has_fee_details && f.clob.fee_rate == 0.04 &&
          f.clob.fee_exponent == 1.0 && f.clob.fee_taker_only, "clob fee details");
    CHECK(f.book.has_tick && f.book.tick_size == 10000, "book tick");
    CHECK(f.book.has_neg_risk && !f.book.neg_risk, "book neg risk");

    uint64_t tick = 0;
    CHECK(crowdintel::parse_tick_size_response(R"({"minimum_tick_size":0.005})",
                                               std::strlen(R"({"minimum_tick_size":0.005})"),
                                               tick) && tick == 5000,
          "tick-size response");
    double fee = 0.0;
    const char* fee_doc = R"({"base_fee":700})";
    CHECK(crowdintel::parse_fee_rate_response(fee_doc, std::strlen(fee_doc), fee) &&
          fee == 700.0, "fee-rate response");
    int64_t server = 0;
    const char* bare = "1759420800";
    CHECK(crowdintel::parse_server_time(bare, std::strlen(bare), server) &&
          server == 1759420800, "bare server time");
    const char* wrapped = R"({"time":1759420800})";
    CHECK(crowdintel::parse_server_time(wrapped, std::strlen(wrapped), server),
          "wrapped server time");
    const char* millis = R"({"time":1759420800000})";
    CHECK(crowdintel::parse_server_time(millis, std::strlen(millis), server) &&
          server == 1759420800, "millisecond server time");
    const char* garbage = R"({"time":"soon"})";
    CHECK(!crowdintel::parse_server_time(garbage, std::strlen(garbage), server),
          "garbage server time rejected");
}

// ── resolution matrix ────────────────────────────────────────────────────────

void test_happy_path() {
    std::printf("resolution_happy_path\n");
    Fixture f;
    CHECK(load_fixture(f), "fixture");
    const crowdintel::MarketResolution r = resolve_fixture(f, "Yes");
    CHECK(r.ok(), "resolves");
    CHECK(std::strcmp(r.runtime.token_id_dec, kYesToken) == 0, "selected Yes token");
    CHECK(std::strcmp(r.runtime.outcome_label, "Yes") == 0, "outcome label");
    CHECK(r.runtime.tick_size == 10000, "tick");
    CHECK(r.runtime.min_order_size == 5000000, "min size");
    CHECK(r.runtime.fee_rate == 0.04 && r.runtime.fee_exponent == 1.0, "fee");
    CHECK(!r.runtime.neg_risk, "neg risk");
    CHECK(std::memcmp(r.runtime.exchange,
                      crowdintel::K_STANDARD_EXCHANGE_ADDRESS, 20) == 0,
          "standard exchange selected");
    CHECK(r.runtime.clock_offset_ms == 1000, "clock offset");
    CHECK(r.runtime.token_id_be[31] != 0 || r.runtime.token_id_be[0] != 0,
          "token uint256 decoded");

    const crowdintel::MarketResolution yEs = resolve_fixture(f, "yEs");
    CHECK(yEs.ok() && std::strcmp(yEs.runtime.token_id_dec, kYesToken) == 0,
          "case-insensitive outcome selection");
    // Selecting "No" is rejected here because the fixture book belongs to the
    // Yes token: the venue echo must match the selected asset.
    CHECK(resolve_fixture(f, "no").issue ==
              crowdintel::ResolutionIssue::IDENTITY_MISMATCH,
          "book of a different asset rejected");
}

void test_outcome_errors() {
    std::printf("resolution_outcome_errors\n");
    Fixture f;
    CHECK(load_fixture(f), "fixture");
    const crowdintel::MarketResolution missing =
        resolve_fixture(f, "Maybe");
    CHECK(missing.issue == crowdintel::ResolutionIssue::TOKEN_NOT_FOUND,
          "unknown outcome rejected");
    const crowdintel::MarketResolution ambiguous = resolve_fixture(f, nullptr);
    CHECK(ambiguous.issue == crowdintel::ResolutionIssue::OUTCOME_NOT_FOUND,
          "missing outcome on multi-outcome market rejected");

    // A single-outcome market needs no operator label: the only token is used.
    Fixture single;
    CHECK(load_fixture(single), "single-token fixture");
    single.gamma.token_count = 1;
    std::memcpy(single.gamma.token_ids[0], kYesToken, std::strlen(kYesToken) + 1);
    const crowdintel::MarketResolution implied = resolve_fixture(single, nullptr);
    CHECK(implied.ok() && std::strcmp(implied.runtime.outcome_label, "Yes") == 0 &&
              std::strcmp(implied.runtime.token_id_dec, kYesToken) == 0,
          "single-token market resolved without label");

    // Gamma and CLOB disagreeing on the outcome vocabulary of the same token
    // is a broken identity, not a preference to resolve.
    Fixture relabelled;
    CHECK(load_fixture(relabelled), "relabelled fixture");
    relabelled.gamma.token_count = 1;
    std::memcpy(relabelled.gamma.token_ids[0], kYesToken,
                std::strlen(kYesToken) + 1);
    std::memcpy(relabelled.gamma.outcome_labels[0], "Up", 3);
    CHECK(resolve_fixture(relabelled, nullptr).issue ==
              crowdintel::ResolutionIssue::IDENTITY_MISMATCH,
          "gamma/clob outcome vocabulary mismatch rejected");

    // Gamma announcing a token the CLOB market document does not know: the
    // venue identity is broken and resolution must fail closed.
    Fixture unknown;
    CHECK(load_fixture(unknown), "unknown-token fixture");
    unknown.gamma.token_count = 1;
    std::memcpy(unknown.gamma.outcome_labels[0], "Up", 3);
    char unknown_id[crowdintel::K_TOKEN_ID_CHARS];
    std::snprintf(unknown_id, sizeof(unknown_id), "%s", kNoToken);
    unknown_id[std::strlen(unknown_id) - 1] = '3';
    std::memcpy(unknown.gamma.token_ids[0], unknown_id,
                std::strlen(unknown_id) + 1);
    CHECK(resolve_fixture(unknown, nullptr).issue ==
              crowdintel::ResolutionIssue::TOKEN_NOT_FOUND,
          "unknown gamma token rejected");
}

void test_status_errors() {
    std::printf("resolution_status_errors\n");
    Fixture f;
    CHECK(load_fixture(f), "fixture");

    {
        std::string closed = replace_once(kGamma, "\"closed\":false", "\"closed\":true");
        Fixture g;
        CHECK(load_fixture(g, closed.c_str()), "closed fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::MARKET_NOT_TRADING,
              "closed market rejected");
    }
    {
        std::string not_accepting =
            replace_once(kGamma, "\"acceptingOrders\":true", "\"acceptingOrders\":false");
        Fixture g;
        CHECK(load_fixture(g, not_accepting.c_str()), "not-accepting fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::MARKET_NOT_TRADING,
              "market not accepting orders rejected");
    }
    {
        std::string no_book =
            replace_once(kGamma, "\"enableOrderBook\":true", "\"enableOrderBook\":false");
        Fixture g;
        CHECK(load_fixture(g, no_book.c_str()), "no-book fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::MARKET_NOT_TRADING,
              "order book disabled rejected");
    }
    {
        // An absent status flag is not a default: parsing must fail.
        std::string missing =
            replace_once(kGamma, "\"active\":true,", "");
        Fixture g;
        CHECK(!load_fixture(g, missing.c_str()), "missing active flag fails parse");
    }
    {
        std::string restricted =
            replace_once(kGamma, "\"restricted\":false", "\"restricted\":true");
        Fixture g;
        CHECK(load_fixture(g, restricted.c_str()), "restricted fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::MARKET_NOT_TRADING,
              "restricted market rejected");
    }
}

void test_metadata_mismatches() {
    std::printf("resolution_metadata_mismatches\n");
    Fixture f;
    CHECK(load_fixture(f), "fixture");

    {
        const std::string book = replace_once(
            kBook, "\"tick_size\":\"0.01\"", "\"tick_size\":\"0.001\"");
        Fixture g;
        CHECK(load_fixture(g, kGamma, kClobMarkets, book.c_str()), "tick mismatch fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::TICK_MISMATCH,
              "book tick mismatch rejected");
    }
    {
        Fixture g;
        CHECK(load_fixture(g), "fixture");
        g.tick_probe = 1000;  // /tick-size disagrees with clob-markets/book
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::TICK_MISMATCH,
              "tick-size probe mismatch rejected");
    }
    {
        const std::string clob = replace_once(kClobMarkets, "\"mts\":0.01", "\"mts\":0.02");
        Fixture g;
        CHECK(load_fixture(g, kGamma, clob.c_str()), "unsupported grid fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::TICK_MISMATCH ||
              resolve_fixture(g).issue == crowdintel::ResolutionIssue::TICK_UNSUPPORTED,
              "unsupported grid rejected");
    }
    {
        const std::string clob = replace_once(kClobMarkets, "\"mos\":5", "\"mos\":10");
        Fixture g;
        CHECK(load_fixture(g, kGamma, clob.c_str()), "min size fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::MIN_SIZE_MISMATCH,
              "min size mismatch rejected");
    }
    {
        const std::string gamma = replace_once(kGamma, "\"negRisk\":false", "\"negRisk\":true");
        Fixture g;
        CHECK(load_fixture(g, gamma.c_str()), "neg risk fixture");
        const crowdintel::MarketResolution r = resolve_fixture(g);
        CHECK(r.issue == crowdintel::ResolutionIssue::NEG_RISK_MISMATCH,
              "neg risk mismatch rejected");
    }
    {
        // Gamma omitting the flag entirely is a hard failure, not false.
        const std::string gamma = replace_once(kGamma, "\"negRisk\":false,", "");
        Fixture g;
        CHECK(load_fixture(g, gamma.c_str()), "neg risk absent fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::MISSING_FIELD,
              "absent neg risk rejected");
    }
    {
        const std::string book = replace_once(
            kBook, kYesToken, kNoToken);
        Fixture g;
        CHECK(load_fixture(g, kGamma, kClobMarkets, book.c_str()), "asset fixture");
        CHECK(resolve_fixture(g).issue == crowdintel::ResolutionIssue::IDENTITY_MISMATCH,
              "book asset id mismatch rejected");
    }
}

void test_fee_and_clock() {
    std::printf("resolution_fee_and_clock\n");
    {
        // fd absent: the bps base fee is used with the published exponent 1.
        const std::string clob = replace_once(kClobMarkets, "\"tbf\":0", "\"tbf\":700");
        Fixture f;
        CHECK(load_fixture(f, kGamma, clob.c_str()), "tbf fixture");
        f.fee_probe = 700.0;
        const crowdintel::MarketResolution r = resolve_fixture(f);
        CHECK(r.ok(), "tbf-only resolution succeeds");
        CHECK(r.runtime.fee_rate == 0.07 && r.runtime.fee_exponent == 1.0 &&
              std::strcmp(r.runtime.fee_source, "tbf") == 0, "tbf fee");
    }
    {
        // fd wins when it is stricter than tbf.
        const std::string clob = replace_once(kClobMarkets, "\"tbf\":0", "\"tbf\":100");
        Fixture f;
        CHECK(load_fixture(f, kGamma, clob.c_str()), "fd-over-tbf fixture");
        f.fee_probe = 100.0;
        const crowdintel::MarketResolution r = resolve_fixture(f);
        CHECK(r.ok() && r.runtime.fee_rate == 0.04 &&
              std::strcmp(r.runtime.fee_source, "fd") == 0, "fd fee kept");
    }
    {
        // No fd and no tbf: nothing to size against -> fail closed.
        std::string clob = replace_once(kClobMarkets, "\"tbf\":0,", "");
        clob = replace_once(clob, "\"fd\":{\"r\":0.04,\"e\":1,\"to\":true},", "");
        Fixture f;
        CHECK(load_fixture(f, kGamma, clob.c_str()), "no-fee fixture");
        CHECK(resolve_fixture(f).issue == crowdintel::ResolutionIssue::FEE_UNAVAILABLE,
              "missing fee rejected");
    }
    {
        // The dedicated /fee-rate endpoint must not contradict the document.
        Fixture f;
        CHECK(load_fixture(f), "fixture");
        f.fee_probe = 700.0;
        CHECK(resolve_fixture(f).issue == crowdintel::ResolutionIssue::FEE_OUT_OF_RANGE,
              "fee probe disagreement rejected");
    }
    {
        // Clock skew beyond the configured bound blocks trading.
        Fixture f;
        CHECK(load_fixture(f), "fixture");
        f.local_time = f.server_time - 5;  // 5 s off
        CHECK(resolve_fixture(f).issue == crowdintel::ResolutionIssue::CLOCK_SKEW,
              "clock skew rejected");
    }
}

void test_malformed_documents() {
    std::printf("resolution_malformed\n");
    Fixture f;
    CHECK(load_fixture(f), "fixture");
    CHECK(resolve_fixture(f).ok(), "baseline resolves");

    crowdintel::GammaMarketView gamma;
    CHECK(!crowdintel::parse_gamma_market(R"({"conditionId":)", 15, gamma),
          "truncated gamma rejected");
    const char* duplicate = R"({"conditionId":"0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75","conditionId":"0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75"})";
    CHECK(!crowdintel::parse_gamma_market(duplicate, std::strlen(duplicate), gamma),
          "duplicate field rejected");
    const char* bad_condition = R"({"conditionId":"0xZZ","outcomes":"[\"Yes\"]","clobTokenIds":"[\"1\"]","active":true,"closed":false,"acceptingOrders":true,"enableOrderBook":true})";
    CHECK(!crowdintel::parse_gamma_market(bad_condition,
                                          std::strlen(bad_condition), gamma),
          "non-hex condition id rejected");

    crowdintel::ClobMarketDetailsView clob;
    const char* negative_tick = R"({"mts":-0.01,"mos":5})";
    CHECK(crowdintel::parse_clob_market_details(negative_tick,
                                                std::strlen(negative_tick), clob) &&
          !clob.has_tick, "negative tick rejected as absent");
    const char* too_many_tokens =
        R"({"t":[{"t":"1","o":"A"},{"t":"2","o":"B"},{"t":"3","o":"C"},{"t":"4","o":"D"},{"t":"5","o":"E"}]})";
    CHECK(!crowdintel::parse_clob_market_details(too_many_tokens,
                                                 std::strlen(too_many_tokens), clob),
          "token overflow rejected");
}

void test_fee_curve() {
    std::printf("fee_curve\n");
    crowdintel::MarketRuntime runtime;
    runtime.fee_rate = 0.07;
    runtime.fee_exponent = 1.0;
    CHECK(std::fabs(crowdintel::fee_per_share(runtime, 0.5) - 0.0175) < 1e-12,
          "crypto 0.07 at 50c = 1.75c/share (docs table)");
    CHECK(std::fabs(crowdintel::fee_per_share(runtime, 0.1) - 0.0063) < 1e-12,
          "crypto 0.07 at 10c matches docs table");
    runtime.fee_exponent = 2.0;
    CHECK(std::fabs(crowdintel::fee_per_share(runtime, 0.5) - 0.004375) < 1e-12,
          "exponent 2 squares the price component");
    CHECK(crowdintel::fee_per_share(runtime, 0.0) == 0.0 &&
          crowdintel::fee_per_share(runtime, 1.0) == 0.0,
          "boundary prices produce no fee");

    // Runtime accessors on a mock config keep the historic coefficient.
    setenv("BOT_PRIVATE_KEY_HEX",
           "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b", 1);
    MarketConfig mock;
    MarketConfig::LoadOptions replay_options;
    replay_options.force_replay = true;
    CHECK(mock.load(replay_options) == nullptr, "replay config loads");
    CHECK(mock.trading_parameters_ready(), "mock parameters ready");
    CHECK(mock.effective_tick() == 10000 &&
          mock.effective_min_size() == 5000000, "mock accessors use config");
    unsetenv("BOT_PRIVATE_KEY_HEX");
}

void test_live_config_rejects_venue_params() {
    std::printf("live_config_venue_params\n");
    const char* key =
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
    setenv("BOT_PRIVATE_KEY_HEX", key, 1);
    setenv("BOT_MODE", "live", 1);
    setenv("BOT_MARKET_SLUG", "aliens-before-2027", 1);
    unsetenv("BOT_TICK_SIZE");
    // Inspector role: this test validates the live *document* (no arming, no
    // credentials), exactly what crowdintel-metadata parses.
    MarketConfig::LoadOptions inspector;
    inspector.role = MarketConfig::Role::kInspector;
    setenv("CLOB_HOST", "https://clob.polymarket.com", 1);
    setenv("WS_HOST", "wss://ws-subscriptions-clob.polymarket.com/ws/market", 1);
    {
        MarketConfig live;
        CHECK(live.load(inspector) == nullptr, "live config loads without venue params");
        CHECK(!live.trading_parameters_ready(),
              "live mode is not ready before metadata resolution");
        CHECK(live.effective_tick() == 0 && live.effective_min_size() == 0,
              "live accessors fail closed without runtime");
    }
    setenv("BOT_TICK_SIZE", "0.01", 1);
    {
        MarketConfig live;
        CHECK(live.load(inspector) != nullptr,
              "live mode rejects BOT_TICK_SIZE");
    }
    unsetenv("BOT_TICK_SIZE");
    setenv("BOT_NEG_RISK", "0", 1);
    {
        MarketConfig live;
        CHECK(live.load(inspector) != nullptr, "live mode rejects BOT_NEG_RISK");
    }
    unsetenv("BOT_NEG_RISK");
    unsetenv("BOT_MODE");
    unsetenv("BOT_MARKET_SLUG");
    unsetenv("BOT_PRIVATE_KEY_HEX");
    unsetenv("CLOB_HOST");
    unsetenv("WS_HOST");
}

}  // namespace

int main() {
    std::printf("== CROWDINTEL market metadata tests ==\n");
    test_json_field_basics();
    test_parsers();
    test_happy_path();
    test_outcome_errors();
    test_status_errors();
    test_metadata_mismatches();
    test_fee_and_clock();
    test_malformed_documents();
    test_fee_curve();
    test_live_config_rejects_venue_params();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
