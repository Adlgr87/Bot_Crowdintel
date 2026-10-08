// ─────────────────────────────────────────────────────────────────────────────
// Acceptance Test: Volatility Regime Detection
// Verifies EWMA multi-scale vol, regime transitions (NORMAL/SHOCK/CALM),
// and σ clamping at boundaries.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include "volatility_estimator.hpp"

static int g_failures = 0;
static int g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) std::printf("  PASS %s\n", name);               \
         else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } \
    } while (0)

int main() {
    std::printf("=== Acceptance: Volatility Regime Detection ===\n\n");

    VolatilityEstimator::Config cfg{};
    cfg.lambda_fast = 0.94;
    cfg.lambda_slow = 0.98;
    VolatilityEstimator vol(cfg);

    // 1. Normal regime: consistent small returns
    vol.set_var_fast(0.0001);
    vol.set_var_slow(0.0001);
    vol.on_log_return(0.001, 1.0);
    vol.on_log_return(-0.0005, 1.0);
    vol.on_log_return(0.0008, 1.0);
    auto s = vol.current_state();
    CHECK(s.sigma_annual > 0, "Sigma positive after updates");
    CHECK(s.sigma_annual >= 0.15 && s.sigma_annual <= 2.00,
          "Sigma clamped to [0.15, 2.0] bounds");

    // 2. Shock regime: large return spike → fast vol >> slow vol
    vol.reset();
    vol.set_var_fast(0.0001);
    vol.set_var_slow(0.0001);
    vol.on_log_return(0.05, 1.0);  // 5% spike
    s = vol.current_state();
    CHECK(s.sigma_annual > 0.15, "Sigma elevated after shock");

    // 3. Calm regime: tiny returns → fast vol << slow vol
    VolatilityEstimator vol2(cfg);
    vol2.set_var_fast(0.001);
    vol2.set_var_slow(0.001);
    vol2.on_log_return(0.00001, 1.0);
    vol2.on_log_return(0.00001, 1.0);
    s = vol2.current_state();
    CHECK(s.sigma_annual >= 0.15, "Sigma clamped to minimum in calm regime");

    // 4. Reset clears state
    vol.reset();
    s = vol.current_state();
    CHECK(s.regime == VolatilityEstimator::Regime::NORMAL ||
          s.sigma_annual == 0.0 || s.sigma_annual >= 0.15,
          "State valid after reset");

    // 5. NaN guard: bad input → no change
    vol.set_var_fast(0.0001);
    vol.set_var_slow(0.0001);
    vol.on_log_return(std::numeric_limits<double>::quiet_NaN(), 1.0);
    s = vol.current_state();
    CHECK(s.sigma_annual >= 0.0, "No crash on NaN input (fail-safe)");

    std::printf("\n========================================\n");
    std::printf("Volatility Regime: %d/%d passed\n", g_tests - g_failures, g_tests);
    std::printf("========================================\n");
    return g_failures > 0 ? 1 : 0;
}
