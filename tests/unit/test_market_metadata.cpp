// ─────────────────────────────────────────────────────────────────────────────
// test_market_metadata: read Motor C's parameters from the venue object
//
// The trap this file pins: rewardsMaxSpread arrives as FRACTIONAL CENTS (3.5),
// not an integer. Carrying it as integer cents rounds the eligibility band by up
// to 15%, which silently puts quotes outside the reward program while they keep
// carrying full adverse risk.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../core/src/market_metadata.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
                ++g_failures; }                                                \
    } while (0)

using namespace crowdintel;

// Real shape captured from Gamma on 2026-10-29.
static const char* kLiveMarket =
    R"([{"id":"682705","slug":"will-benjamin-netanyahu-be-the-next-prime-minister-of-israel",)"
    R"("rewardsMaxSpread":3.5,"rewardsMinSize":200,"competitive":0.9969,)"
    R"("clobRewards":[{"id":"4566681",)"
    R"("conditionId":"0x7586a96520578acaaaa4ea84a2582f197f84255da1f3392a7aa300386c187b37",)"
    R"("assetAddress":"0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174",)"
    R"("rewardsAmount":0,"rewardsDailyRate":3000,)"
    R"("startDate":"2026-10-27","endDate":"2500-12-31"}],)"
    R"("orderPriceMinTickSize":0.001,)"
    R"("feeSchedule":{"exponent":1,"rate":0.07,"takerOnly":true,"rebateRate":0.2}}])";

static void test_live_shape() {
    std::printf("live_shape\n");
    MarketRewards r{};
    const RewardParse p = parse_market_rewards(kLiveMarket,
                                               std::strlen(kLiveMarket), r);
    CHECK(p == RewardParse::Ok, "the live market object parses");
    CHECK(r.max_spread_bps == 350u,
          "rewardsMaxSpread 3.5 cents becomes 350 bps, not 3");
    CHECK(r.min_size_shares == 200000000ULL,
          "rewardsMinSize 200 shares becomes 2e8 in 1e6 fixed point");
    CHECK(r.daily_rate_usdc == 3000, "rewardsDailyRate summed into the pool");
    CHECK(r.has_program, "an active program is reported");
    CHECK(r.tick_size_units == 1000ULL,
          "orderPriceMinTickSize 0.001 becomes 1000 units of 1e6");
    CHECK(r.taker_fee_rate == 0.07, "feeSchedule.rate read from the nested object");
    // The distractor that a flat search would have picked up.
    CHECK(r.taker_fee_rate != 3000.0,
          "rewardsDailyRate is NOT mistaken for the fee rate");
}

static void test_fractional_band() {
    std::printf("fractional_band\n");
    // Every value observed live is fractional; none must truncate to zero or to
    // the wrong basis point.
    struct Case { const char* json; uint32_t bps; };
    const Case cases[] = {
        {R"({"rewardsMaxSpread":4.5,"rewardsMinSize":50})", 450u},
        {R"({"rewardsMaxSpread":6.5,"rewardsMinSize":150})", 650u},
        {R"({"rewardsMaxSpread":3.5,"rewardsMinSize":200})", 350u},
        {R"({"rewardsMaxSpread":1,"rewardsMinSize":20})", 100u},
        {R"({"rewardsMaxSpread":0.5,"rewardsMinSize":20})", 50u},
    };
    bool all = true;
    for (const Case& c : cases) {
        MarketRewards r{};
        if (parse_market_rewards(c.json, std::strlen(c.json), r) !=
                RewardParse::Ok ||
            r.max_spread_bps != c.bps)
            all = false;
    }
    CHECK(all, "fractional cent bands convert exactly to basis points");
}

static void test_no_program() {
    std::printf("no_program\n");
    MarketRewards r{};
    const char* no_rewards =
        R"({"rewardsMaxSpread":4.5,"rewardsMinSize":50,"clobRewards":[]})";
    CHECK(parse_market_rewards(no_rewards, std::strlen(no_rewards), r) ==
              RewardParse::Ok,
          "a market without an active program still parses");
    CHECK(!r.has_program, "but it reports no program");
    CHECK(r.daily_rate_usdc == 0, "and a zero pool");

    // Multiple entries sum.
    const char* two =
        R"({"rewardsMaxSpread":4.5,"rewardsMinSize":50,)"
        R"("clobRewards":[{"rewardsDailyRate":1000},{"rewardsDailyRate":250}]})";
    MarketRewards r2{};
    CHECK(parse_market_rewards(two, std::strlen(two), r2) == RewardParse::Ok &&
              r2.daily_rate_usdc == 1250,
          "multiple reward entries sum");
}

static void test_market_mechanics() {
    std::printf("market_mechanics\n");
    MarketRewards r{};
    const char* m =
        R"({"rewardsMaxSpread":4.5,"rewardsMinSize":50,)"
        R"("orderPriceMinTickSize":0.01,)"
        R"("feeSchedule":{"rate":0.04,"takerOnly":true}})";
    CHECK(parse_market_rewards(m, std::strlen(m), r) == RewardParse::Ok,
          "a market with standard tick parses");
    CHECK(r.tick_size_units == 10000ULL, "0.01 becomes 10000 units");
    CHECK(r.taker_fee_rate == 0.04, "a different fee rate is read verbatim");

    // A tick that does not divide the 1e6 domain must be refused, not rounded
    // into an invalid one.
    MarketRewards r2{};
    const char* bad =
        R"({"rewardsMaxSpread":4.5,"rewardsMinSize":50,)"
        R"("orderPriceMinTickSize":0.0003})";
    CHECK(parse_market_rewards(bad, std::strlen(bad), r2) == RewardParse::Ok,
          "a market with an unusable tick still parses");
    CHECK(r2.tick_size_units == 0ULL, "an unusable tick is reported as absent");

    // No feeSchedule at all: the caller keeps its configured rate.
    MarketRewards r3{};
    const char* no_fee = R"({"rewardsMaxSpread":4.5,"rewardsMinSize":50})";
    CHECK(parse_market_rewards(no_fee, std::strlen(no_fee), r3) ==
              RewardParse::Ok,
          "a market without a fee schedule parses");
    CHECK(r3.taker_fee_rate < 0.0, "an absent fee is negative, never 0.0");
}

static void test_rejections() {
    std::printf("rejections\n");
    MarketRewards r{};
    const char* no_band = R"({"rewardsMinSize":50})";
    CHECK(parse_market_rewards(no_band, std::strlen(no_band), r) ==
              RewardParse::MissingFields,
          "a market without rewardsMaxSpread is reported, never defaulted");

    const char* no_size = R"({"rewardsMaxSpread":4.5})";
    CHECK(parse_market_rewards(no_size, std::strlen(no_size), r) ==
              RewardParse::MissingFields,
          "a missing rewardsMinSize is reported");

    const char* broken = R"({"rewardsMaxSpread":4.5,)";
    CHECK(parse_market_rewards(broken, std::strlen(broken), r) ==
              RewardParse::NotJson,
          "malformed JSON is rejected");

    CHECK(parse_market_rewards(nullptr, 0, r) == RewardParse::NotJson,
          "null payload is rejected");

    const char* absurd = R"({"rewardsMaxSpread":900,"rewardsMinSize":50})";
    CHECK(parse_market_rewards(absurd, std::strlen(absurd), r) ==
              RewardParse::BadNumber,
          "a band above a dollar is rejected as bad data");
}

int main() {
    std::printf("=== test_market_metadata (Motor C) ===\n");
    test_live_shape();
    test_fractional_band();
    test_no_program();
    test_market_mechanics();
    test_rejections();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
