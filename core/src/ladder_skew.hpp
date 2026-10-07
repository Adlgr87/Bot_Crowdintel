// ─────────────────────────────────────────────────────────────────────────────
// ladder_skew.hpp — Phase 4: Dynamic Pre-Signed Ladder Skewing
//
// Adjusts quote positions and sizes based on combined conviction signal.
//
// CONVOLUCIÓN DE CONvicción:
//   conviction = w1·cfc_p + w2·ofi_signal + w3·twap_signal
//   skew = (conviction - 0.5) * 2    // [-1, +1]
//
// SKEW APPLICATION (skew > 0 = bullish):
//   bid_size = base * (1 + skew * 0.7)
//   ask_size = base * (1 - skew * 0.7)
//   bid_offset = -spread/2 * (1 - skew * 0.3)   // closer to mid
//   ask_offset = +spread/2 * (1 + skew * 0.3)   // farther from mid
//
// INVARIANTS:
//   - Hot-path (order construction phase)
//   - < 3μs p50 (16-order pool construction)
//   - Zero heap (pre-signed pool)
//   - Never violate bid/ask ratio limits [0.2, 5.0]
//   - Skewed prices within [mid - max_slippage, mid + max_slippage]
//
// TODO(P4-T3): Integrate with PresignedOrderPool.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <algorithm>

struct LadderSkewConfig {
    float w_cfc = 0.5f;
    float w_ofi = 0.3f;
    float w_twap = 0.2f;

    float skew_intensity = 0.7f;        // how strongly to skew sizes
    float spread_intensity = 0.3f;     // how strongly to skew offsets
    float max_ratio = 5.0f;            // max bid/ask size ratio
    float min_ratio = 0.2f;
    float max_slippage_bps = 5.0f;      // max deviation from mid
};

struct alignas(64) ConvictionInput {
    float cfc_probability;    // [0, 1] from CfCNetwork
    float ofi_signal;         // [0, 1] from normalized OFI
    float twap_signal;        // [0, 1] from TWAP deviation
};

struct alignas(64) LadderQuote {
    int64_t price_cents;      // quote price in cents
    uint64_t size_shares;     // size in shares (quantized)
    bool is_bid;              // true = bid, false = ask
};

struct alignas(64) LadderOutput {
    LadderQuote quotes[16];    // up to 16 quotes (8 bid levels + 8 ask)
    uint32_t n_quotes;
    float conviction;
    float skew;
};

// ── LadderSkewer ─────────────────────────────────────────────────────────────
class LadderSkewer {
public:
    explicit LadderSkewer(const LadderSkewConfig& cfg) : cfg_(cfg) {}

    // Build skewed ladder from market state.
    // mid_price_cents: current market mid in cents
    // spread_cents: current spread in cents
    // base_size_shares: base quote size (before skew)
    // bankroll_cents: for sizing cap
    LadderOutput build(const ConvictionInput& conviction,
                       uint64_t mid_price_cents,
                       uint64_t spread_cents,
                       uint64_t base_size_shares,
                       uint64_t bankroll_cents) const noexcept;

    // Compute combined conviction signal [0, 1]
    static float compute_conviction(const ConvictionInput& input) noexcept {
        float c = input.cfc_probability * 0.5f
                + input.ofi_signal * 0.3f
                + input.twap_signal * 0.2f;
        return std::clamp(c, 0.0f, 1.0f);
    }

private:
    LadderSkewConfig cfg_;

    static float skew_factor(float skew, float intensity) noexcept {
        return 1.0f + skew * intensity;
    }
};
