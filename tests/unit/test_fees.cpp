// ─────────────────────────────────────────────────────────────────────────────
// test_fees: the PRODUCTION fee model (Fase 4 rewrite).
//
// This file used to validate `FeeCalculator` in fee_calculator.hpp — a flat-tier
// table that does not match Polymarket and that no production translation unit
// ever included. Production charges:
//
//     fee_per_share = rate * p * (1 - p)
//
// with a per-category `rate` (crypto 0.07, sports 0.05, politics 0.04,
// geopolitics 0.00). The two models disagree by up to 2.4x, so a green suite
// here was actively misleading: it certified a model the bot never runs.
// It now validates `taker_fee_per_share()` and `net_edge_after_fees()` — the
// functions the engine actually calls.
//
// Verifies:
//   1. parabola shape: zero at both ends, maximum at 0.5, symmetric
//   2. the per-category rates and the fail-closed unknown rate
//   3. out-of-range prices are clamped, never extrapolated
//   4. net edge sign convention for BUY and SELL
//   5. the economic claim Fase 1.1 rests on: a geopolitics market needs no fee
//      cover, while crypto needs 1.75c at mid before it can profit
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include "../../core/src/market_config.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
               ++g_failures; }                                                \
    } while (0)

static bool approx(double a, double b, double eps = 1e-12) {
    return std::fabs(a - b) <= eps;
}

// ── 1. Parabola shape ────────────────────────────────────────────────────────

static void test_parabola() {
    std::printf("parabola\n");
    const double rate = 0.07;

    CHECK(approx(taker_fee_per_share(rate, 0.0), 0.0),
          "fee is zero at p=0 (a certain loser costs nothing to buy)");
    CHECK(approx(taker_fee_per_share(rate, 1.0), 0.0), "fee is zero at p=1");
    CHECK(approx(taker_fee_per_share(rate, 0.5), rate * 0.25),
          "fee peaks at p=0.5 with rate/4");
    CHECK(approx(taker_fee_per_share(rate, 0.5), 0.0175),
          "crypto at p=0.5 costs 1.75c per share");

    // Symmetry: the venue does not care which side of 0.5 you are on.
    CHECK(approx(taker_fee_per_share(rate, 0.3),
                 taker_fee_per_share(rate, 0.7)),
          "fee is symmetric around p=0.5");
    CHECK(approx(taker_fee_per_share(rate, 0.1),
                 taker_fee_per_share(rate, 0.9)),
          "fee is symmetric at 0.1/0.9");

    // The tail is a slope, not a plateau: exactly what the old flat-tier model
    // got wrong when it charged a constant 0.63c for everything below 0.1.
    CHECK(taker_fee_per_share(rate, 0.05) < taker_fee_per_share(rate, 0.10),
          "fee keeps falling below p=0.10 (no flat tail)");
    CHECK(approx(taker_fee_per_share(rate, 0.10), 0.0063),
          "crypto at p=0.10 costs 0.63c");
    CHECK(taker_fee_per_share(rate, 0.02) < taker_fee_per_share(rate, 0.05),
          "fee at p=0.02 is below fee at p=0.05");
}

// ── 2. Categories ────────────────────────────────────────────────────────────

static void test_categories() {
    std::printf("categories\n");
    const double p = 0.5;

    CHECK(approx(taker_fee_per_share(fee_rate_for(FeeCategory::Crypto), p),
                 0.0175),
          "crypto at mid = 1.75c");
    CHECK(approx(taker_fee_per_share(fee_rate_for(FeeCategory::Sports), p),
                 0.0125),
          "sports at mid = 1.25c");
    CHECK(approx(taker_fee_per_share(fee_rate_for(FeeCategory::Politics), p),
                 0.0100),
          "politics at mid = 1.00c");
    CHECK(approx(taker_fee_per_share(fee_rate_for(FeeCategory::Geopolitics), p),
                 0.0),
          "geopolitics at mid = 0c");

    // Fail-closed: an unclassified market is priced as the most expensive one,
    // so it can never overstate EV.
    CHECK(approx(fee_rate_for(FeeCategory::Unknown),
                 fee_rate_for(FeeCategory::Crypto)),
          "unknown category falls back to the crypto rate");

    // The ordering must hold at every price, not just at mid.
    bool monotone = true;
    for (double price = 0.05; price < 0.96; price += 0.05) {
        const double geo =
            taker_fee_per_share(fee_rate_for(FeeCategory::Geopolitics), price);
        const double crypto =
            taker_fee_per_share(fee_rate_for(FeeCategory::Crypto), price);
        const double politics =
            taker_fee_per_share(fee_rate_for(FeeCategory::Politics), price);
        if (!(geo == 0.0 && politics > 0.0 && crypto > politics)) monotone = false;
    }
    CHECK(monotone, "geo < politics < crypto fee at every price");
}

// ── 3. Clamping ──────────────────────────────────────────────────────────────

static void test_clamping() {
    std::printf("clamping\n");
    const double rate = 0.07;
    // Out-of-range prices clamp to the probability domain instead of producing
    // a negative or inflated fee through extrapolation.
    CHECK(approx(taker_fee_per_share(rate, -0.5), 0.0),
          "negative price clamps to 0 (fee 0)");
    CHECK(approx(taker_fee_per_share(rate, 1.5), 0.0),
          "price above 1 clamps to 1 (fee 0)");
    CHECK(taker_fee_per_share(rate, 1.2) >= 0.0, "fee is never negative");
}

// ── 4. Net edge ──────────────────────────────────────────────────────────────

static void test_net_edge() {
    std::printf("net_edge\n");
    const double crypto = fee_rate_for(FeeCategory::Crypto);
    const double geo = fee_rate_for(FeeCategory::Geopolitics);

    // BUY: model above the price is good.
    CHECK(approx(net_edge_after_fees(0.60, 0.50, crypto, true), 0.10 - 0.0175),
          "BUY edge = p_win - price - fee");
    // SELL: model below the price is good.
    CHECK(approx(net_edge_after_fees(0.40, 0.50, crypto, false), 0.10 - 0.0175),
          "SELL edge = price - p_win - fee");

    // Exactly at breakeven: p_win = price + fee must yield zero edge.
    const double price = 0.50;
    const double fee = taker_fee_per_share(crypto, price);
    CHECK(approx(net_edge_after_fees(price + fee, price, crypto, true), 0.0),
          "edge is exactly zero at the breakeven win rate");

    // The claim Fase 1.1 rests on, stated as a test.
    CHECK(approx(net_edge_after_fees(0.52, 0.50, geo, true), 0.02),
          "geopolitics collects the full 2c edge with no fee drag");
    CHECK(approx(net_edge_after_fees(0.52, 0.50, crypto, true), 0.02 - 0.0175),
          "crypto at the same price keeps only 0.25c of that edge");

    // A trade that looks profitable gross is a net loss in crypto. This is the
    // entire reason the decision gate has to be fee-aware.
    CHECK(net_edge_after_fees(0.51, 0.50, crypto, true) < 0.0,
          "a 1c gross edge is a net LOSS in crypto at mid");
    CHECK(net_edge_after_fees(0.51, 0.50, geo, true) > 0.0,
          "a 1c gross edge is a net PROFIT in geopolitics");
}

int main() {
    std::printf("=== test_fees (production model) ===\n");

    test_parabola();
    test_categories();
    test_clamping();
    test_net_edge();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
