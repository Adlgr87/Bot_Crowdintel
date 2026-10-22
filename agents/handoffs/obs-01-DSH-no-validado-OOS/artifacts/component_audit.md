# Component Audit — DSH OOS Validation Gap

## Summary

The DSH (Deterministic Signal) strategy consists of 7 interconnected modules across P1-P6 layers. **None** have an out-of-sample backtest pipeline.

## Module-by-module calibration status

### P1: TwapBrownianBridge
- **Math:** P(TWAP_final > K) = Φ(d), d = (A_t·t + S_t·τ − K·T) / (σ·√(τ³/3))
- **Params hardcoded:** ewma_lambda=0.94, vol_min=0.15, vol_max=2.00, twap_window_sec=60.0
- **Calibration status:** ❌ Uncalibrated — values chosen by developer intuition
- **Data source:** None (requires Polymarket CLOB V2 historical + Binance BTC)

### P1: VolatilityEstimator
- **Math:** Multi-scale EWMA σ with regime detection (NORMAL/SHOCK/CALM)
- **Params hardcoded:** lambda_fast=0.94, lambda_slow=0.98, shock_threshold=2.0
- **Calibration status:** ❌ Uncalibrated
- **Data source:** None

### P2: OfiLinearFilter
- **Math:** Cont et al. (2014) OFI with z-score thresholds
- **Params hardcoded:** z-score thresholds, EMA decay
- **Calibration status:** ❌ Uncalibrated
- **Data source:** None

### P3: CircuitBreaker + RateLimiter
- **Params hardcoded:** failure_threshold=3, open_duration_ms=5000, etc.
- **Calibration status:** ❌ Not calibrated for real venue behavior

### P4: BayesianEngine
- **Math:** Beta-Binomial conjugate posterior
- **Params hardcoded:** bayes_prior_strength=24.0, bayes_signal_threshold=0.03
- **Calibration status:** ❌ Uncalibrated
- **Data source:** None

### P4: KellySizer
- **Math:** Quarter-Kelly, cap at 5% bankroll
- **Params hardcoded:** kelly_fraction=0.25, max_bankroll_pct=0.05, min_edge=0.005
- **Calibration status:** ❌ Uncalibrated
- **Data source:** None

## Required backtest infrastructure

```
ml_training/backtest_dsh.py  ← CREATE
├── Load Polymarket CLOB V2 historical data (mid, depth, trades)
├── Load Binance BTC/USDT historical data (1m candles)
├── Simulate DSH hot path: BB prob + vol + OFI + Kelly + Ladder
├── Apply fees (0.072) and slippage model
├── Train/val/test split (temporal, no leakage)
├── Baseline comparison: 50/50, market price, moving average
├── Metrics: Brier, log-loss, reliability diagram, ECE
├── Regime calibration: NORMAL/SHOCK/CALM
└── CI: walk-forward with σ from prior to t
```

## Conclusion

**VERDICT:** No statistical validation exists. Canary cannot proceed without backtest.
