// ─────────────────────────────────────────────────────────────────────────────
// cfc_network.hpp — Closed-Form Continuous-time Neural Network (Hasani et al.)
//
// Native C++20 implementation with SIMD-accelerated inference.
// No external ML frameworks. Hand-rolled SIMD via SSE4.2/AVX2 intrinsics.
//
// MATH (closed-form solution to dh/dt = f(x,h) - h/τ(x)):
//
//   f(x, h) = tanh(W_x·x + W_h·h + b)
//   g(x, h) = σ(W_gx·x + W_gh·h + b_g)
//   τ(x)    = softplus(W_τ·x + b_τ)                         > 0
//   h_new   = h + (f(x,h) - h) · (1 - exp(-Δt / τ(x)))·g(x,h)
//   y       = W_out · h_new + b_out
//   p_up    = σ(y)
//
// INVARIANTS:
//   - Zero heap allocation in inference (stack + pre-loaded weights)
//   - O(N) per inference where N = N_HIDDEN (32)
//   - SIMD-friendly: all arrays aligned to 64 bytes
//   - Fail-closed: NaN → returns 0.5 (neutral signal)
//
// TODO(P2-T1, P2-T2): Implement full training export + SIMD inference kernel.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <immintrin.h>  // SSE4.2, AVX2

// ── Configuration ────────────────────────────────────────────────────────────
struct alignas(64) CfCConfig {
    static constexpr uint32_t N_HIDDEN = 32;
    static constexpr uint32_t D_INPUT = 6;   // OFI, trade_intensity, spread,
                                             // depth_imbalance, microprice, mid_velocity
    static constexpr float DIVERGENCE_THRESHOLD = 0.3f;  // logit vs log-prior
    static constexpr uint32_t MAX_INFERENCE_COUNT = 100'000;
    static constexpr const char* DEFAULT_MODEL_PATH =
        "infra/models/cfc_btc_5m_v1.bin";
};

// ── Model Weights (loaded from Python-exported binary) ───────────────────────
struct alignas(64) CfCWeights {
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::D_INPUT> W_x{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::N_HIDDEN> W_h{};
    std::array<float, CfCConfig::N_HIDDEN> b{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::D_INPUT> W_gx{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::N_HIDDEN> W_gh{};
    std::array<float, CfCConfig::N_HIDDEN> b_g{};
    std::array<float, CfCConfig::D_INPUT> W_tau{};
    float b_tau{};
    std::array<float, CfCConfig::N_HIDDEN> W_out{};
    float b_out{};

    char sha256[65]{0};  // hex hash of model file for verification
};
static_assert(sizeof(CfCWeights) < 16 * 1024, "Weights must fit in L1");

// ── Runtime State (per-market, hot-path) ─────────────────────────────────────
struct alignas(64) CfCState {
    std::array<float, CfCConfig::N_HIDDEN> h{};
    uint64_t last_update_ns = 0;
    float last_output = 0.5f;        // sigmoid output (probability up)
    uint32_t inference_count = 0;
    bool active = false;            // false if NaN guard tripped
};

// ── Market State (consumed by CfC) ───────────────────────────────────────────
struct CfCInput {
    alignas(64) float features[CfCConfig::D_INPUT];
    uint64_t timestamp_ns;
};

// ── Output ───────────────────────────────────────────────────────────────────
struct CfCSignal {
    float probability_up;      // [0, 1] after sigmoid
    float logit;               // raw logit (y)
    bool nan_guard_triggered;  // true if input/output was NaN
    uint64_t inference_cycle;  // monotonicity counter
};

// ── Public API ───────────────────────────────────────────────────────────────
class CfCNetwork {
public:
    CfCNetwork() = default;

    // Load weights from binary file. Returns false on hash mismatch.
    bool load_weights(const char* path) noexcept;

    // Check SHA256 of weights (called after load).
    bool verify_hash(const char* expected_sha256) const noexcept;

    // Main inference. O(N_HIDDEN) with SSE4.2 FMA. No heap.
    // Returns CfCSignal with probability_up, logit, nan_guard flag.
    CfCSignal infer(CfCState& state,
                    const CfCInput& input,
                    uint64_t now_ns) noexcept;

    // Reset hidden state at window boundary.
    void reset(CfCState& state, float prior_logit = 0.0f) noexcept;

    // Current model hash (for logging).
    const char* model_hash() const noexcept { return weights_.sha256; }

    uint32_t n_hidden() const noexcept { return CfCConfig::N_HIDDEN; }
    uint32_t n_input() const noexcept { return CfCConfig::D_INPUT; }

private:
    CfCWeights weights_{};
    float log_prior_odds_ = 0.0f;  // configurable

    // SIMD helpers (P2-T2 implementation):
    static float tanh_approx_sse(float x) noexcept;
    static float exp_approx_sse(float x) noexcept;
    static float sigmoid_approx_sse(float x) noexcept;
    static float softplus_approx_sse(float x) noexcept;
};
