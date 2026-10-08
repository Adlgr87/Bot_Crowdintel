# MATH_SPEC.md — Phase 1: Brownian Bridge TWAP Prediction & Volatility

> **Pure deterministic math, zero ML.** All formulas are closed-form.

---

## 1. Problem Statement

Given:
- A 60-second TWAP window (Chainlink-style) with `n` observed price ticks at times `t₁ … tₙ`
- Current price `S_t` (last tick)
- TWAP accumulated so far: `A_t = (1/t) · ∫₀ᵗ S(u) du`
- Strike price `K` (market price at window open)
- Remaining time `τ = T_total − t`

**Goal:** Compute `P(TWAP_final > K)` — the probability that the final TWAP exceeds `K` at window close.

---

## 2. Brownian Bridge Framework

### 2.1 Price Model

Assume price follows a **driftless arithmetic Brownian motion** (valid for short horizons):

```
dS = σ_abs · dW
```

where `σ_abs` is the **absolute** volatility (price-scaled, in $/√s) and `W` is a standard Brownian motion.

### 2.2 TWAP Decomposition

The final TWAP is:

```
TWAP_final = (1/T_total) · [ ∫₀ᵗ S(u)du + ∫ₜᵀ S(u)du ]
           = (t / T_total) · A_t + (τ / T_total) · (1/τ) · ∫ₜᵀ S(u)du
```

So:

```
TWAP_final > K
⟺ (t · A_t + ∫ₜᵀ S(u)du) > K · T_total
```

### 2.3 Future Integral Distribution

Conditional on `S(t) = S_t`, the future integral `I = ∫ₜᵀ S(u)du` is:

```
E[I | S_t] = S_t · τ
Var[I | S_t] = σ_abs² · ∫₀^τ ∫₀^τ min(u,v) du dv = σ_abs² · τ³ / 3
```

This is the **Brownian Bridge variance** `τ³/3`.

### 2.4 The d-Statistic

Define the numerator as the "distance to strike" in price-seconds:

```
N = A_t · t + S_t · τ − K · T_total
```

Then:

```
d = N / (σ_abs · √(τ³/3))
```

And:

```
P(TWAP_final > K) = Φ(d)
```

where `Φ` is the standard normal CDF.

---

## 3. Volatility Conversion

### 3.1 Annualised to Absolute

```
σ_annual    = EWMA of (log_ret² / dt_yr)          [annualised variance]
σ_abs       = σ_annual · S_t / √(SECS_PER_YEAR)     [price-scaled / √s]
```

where:
- `SECS_PER_YEAR = 365 × 24 × 3600 = 31,536,000`
- `√SECS_PER_YEAR ≈ 5615.69`
- `dt_yr = dt_sec / SECS_PER_YEAR`

### 3.2 EWMA Volatility

```
var_rate = λ · var_prev + (1−λ) · (log_ret² / dt_yr)
σ_annual = √(var_rate)
```

**Clamped** to `[vol_min, vol_max] = [0.15, 2.00]` (15%–200% annualised).

---

## 4. Dual-Scale Volatility Estimator

### 4.1 Dual EWMAs

```
λ_fast = 0.94    (1-second returns, fast decay)
λ_slow = 0.98    (1-minute returns, slow decay)
```

### 4.2 Regime Detection

| Regime   | Condition                          | vol_multiplier |
|----------|------------------------------------|----------------|
| NORMAL   | `vol_fast ≈ vol_slow`              | 1.0            |
| SHOCK    | `vol_fast > 2.0 × vol_slow`        | 1.5×           |
| CALM     | `vol_fast < vol_slow / 2.0`        | 0.8×           |

**Note:** With `λ_fast = 0.94`, `λ_slow = 0.98`, the maximum achievable
`vol_fast / vol_slow` ratio from a single return spike is `√3 ≈ 1.73 < 2.0`.
SHOCK and CALM regimes cannot be triggered from returns alone — test setters
(`set_var_fast`, `set_var_slow`) are provided for verification.

### 4.3 Effective Volatility

```
vol_eff = w_fast · √var_fast + w_slow · √var_slow
```
where `w_fast = 0.6`, `w_slow = 0.4`. **Clamped** to `[0.15, 2.00]`.

---

## 5. Deterministic Edge Cases

| Condition              | P value                       |
|------------------------|-------------------------------|
| `τ < 0.5s`             | `1.0` if `A_t > K`, else `0.0` |
| `σ_abs < 1e-12`        | `1.0` if `A_t > K`, else `0.0` |
| `denominator < 1e-15`  | `1.0` if `N > 0`, else `0.0`   |
| Probabilistic (normal) | `Φ(d)`, clamped to `[0.01, 0.99]` |

When `A_t = K` exactly, `P = 0.5` (at-the-money).

---

## 6. Normal CDF Approximation

Using **Abramowitz & Stegun 7.1.26** (erf approximation):

```
Φ(x) = 0.5 · [1 + erf(x/√2)]

erf(z) = 1 − (a₁t + a₂t² + a₃t³ + a₄t⁴ + a₅t⁵) · e^(−z²)
t = 1 / (1 + p·z)
```

Constants:
- `a₁ =  0.254829592`
- `a₂ = -0.284496736`
- `a₃ =  1.421413741`
- `a₄ = -1.453152027`
- `a₅ =  1.061405429`
- `p  =  0.3275911`

**Max error:** `7.5 × 10⁻⁸` (well within the `1e-7` requirement).

---

## 7. Trade Evaluation

### 7.1 Fees

```
taker_fee = 0.072 × price × (1 − price)     [Polymarket CLOB V2]
maker_fee = 0
```

### 7.2 Edge After Fees

```
edge = P_model − market_price − fee_cost
```

### 7.3 Trading Decision

```
should_trade = (edge ≥ 0.005) ∧ (bankroll > 0)
```

Threshold: **0.5%** minimum edge after fees.

### 7.4 Kelly Position Sizing

For a binary bet at price `p` (payout 1.0):

```
odds_ratio b = (1 − p) / p
full_kelly  = (P_model − p) / (1 − p)         [equivalent to (P·b − q)/b with b=(1−p)/p]
quarter     = full_kelly × 0.25
capped      = clamp(quarter, [0, 0.05])         [max 5% of bankroll]
```

---

## 8. Known-Answer Tests

| # | Parameters                                           | Expected P  |
|---|------------------------------------------------------|-------------|
| 1 | `S_t=67500, K=67500, τ=30s, σ=40%`                   | `≈0.50`     |
| 2 | `S_t=67600, K=67500, τ=5s, σ=40%`                    | `>0.95`     |
| 3 | `S_t=67400, K=67600, τ=50s, σ=30%`                   | `<0.15`     |
| 4 | `τ=0, TWAP=67600 > K=67500` (deterministic)          | `1.0`       |
| 5 | `σ=200% (clamped), τ=55s, S_t=K`                     | `0.15–0.85` |
| 6 | `p=0.55, mkt=0.50, taker` → edge=0.032, trade=✓      | —           |
| 7 | `p=0.51, mkt=0.50, taker` → edge=−0.008, trade=✗     | —           |
| 8 | `p=0.52, mkt=0.50, maker` → edge=0.02, trade=✓       | —           |

---

## 9. References

- Brownian Bridge integral formula: standard result for ∫ₜᵀ W(u)du under
  arithmetic Brownian motion (Var = τ³/3).
- Abramowitz, M. & Stegun, I.A. (1964). *Handbook of Mathematical Functions*,
  formula 7.1.26.
- Cont, R., Kearns, M. & Laskoski, M. (2014). "Price dynamics, order flow,
  and pricing errors in markets with mechanisms." *Quantitative Finance*.
- Polymarket CLOB V2 fee schedule: 7.2% taker fee × p(1−p).
