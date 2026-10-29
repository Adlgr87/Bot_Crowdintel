// ─────────────────────────────────────────────────────────────────────────────
// test_config_hardening: Fase 0 (capital survival) + Fase 1.1 (dynamic fee)
//
// Verifies:
//   1. FeeCategory rate table and its fail-closed Unknown -> 0.07 behaviour
//   2. BOT_MARKET_CATEGORY resolves taker_fee_rate to the real category rate
//   3. BOT_TAKER_FEE_RATE stays the highest-authority override
//   4. BOT_MAX_*_USDC is accepted as an alias of BOT_MAX_*_USD
//   5. A conflicting canonical/alias pair fails closed instead of picking one
//   6. BOT_MAX_DAILY_BUY_VOLUME_USD is actually read (it previously was not)
//   7. BOT_MODE accepts mock|paper|live and rejects anything else
//   8. check_live_invariants rejects every incoherent live limit set
//
// Nothing here needs a network, a signer, or real credentials.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

static bool approx(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

// The env keys this suite touches.  Every case starts from a clean slate so a
// developer's shell cannot make a test pass or fail.
static const char* kEnvKeys[] = {
    "BOT_MODE",
    "BOT_PRIVATE_KEY_HEX",
    "BOT_TOKEN_ID",
    "BOT_MARKET_CATEGORY",
    "BOT_TAKER_FEE_RATE",
    "BOT_BANKROLL_USD",
    "BOT_MAX_BANKROLL_USDC",
    "BOT_MAX_ORDER_USD",
    "BOT_MAX_ORDER_USDC",
    "BOT_MAX_EXPOSURE_USD",
    "BOT_MAX_EXPOSURE_USDC",
    "BOT_MAX_PORTFOLIO_EXPOSURE_USD",
    "BOT_MAX_PORTFOLIO_EXPOSURE_USDC",
    "BOT_MAX_DAILY_LOSS_USD",
    "BOT_MAX_DAILY_LOSS_USDC",
    "BOT_MAX_DAILY_BUY_VOLUME_USD",
    "BOT_MAX_DAILY_BUY_VOLUME_USDC",
};

static void clear_env() {
    for (const char* key : kEnvKeys) ::unsetenv(key);
}

// Deterministic dummy identity so load() reaches the numeric parsing paths.
static const char* kDummyKey =
    "1111111111111111111111111111111111111111111111111111111111111111";

static const char* load_mock(MarketConfig& cfg) {
    ::setenv("BOT_PRIVATE_KEY_HEX", kDummyKey, 1);
    ::setenv("BOT_TOKEN_ID", "12345", 1);
    return cfg.load(false, /*force_mock=*/true);
}

// ── 1. Rate table ────────────────────────────────────────────────────────────

static void test_fee_rate_table() {
    std::printf("fee_rate_table\n");
    CHECK(approx(fee_rate_for(FeeCategory::Crypto), 0.07), "crypto = 0.07");
    CHECK(approx(fee_rate_for(FeeCategory::Sports), 0.05), "sports = 0.05");
    CHECK(approx(fee_rate_for(FeeCategory::Politics), 0.04), "politics = 0.04");
    CHECK(approx(fee_rate_for(FeeCategory::Geopolitics), 0.00),
          "geopolitics = 0.00 (fee-free)");
    // Fail-closed: an unclassified market must never be priced as cheap.
    CHECK(approx(fee_rate_for(FeeCategory::Unknown), 0.07),
          "unknown fails closed to the most expensive rate");
    // Monotonic sanity: geopolitics must be strictly cheaper than crypto.
    CHECK(fee_rate_for(FeeCategory::Geopolitics) <
              fee_rate_for(FeeCategory::Crypto),
          "geopolitics strictly cheaper than crypto");
}

static void test_fee_category_parse() {
    std::printf("fee_category_parse\n");
    CHECK(parse_fee_category("crypto") == FeeCategory::Crypto, "parse crypto");
    CHECK(parse_fee_category("sports") == FeeCategory::Sports, "parse sports");
    CHECK(parse_fee_category("politics") == FeeCategory::Politics,
          "parse politics");
    CHECK(parse_fee_category("geopolitics") == FeeCategory::Geopolitics,
          "parse geopolitics");
    CHECK(parse_fee_category("Crypto") == FeeCategory::Unknown,
          "case-sensitive: 'Crypto' is unknown (fail-closed)");
    CHECK(parse_fee_category("") == FeeCategory::Unknown, "empty -> unknown");
    CHECK(parse_fee_category(nullptr) == FeeCategory::Unknown,
          "null -> unknown");
    CHECK(parse_fee_category("nonsense") == FeeCategory::Unknown,
          "garbage -> unknown");
}

// ── 2. Alias resolution ──────────────────────────────────────────────────────

static void test_env_alias() {
    std::printf("env_alias\n");
    double value = 0.0;

    // Neither set: the caller-provided default survives untouched.
    clear_env();
    value = 1000.0;
    CHECK(MarketConfig::env_d_alias("BOT_BANKROLL_USD", "BOT_MAX_BANKROLL_USDC",
                                    value) &&
              approx(value, 1000.0),
          "neither set keeps the default");

    // Alias only: the documented *_USDC spelling must be honoured.  This is the
    // canary case that previously ran with 20x its intended capital.
    clear_env();
    ::setenv("BOT_MAX_BANKROLL_USDC", "50", 1);
    value = 1000.0;
    CHECK(MarketConfig::env_d_alias("BOT_BANKROLL_USD", "BOT_MAX_BANKROLL_USDC",
                                    value) &&
              approx(value, 50.0),
          "alias only is honoured (50 USDC canary)");

    // Canonical only.
    clear_env();
    ::setenv("BOT_BANKROLL_USD", "2500", 1);
    value = 1000.0;
    CHECK(MarketConfig::env_d_alias("BOT_BANKROLL_USD", "BOT_MAX_BANKROLL_USDC",
                                    value) &&
              approx(value, 2500.0),
          "canonical only is honoured");

    // Both agree: accepted.
    clear_env();
    ::setenv("BOT_BANKROLL_USD", "500", 1);
    ::setenv("BOT_MAX_BANKROLL_USDC", "500", 1);
    value = 1000.0;
    CHECK(MarketConfig::env_d_alias("BOT_BANKROLL_USD", "BOT_MAX_BANKROLL_USDC",
                                    value) &&
              approx(value, 500.0),
          "matching pair is accepted");

    // Both set and different: must FAIL, never pick one silently.
    clear_env();
    ::setenv("BOT_BANKROLL_USD", "10000", 1);
    ::setenv("BOT_MAX_BANKROLL_USDC", "50", 1);
    value = 1000.0;
    CHECK(!MarketConfig::env_d_alias("BOT_BANKROLL_USD", "BOT_MAX_BANKROLL_USDC",
                                     value),
          "conflicting pair fails closed");

    // Non-numeric value fails.
    clear_env();
    ::setenv("BOT_MAX_BANKROLL_USDC", "fifty", 1);
    value = 1000.0;
    CHECK(!MarketConfig::env_d_alias("BOT_BANKROLL_USD", "BOT_MAX_BANKROLL_USDC",
                                     value),
          "non-numeric alias fails closed");
    clear_env();
}

// ── 3. load() integration ────────────────────────────────────────────────────

static void test_load_alias_reaches_config() {
    std::printf("load_alias_reaches_config\n");

    clear_env();
    ::setenv("BOT_MAX_BANKROLL_USDC", "50", 1);
    MarketConfig cfg;
    const char* error = load_mock(cfg);
    CHECK(error == nullptr, "load succeeds with alias only");
    CHECK(approx(cfg.bankroll_usd, 50.0), "bankroll_usd == 50 from alias");

    // The gross BUY cap was previously unreachable: it was never read from the
    // environment, so it stayed at the 50.0 default forever.
    clear_env();
    ::setenv("BOT_MAX_DAILY_BUY_VOLUME_USD", "750", 1);
    MarketConfig cfg2;
    CHECK(load_mock(cfg2) == nullptr, "load succeeds with daily buy volume set");
    CHECK(approx(cfg2.max_daily_buy_volume_usd, 750.0),
          "BOT_MAX_DAILY_BUY_VOLUME_USD is now honoured");

    clear_env();
    ::setenv("BOT_MAX_DAILY_BUY_VOLUME_USDC", "800", 1);
    MarketConfig cfg3;
    CHECK(load_mock(cfg3) == nullptr, "load succeeds with daily buy volume alias");
    CHECK(approx(cfg3.max_daily_buy_volume_usd, 800.0),
          "BOT_MAX_DAILY_BUY_VOLUME_USDC alias is honoured");

    // Conflict must abort the load.
    clear_env();
    ::setenv("BOT_BANKROLL_USD", "10000", 1);
    ::setenv("BOT_MAX_BANKROLL_USDC", "50", 1);
    MarketConfig cfg4;
    CHECK(load_mock(cfg4) != nullptr, "conflicting limits abort load()");
    clear_env();
}

static void test_load_fee_category() {
    std::printf("load_fee_category\n");

    // Geopolitics: the case the whole phase exists for.  The bot used to
    // demand 0.07 of fee in a market that charges nothing.
    clear_env();
    ::setenv("BOT_MARKET_CATEGORY", "geopolitics", 1);
    MarketConfig cfg;
    CHECK(load_mock(cfg) == nullptr, "load succeeds with geopolitics category");
    CHECK(approx(cfg.taker_fee_rate, 0.0), "geopolitics fee rate == 0.00");
    CHECK(cfg.fee_category == FeeCategory::Geopolitics, "category recorded");
    CHECK(cfg.fee_rate_source == FeeRateSource::Category, "source == category");

    // Politics.
    clear_env();
    ::setenv("BOT_MARKET_CATEGORY", "politics", 1);
    MarketConfig cfg2;
    CHECK(load_mock(cfg2) == nullptr, "load succeeds with politics category");
    CHECK(approx(cfg2.taker_fee_rate, 0.04), "politics fee rate == 0.04");

    // Explicit override is the highest authority.
    clear_env();
    ::setenv("BOT_MARKET_CATEGORY", "geopolitics", 1);
    ::setenv("BOT_TAKER_FEE_RATE", "0.02", 1);
    MarketConfig cfg3;
    CHECK(load_mock(cfg3) == nullptr, "load succeeds with category + override");
    CHECK(approx(cfg3.taker_fee_rate, 0.02), "explicit rate wins over category");
    CHECK(cfg3.fee_rate_source == FeeRateSource::Explicit, "source == explicit");

    // No category, no override: historical behaviour preserved (fail-closed).
    clear_env();
    MarketConfig cfg4;
    CHECK(load_mock(cfg4) == nullptr, "load succeeds with no fee config");
    CHECK(approx(cfg4.taker_fee_rate, 0.07),
          "default stays 0.07 (most expensive, fail-closed)");
    CHECK(cfg4.fee_rate_source == FeeRateSource::Default, "source == default");

    // Invalid category is a hard error, not a silent fallback.
    clear_env();
    ::setenv("BOT_MARKET_CATEGORY", "cryptocurrency", 1);
    MarketConfig cfg5;
    CHECK(load_mock(cfg5) != nullptr, "invalid category aborts load()");
    clear_env();
}

static void test_load_run_mode() {
    std::printf("load_run_mode\n");

    clear_env();
    ::setenv("BOT_MODE", "paper", 1);
    MarketConfig cfg;
    CHECK(load_mock(cfg) == nullptr, "load accepts BOT_MODE=paper");
    CHECK(cfg.mock_mode, "paper behaves as mock until a paper engine exists");
    CHECK(cfg.paper_mode_requested, "paper request recorded for the notice");

    clear_env();
    ::setenv("BOT_MODE", "mock", 1);
    MarketConfig cfg2;
    CHECK(load_mock(cfg2) == nullptr, "load accepts BOT_MODE=mock");
    CHECK(cfg2.mock_mode, "mock mode set");
    CHECK(!cfg2.paper_mode_requested, "paper flag clear for mock");

    clear_env();
    ::setenv("BOT_MODE", "semi-live", 1);
    MarketConfig cfg3;
    CHECK(load_mock(cfg3) != nullptr, "load rejects an unknown BOT_MODE");
    clear_env();
}

// ── 4. Live invariants ──────────────────────────────────────────────────────

// Defaults are internally incoherent for live trading (the gross BUY cap is
// below two max-size orders), so the baseline is built explicitly.
// MarketConfig is non-copyable by design, so this fills in place.
static void make_coherent_live(MarketConfig& cfg) {
    cfg.mock_mode = false;
    cfg.bankroll_usd = 10000.0;
    cfg.max_daily_loss_usd = 150.0;         // 1.5%
    cfg.max_order_usd = 300.0;              // 3%
    cfg.max_exposure_usd = 2000.0;          // 20%
    cfg.max_portfolio_exposure_usd = 4000.0;  // 40%
    cfg.max_daily_buy_volume_usd = 3000.0;  // >= 2 * max_order
    cfg.stop_loss_pct = 0.15;
    cfg.taker_fee_rate = 0.07;
    cfg.kelly_fraction = 0.10;
}

static void test_live_invariants() {
    std::printf("live_invariants\n");
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        CHECK(cfg.check_live_invariants() == nullptr,
              "coherent live config passes");
    }
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.bankroll_usd = 0.0;
        CHECK(cfg.check_live_invariants() != nullptr, "zero bankroll rejected");
    }
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.max_daily_loss_usd = 0.0;
        CHECK(cfg.check_live_invariants() != nullptr,
              "daily loss cap of 0 rejected (it disables the kill)");
    }
    {
        // 6% of bankroll: a single bad day could not be absorbed.
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.max_daily_loss_usd = 600.0;
        CHECK(cfg.check_live_invariants() != nullptr,
              "daily loss cap above 5% rejected");
    }
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.max_order_usd = 1500.0;  // 15% of bankroll
        CHECK(cfg.check_live_invariants() != nullptr,
              "max order above 10% rejected");
    }
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.max_exposure_usd = 5000.0;  // 50%
        CHECK(cfg.check_live_invariants() != nullptr,
              "market exposure above 25% rejected");
    }
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.max_portfolio_exposure_usd = 9000.0;  // 90%
        CHECK(cfg.check_live_invariants() != nullptr,
              "portfolio exposure above 50% rejected");
    }
    {
        // The exact defect found in the shipped defaults: the gross BUY cap
        // cannot even fund two orders.
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.max_daily_buy_volume_usd = 100.0;
        CHECK(cfg.check_live_invariants() != nullptr,
              "buy-volume cap below two max orders rejected");
    }
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.stop_loss_pct = 0.0;
        CHECK(cfg.check_live_invariants() != nullptr,
              "disabled stop loss rejected in live");
    }
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.taker_fee_rate = 0.20;
        CHECK(cfg.check_live_invariants() != nullptr,
              "fee rate outside the published range rejected");
    }
    {
        MarketConfig cfg;
        make_coherent_live(cfg);
        cfg.kelly_fraction = 0.60;
        CHECK(cfg.check_live_invariants() != nullptr,
              "kelly above 0.25 rejected in live");
    }
    {
        // Shipped defaults are NOT live-coherent: this is deliberate, it forces
        // an operator to size the deployment instead of inheriting 1000/50.
        MarketConfig cfg;
        cfg.mock_mode = false;
        CHECK(cfg.check_live_invariants() != nullptr,
              "shipped defaults are not live-coherent");
    }
}

int main() {
    std::printf("=== test_config_hardening (Fase 0 + Fase 1.1) ===\n");

    test_fee_rate_table();
    test_fee_category_parse();
    test_env_alias();
    test_load_alias_reaches_config();
    test_load_fee_category();
    test_load_run_mode();
    test_live_invariants();

    clear_env();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
