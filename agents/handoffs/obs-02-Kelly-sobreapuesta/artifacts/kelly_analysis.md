# Kelly Sizer Analysis — OBS-02

## File: core/src/kelly_sizer.hpp

## Key code path (lines 52-93):

```cpp
SizingResult compute(double p_model, double market_price, double bankroll_usdc,
                     double inventory, double vol_z_score, bool is_taker,
                     double ofi_size_multiplier) const noexcept
{
    // ... input validation ...
    
    // Fee & edge (inlined)
    const double fee = is_taker ? (FeeCalculator::TAKER_FEE_RATE * market_price * (1.0 - market_price)) : 0.0;
    const double edge = p_model - market_price - fee;
    
    // Full Kelly
    const double odds = (1.0 / market_price) - 1.0;
    const double q = 1.0 - p_model;
    const double kelly_raw = (p_model * odds - q) / odds;
    
    // Quarter-Kelly
    double kelly_fractioned = kelly_raw * config_.kelly_fraction;  // kelly_fraction = 0.25
    
    // Adjustments — applied AFTER fraction
    const double inv_adjust = std::max(0.0, 1.0 - std::abs(inventory));
    const double vol_adjust = 1.0 / (1.0 + std::max(0.0, vol_z_score));
    const double ofi_adjust = std::max(0.0, ofi_size_multiplier);
    
    kelly_fractioned *= inv_adjust * vol_adjust * ofi_adjust;
    
    // Cap to max_bankroll_pct
    const double capped_fraction = std::min(kelly_fractioned, config_.max_bankroll_pct);
    
    // Convert to dollar size
    const double size_usdc = capped_fraction * bankroll_usdc;
    result.size_usdc = size_usdc;
    
    // Convert to shares, cap at max_position (100)
    double size_shares = size_usdc / market_price;
    if (size_shares > config_.max_position) {
        size_shares = config_.max_position;
    }
}
```

## Config struct (lines 21-26):

```cpp
struct Config {
    double kelly_fraction = 0.25;     // Quarter-Kelly (0.25 = 1/4)
    double max_bankroll_pct = 0.05;   // Max 5% of bankroll per trade
    double min_edge = 0.005;          // 0.5% min edge post-fees
    double max_position = 100.0;      // Max 100 shares
};
```

## Issues identified:

### 1. vol_adjust amplification with negative vol_z_score

When `vol_z_score < 0`:
- `std::max(0.0, vol_z_score)` = 0.0
- `vol_adjust = 1.0 / (1.0 + 0.0)` = 1.0
- **No amplification** — the max() clamp prevents it.

**RE-ASSESSMENT:** This is NOT a bug. The `max(0.0, vol_z_score)` prevents amplification.

### 2. ofi_adjust can exceed 1.0

When `ofi_size_multiplier > 1.0`:
- `ofi_adjust = ofi_size_multiplier > 1.0`
- `kelly_fractioned` is multiplied by a value > 1.0
- This can push sizing ABOVE the nominal quarter-Kelly

Example:
- kelly_raw = 0.20, kelly_fraction = 0.25 → fractioned = 0.05
- ofi_adjust = 2.0 → fractioned = 0.10
- capped_fraction = min(0.10, 0.05) = 0.05 → capped ✅

But with kelly_raw = 0.08, kelly_fraction = 0.25 → fractioned = 0.02:
- ofi_adjust = 2.0 → fractioned = 0.04
- capped_fraction = min(0.04, 0.05) = 0.04 → NOT capped, exceeds quarter-Kelly nominal

**Issue:** ofi_adjust can push sizing above the intended quarter-Kelly level when kelly_raw is small.

### 3. No CalibrationGuard

When model ECE > threshold, kelly_fraction is NOT reduced. There's a static `bayes_signal_threshold = 0.03` filter, but no adaptive sizing based on calibration quality.

### 4. Edge case: inventory reduction with high convictions

When `inventory = 0.95`:
- `inv_adjust = max(0.0, 1.0 - 0.95)` = 0.05
- Size reduced to 5% of nominal → very small trades

But if conviction is extremely high (p_model = 0.95), the user might WANT to increase size to close the position. The current logic always REDUCES size near inventory limits, which may be too conservative for closing trades.

## Monte Carlo sensitivity analysis (conceptual)

With artificial model bias of +5%:
- True p = 0.55, model p = 0.60
- At price = 0.50:
  - True edge = 0.55 - 0.50 - 0.018 = 0.032
  - Biased edge = 0.60 - 0.50 - 0.018 = 0.082
  - kelly_raw_true = (0.55*1.0 - 0.45)/1.0 = 0.10
  - kelly_raw_biased = (0.60*1.0 - 0.40)/1.0 = 0.20
  
  Bias doubles kelly_raw. With bankroll = $1000:
  - True size = 0.25 * 0.10 * 1000 = $25
  - Biased size = 0.25 * 0.20 * 1000 = $50
  
  100% over-allocation due to +5% model bias. No guard detects this.

## Summary

| Issue | Severity | Action |
|-------|----------|--------|
| ofi_adjust can exceed 1.0 | MEDIUM | Clamp ofi_adjust to [0.0, 1.5] |
| No CalibrationGuard | HIGH | Add ECE-based kelly_fraction reduction |
| Inventory too conservative for closes | LOW | Separate logic for closing trades |
