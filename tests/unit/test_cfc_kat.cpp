// ─────────────────────────────────────────────────────────────────────────────
// test_cfc_kat: Known-Answer Tests for the C++20 CfC inference kernel.
//
// Reads vectors from core/src/cfc_network_test.hpp (auto-generated from
// ml_training/kat_vectors.json).  Each vector feeds the C++ infer() and
// compares against Python reference outputs.
//
// Exit code 0 = all pass.  Zero external deps.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <limits>

#include "../../core/src/cfc_network.hpp"
#include "../../core/src/cfc_network_test.hpp"

// ── Test framework ─────────────────────────────────────────────────────────────
static int g_failures = 0;
static int g_tests = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);         \
               ++g_failures; }                                                \
    } while (0)

static bool approxf(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps * (1.0f + std::fabs(a) + std::fabs(b));
}

static bool approxh(const float* a, const float* b, uint32_t n, float eps = 1e-4f) {
    for (uint32_t i = 0; i < n; i++) {
        if (!approxf(a[i], b[i], eps)) return false;
    }
    return true;
}

// ── Known-Answer Test ──────────────────────────────────────────────────────────
int main() {
    std::printf("=== CfC Known-Answer Tests ===\n");

    // Load KAT weights directly (no external model file needed)
    CfCNetwork net;
    load_kat_weights(net);

    // Verify SHA256 hash matches KAT header
    bool hash_ok = net.verify_hash(CFC_KAT_SHA256);
    CHECK(hash_ok, "Model SHA256 matches KAT header");

    const uint32_t n_vectors = 10;
    int pass_count = 0;

    for (uint32_t i = 0; i < n_vectors; ++i) {
        const CfCKATVector& v = CFC_KAT_VECTORS[i];

        // Setup state with KAT initial hidden state
        CfCState state{};
        std::memcpy(state.h.data(), v.h_init, sizeof(float) * CfCConfig::N_HIDDEN);
        state.last_update_ns = 0;
        state.active = true;

        // Build input from KAT vector
        CfCInput input{};
        std::memcpy(input.features, v.input, sizeof(float) * CfCConfig::D_INPUT);
        input.timestamp_ns = 0;
        // dt comes from KAT vector as v.dt

        // Run inference with dt from KAT
        CfCSignal result = net.infer(state, input, static_cast<uint64_t>(v.dt * 1e9));

        // Compare outputs
        const bool logit_ok = approxf(result.logit, v.y_ref, 1e-4f);
        const bool prob_ok = approxf(result.probability_up, v.p_up_ref, 1e-4f);
        const bool hidden_ok = approxh(state.h.data(), v.h_new_ref,
                                       CfCConfig::N_HIDDEN, 1e-4f);
        const bool nan_ok = !result.nan_guard_triggered;

        std::printf("  Vec[%u]: y=%.6f (exp %.6f, %s)  p=%.6f (exp %.6f, %s)  h=%s  nan=%s\n",
            i, result.logit, v.y_ref, logit_ok ? "OK" : "FAIL",
            result.probability_up, v.p_up_ref, prob_ok ? "OK" : "FAIL",
            hidden_ok ? "OK" : "FAIL",
            nan_ok ? "OK" : "FAIL");

        if (logit_ok && prob_ok && hidden_ok && nan_ok) {
            ++pass_count;
        } else {
            ++g_failures;
        }
    }

    // ── NaN guard test ─────────────────────────────────────────────────────────
    {
        CfCState state{};
        state.h.fill(0.0f);
        state.last_update_ns = 0;
        state.active = true;

        CfCInput input{};
        input.features[0] = std::numeric_limits<float>::quiet_NaN();
        for (uint32_t d = 1; d < CfCConfig::D_INPUT; d++)
            input.features[d] = 0.0f;
        input.timestamp_ns = 0;

        CfCSignal result = net.infer(state, input, 1'000'000'000ULL);
        CHECK(result.nan_guard_triggered,
              "NaN input triggers nan_guard");
        CHECK(approxf(result.probability_up, 0.5f, 1e-3f),
              "NaN input returns neutral 0.5 probability");
    }

    // ── Reset test ─────────────────────────────────────────────────────────────
    {
        CfCState state{};
        state.h.fill(1.0f);
        state.active = true;
        net.reset(state, 0.0f);
        bool all_zero = true;
        for (float h : state.h) {
            if (h != 0.0f) { all_zero = false; break; }
        }
        CHECK(all_zero, "CfC state reset zeroes hidden");
        CHECK(!state.active, "CfC state reset clears active flag");
    }

    // ── Summary ────────────────────────────────────────────────────────────────
    std::printf("\n========================================\n");
    std::printf("KAT: %d/%u vectors match, %d/%d tests passed\n",
        pass_count, n_vectors, g_tests - g_failures, g_tests);
    std::printf("========================================\n");

    return g_failures > 0 ? 1 : 0;
}
