// Phase 6 unit tests: account-state parsing and the preflight policy.
//
// Everything here is offline: the transport is scripted with payloads taken
// from the official examples (docs.polymarket.com/api-spec/clob-openapi.yaml:
// BalanceAllowanceResponse, ClosedOnlyResponse) so the test proves the parser
// and the decision table, not the network.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "account_state.hpp"
#include "preflight.hpp"

static int g_failures = 0;

#define CHECK(cond, label)                                                   \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL: %s (%s:%d)\n", (label), __FILE__, __LINE__);  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

constexpr const char* kOwner = "f4f247b7-4ac7-ff29-a152-04fda0a8755a";
constexpr const char* kExchange = "0xe111180000d2663c0091e4f400237545b87b996b";
constexpr const char* kExchangeMiXeD = "0xE111180000d2663C0091e4f400237545B87B996B";
constexpr const char* kTokenId =
    "15871154585880608648532107628464183779895785213830018178010423617714102767076";

// The official example, plus a second spender so the map has more than one key.
std::string collateral_body(const char* balance, const char* exchange_amount) {
    return std::string("{\"balance\":\"") + balance + "\",\"allowances\":{\"" +
           kExchange + "\":\"" + exchange_amount +
           "\",\"0xabcdefabcdefabcdefabcdefabcdefabcdefabcd\":\"1000000\"}}";
}

std::string conditional_body(const char* balance, const char* exchange_amount) {
    return std::string("{\"balance\":\"") + balance + "\",\"allowances\":{\"" +
           kExchange + "\":\"" + exchange_amount + "\"}}";
}

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

void make_config(MarketConfig& cfg) {
    std::snprintf(cfg.owner_api_key, sizeof(cfg.owner_api_key), "%s", kOwner);
    std::snprintf(cfg.api_address_hex, sizeof(cfg.api_address_hex),
                  "%s", kExchangeMiXeD);
    std::snprintf(cfg.api_secret_b64, sizeof(cfg.api_secret_b64),
                  "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=");
    std::snprintf(cfg.api_passphrase, sizeof(cfg.api_passphrase),
                  "0123456789abcdef0123456789abcdef");
    std::snprintf(cfg.clob_host, sizeof(cfg.clob_host),
                  "https://clob.polymarket.com");
    cfg.runtime.resolved = true;
    std::snprintf(cfg.runtime.token_id_dec, sizeof(cfg.runtime.token_id_dec),
                  "%s", kTokenId);
    std::snprintf(cfg.runtime.condition_id, sizeof(cfg.runtime.condition_id),
                  "%s",
                  "0x747dc809fb79e1b05be09c42d6179459a58de2ef3e40f02484a4e1260f741f75");
    cfg.signature_type = 0;
    cfg.max_order_usd = 100.0;
    cfg.initial_position_shares = 0;
}

preflight::Requirements make_requirements(uint64_t collateral_f6,
                                          uint64_t conditional_f6) {
    preflight::Requirements requirements;
    std::snprintf(requirements.exchange_hex, sizeof(requirements.exchange_hex),
                  "%s", kExchange);
    requirements.collateral_required_f6 = collateral_f6;
    requirements.conditional_required_f6 = conditional_f6;
    requirements.signature_type = 0;
    return requirements;
}

std::string time_body(int64_t offset_seconds) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "{\"time\":%lld}",
                  static_cast<long long>(::time(nullptr) + offset_seconds));
    return buffer;
}

// A ready script: clock, closed-only, collateral, conditional.
ScriptedTransport ready_transport(const std::string& collateral,
                                  const std::string& conditional) {
    ScriptedTransport transport;
    transport.routes = {
        {"/time", 200, time_body(0)},
        {"/auth/ban-status/closed-only", 200, "{\"closed_only\":false}"},
        {"/balance-allowance", 200, collateral},
        {"/balance-allowance", 200, conditional},
    };
    return transport;
}

}  // namespace

// ── parser ───────────────────────────────────────────────────────────────────

void test_parser_accepts_documented_shape() {
    std::printf("preflight_parser\n");
    const std::string body = collateral_body("100.5", "250");
    account::State state;
    CHECK(account::parse_balance_allowance(body.c_str(), body.size(), state),
          "documented shape parses");
    CHECK(state.balance_f6 == 100500000ull, "balance is 6-decimal fixed");
    CHECK(state.spender_count == 2, "both spenders kept");
    CHECK(state.declared_spenders == 2, "spenders counted");
    CHECK(!state.unlimited_sentinel, "no sentinel");

    char canonical[account::kAddressChars];
    CHECK(account::canonical_address("0xE111180000d2663C0091e4f400237545B87B996B",
                                     canonical) &&
              std::strcmp(canonical,
                          "0xe111180000d2663c0091e4f400237545b87b996b") == 0,
          "addresses canonicalize to lowercase");
    CHECK(!account::canonical_address("0x1234", canonical),
          "short address rejected");
    CHECK(!account::canonical_address(
              "0xzz11180000d2663C0091e4f400237545B87B996B", canonical),
          "non-hex address rejected");

    uint64_t amount = 0;
    bool unlimited = false;
    CHECK(account::find_allowance(state, kExchangeMiXeD, amount, unlimited) &&
              amount == 250000000ull && !unlimited,
          "lookup is case-insensitive");
    CHECK(!account::find_allowance(state, "0x0000000000000000000000000000000000000001",
                                   amount, unlimited),
          "absent spender is not found");

    // "max" is the Data API sentinel for an unlimited grant; the CLOB schema
    // only says "amount", so this is accepted and counted explicitly instead of
    // being guessed silently.
    const std::string unlimited_body =
        std::string("{\"balance\":\"1\",\"allowances\":{\"") + kExchange +
        "\":\"max\"}}";
    CHECK(account::parse_balance_allowance(unlimited_body.c_str(),
                                           unlimited_body.size(), state) &&
              state.spender_count == 1 && state.unlimited_sentinel &&
              state.spenders[0].unlimited,
          "max sentinel accepted and flagged");
}

void test_parser_rejects_deviations() {
    std::printf("preflight_parser_rejections\n");
    account::State state;
    const std::string missing_map =
        R"({"balance":"100","allowance":"100"})";
    CHECK(!account::parse_balance_allowance(missing_map.c_str(),
                                            missing_map.size(), state),
          "legacy singular allowance is not a map: rejected");
    const std::string missing_balance = R"({"allowances":{}})";
    CHECK(!account::parse_balance_allowance(missing_balance.c_str(),
                                            missing_balance.size(), state),
          "missing balance rejected");
    const std::string not_object =
        std::string(R"({"balance":"1","allowances":")") + kExchange + "\"}";
    CHECK(!account::parse_balance_allowance(not_object.c_str(),
                                            not_object.size(), state),
          "allowances must be an object");
    const std::string duplicate_keys =
        std::string("{\"balance\":\"1\",\"allowances\":{\"") + kExchange +
        "\":\"1\",\"" + kExchangeMiXeD + "\":\"2\"}}";
    CHECK(!account::parse_balance_allowance(duplicate_keys.c_str(),
                                            duplicate_keys.size(), state),
          "case-variant duplicate spender rejected");
    const std::string bad_address =
        R"({"balance":"1","allowances":{"0x1234":"1"}})";
    CHECK(!account::parse_balance_allowance(bad_address.c_str(),
                                            bad_address.size(), state),
          "malformed spender rejected");
    const std::string bad_amount =
        std::string("{\"balance\":\"1\",\"allowances\":{\"") + kExchange +
        "\":\"12,5\"}}";
    CHECK(!account::parse_balance_allowance(bad_amount.c_str(),
                                            bad_amount.size(), state),
          "unparsable amount rejected");
    const std::string fractional_allowance =
        std::string("{\"balance\":\"1\",\"allowances\":{\"") + kExchange +
        "\":\"1.0000001\"}}";
    CHECK(!account::parse_balance_allowance(fractional_allowance.c_str(),
                                            fractional_allowance.size(), state),
          "amount not exact at 1e-6 rejected");
    const std::string huge_balance =
        R"({"balance":"1000000000000000000","allowances":{"0xe111180000d2663c0091e4f400237545b87b996b":"1"}})";
    CHECK(!account::parse_balance_allowance(huge_balance.c_str(),
                                            huge_balance.size(), state),
          "1e18 balance (the spec's illustrative example) is rejected: it is "
          "not a 6-decimal value");
    // Bounded table: 9 spenders overflow kMaxSpenders.
    std::string many = R"({"balance":"1","allowances":{)";
    for (int i = 0; i < 9; ++i) {
        char address[43];
        std::snprintf(address, sizeof(address),
                      "0x%040x", static_cast<unsigned>(0x1000 + i));
        many += std::string(i ? ",\"" : "\"") + address + "\":\"1\"";
    }
    many += "}}";
    CHECK(!account::parse_balance_allowance(many.c_str(), many.size(), state),
          "more spenders than the bounded table is rejected");
}

// ── policy ───────────────────────────────────────────────────────────────────

void test_policy_ready() {
    std::printf("preflight_policy_ready\n");
    MarketConfig cfg;
    make_config(cfg);
    auto requirements = make_requirements(100000000ull /*100.0*/,
                                          20000000ull /*20.0*/);
    ScriptedTransport transport = ready_transport(
        collateral_body("150", "500"), conditional_body("25", "1"));
    preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                      requirements);
    char error[256];
    CHECK(preflight.run(error, sizeof(error)) == preflight::Verdict::kReady,
          "ready account");
    CHECK(preflight.reason() == preflight::Reason::kReady, "reason ready");
    CHECK(std::strcmp(error, "") == 0, "no error text");
    const preflight::Report& report = preflight.report();
    CHECK(report.collateral_balance_f6 == 150000000ull &&
              report.collateral_allowance_f6 == 500000000ull,
          "collateral numbers reported");
    CHECK(report.conditional_checked && report.conditional_approval_present &&
              report.conditional_balance_f6 == 25000000ull,
          "conditional numbers reported");
    CHECK(transport.requests.size() == 4 &&
              transport.requests[2] ==
                  "/balance-allowance?asset_type=COLLATERAL&signature_type=0" &&
              transport.requests[3] ==
                  std::string("/balance-allowance?asset_type=CONDITIONAL&") +
                      "token_id=" + kTokenId + "&signature_type=0",
          "queries carry asset_type/token_id/signature_type");
}

void test_policy_unlimited_and_cases() {
    std::printf("preflight_policy_unlimited\n");
    MarketConfig cfg;
    make_config(cfg);
    auto requirements = make_requirements(100000000ull, 0);
    // Unlimited collateral allowance, conditional approval present.
    std::string collateral =
        std::string("{\"balance\":\"100\",\"allowances\":{\"") +
        kExchangeMiXeD + "\":\"max\"}}";  // mixed-case key must still match
    ScriptedTransport transport =
        ready_transport(collateral, conditional_body("0", "1"));
    preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                      requirements);
    char error[256];
    CHECK(preflight.run(error, sizeof(error)) == preflight::Verdict::kReady,
          "unlimited allowance is ready");
    CHECK(preflight.report().collateral_allowance_unlimited &&
              preflight.report().unlimited_sentinels == 1,
          "sentinel counted");
    // Exact boundary: balance == required is enough.
    requirements.collateral_required_f6 = 100000000ull;
    ScriptedTransport boundary =
        ready_transport(collateral_body("100", "100"), conditional_body("0", "1"));
    preflight::Preflight<ScriptedTransport> boundary_preflight(boundary, cfg,
                                                               requirements);
    CHECK(boundary_preflight.run(error, sizeof(error)) ==
              preflight::Verdict::kReady,
          "exactly enough is enough");
}

void test_policy_blocks() {
    std::printf("preflight_policy_blocks\n");
    MarketConfig cfg;
    make_config(cfg);
    auto requirements = make_requirements(100000000ull, 0);
    char error[256] = "stale";

    {  // insufficient collateral balance
        ScriptedTransport transport =
            ready_transport(collateral_body("99.5", "500"),
                            conditional_body("0", "1"));
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "short balance blocks");
        CHECK(preflight.reason() == preflight::Reason::kCollateralBalance,
              "reason is the balance");
        CHECK(std::strstr(error, "below the required") != nullptr,
              "detail names the shortfall");
    }
    {  // spender absent from the map
        ScriptedTransport transport = ready_transport(
            R"({"balance":"500","allowances":{"0x0000000000000000000000000000000000000001":"500"}})",
            conditional_body("0", "1"));
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "absent spender blocks");
        CHECK(preflight.reason() == preflight::Reason::kCollateralAllowance,
              "reason is the allowance");
        CHECK(std::strstr(error, "absent") != nullptr, "detail says absent");
    }
    {  // allowance present but too small
        ScriptedTransport transport =
            ready_transport(collateral_body("500", "10"),
                            conditional_body("0", "1"));
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "small allowance blocks");
        CHECK(preflight.reason() == preflight::Reason::kCollateralAllowance,
              "reason is the allowance");
    }
    {  // conditional operator approval missing (a SELL becomes possible once a
       // BUY fills, so this is blocking even with no declared inventory)
        ScriptedTransport transport =
            ready_transport(collateral_body("500", "500"),
                            R"({"balance":"0","allowances":{"0x0000000000000000000000000000000000000001":"1"}})");
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "missing conditional approval blocks");
        CHECK(preflight.reason() ==
                  preflight::Reason::kConditionalAllowance,
              "reason is the conditional approval");
    }
    {  // declared inventory larger than the venue balance
        auto inventory = make_requirements(100000000ull, 50000000ull);
        ScriptedTransport transport = ready_transport(
            collateral_body("500", "500"), conditional_body("10", "1"));
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          inventory);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "declared inventory above the venue balance blocks");
        CHECK(preflight.reason() == preflight::Reason::kConditionalBalance,
              "reason is the conditional balance");
    }
    {  // closed-only account
        ScriptedTransport transport = ready_transport(
            collateral_body("500", "500"), conditional_body("0", "1"));
        transport.routes[1].body = R"({"closed_only":true})";
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "closed-only blocks");
        CHECK(preflight.reason() == preflight::Reason::kClosedOnly,
              "reason is closed-only");
    }
    {  // credentials rejected by the venue
        ScriptedTransport transport = ready_transport(
            collateral_body("500", "500"), conditional_body("0", "1"));
        transport.routes[1].http_code = 401;
        transport.routes[1].body = R"({"error":"Invalid API key"})";
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "401 blocks");
        CHECK(preflight.reason() == preflight::Reason::kCredentialsRejected &&
              preflight.report().auth_failures == 1,
              "reason is credentials");
    }
    {  // malformed balance payload
        ScriptedTransport transport =
            ready_transport(R"({"balance":"1"})", conditional_body("0", "1"));
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "missing allowances map blocks");
        CHECK(preflight.reason() == preflight::Reason::kMalformedResponse &&
              preflight.report().malformed_payloads == 1,
              "reason is malformed");
    }
    {  // clock outside the configured tolerance
        ScriptedTransport transport = ready_transport(
            collateral_body("500", "500"), conditional_body("0", "1"));
        transport.routes[0].body = time_body(-3600);  // in range, out of tolerance
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "clock skew blocks");
        CHECK(preflight.reason() == preflight::Reason::kClockUnsynced,
              "reason is the clock");
    }
    {  // transport failure is its own verdict: the venue was never asked
        ScriptedTransport transport = ready_transport(
            collateral_body("500", "500"), conditional_body("0", "1"));
        transport.routes[1].transport_ok = false;
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kTransportError,
              "transport error is not a venue verdict");
        CHECK(preflight.reason() == preflight::Reason::kTransport,
              "reason is transport");
    }
    {  // unusable configuration (no exchange -> no proven spender)
        preflight::Requirements no_exchange;
        ScriptedTransport transport = ready_transport(
            collateral_body("500", "500"), conditional_body("0", "1"));
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          no_exchange);
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kBlocked,
              "missing exchange blocks");
        CHECK(preflight.reason() == preflight::Reason::kConfigIncomplete,
              "reason is configuration");
    }
}

void test_policy_refresh_is_opt_in() {
    std::printf("preflight_refresh_opt_in\n");
    MarketConfig cfg;
    make_config(cfg);
    auto requirements = make_requirements(100000000ull, 0);
    requirements.refresh_allowances = true;
    {
        ScriptedTransport transport;
        transport.routes = {
            {"/time", 200, time_body(0)},
            {"/balance-allowance/update", 200, "{}"},
            {"/auth/ban-status/closed-only", 200, "{\"closed_only\":false}"},
            {"/balance-allowance", 200, collateral_body("500", "500")},
            {"/balance-allowance", 200, conditional_body("0", "1")},
        };
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        char error[256];
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kReady,
              "refresh then check");
        CHECK(preflight.report().refreshes == 1, "refresh counted");
        CHECK(transport.requests[1] ==
                  "/balance-allowance/update?asset_type=COLLATERAL"
                  "&signature_type=0",
              "refresh is signed on the bare path with query params");
    }
    {
        requirements.refresh_allowances = false;
        ScriptedTransport transport = ready_transport(
            collateral_body("500", "500"), conditional_body("0", "1"));
        preflight::Preflight<ScriptedTransport> preflight(transport, cfg,
                                                          requirements);
        char error[256];
        CHECK(preflight.run(error, sizeof(error)) ==
                  preflight::Verdict::kReady,
              "no refresh by default");
        CHECK(preflight.report().refreshes == 0, "no refresh counted");
        CHECK(transport.requests.size() == 4 &&
                  transport.requests[1] == "/auth/ban-status/closed-only?",
              "second request is the closed-only check");
    }
}

int main() {
    std::printf("== CROWDINTEL preflight tests ==\n");
    test_parser_accepts_documented_shape();
    test_parser_rejects_deviations();
    test_policy_ready();
    test_policy_unlimited_and_cases();
    test_policy_blocks();
    test_policy_refresh_is_opt_in();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
