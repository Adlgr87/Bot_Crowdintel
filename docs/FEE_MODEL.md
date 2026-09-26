# Fee Model Documentation — Phase 4: Commission, Gas & Slippage

## Overview

The `FeeModel` class (`core/include/fee_model.hpp`) implements cost estimation for the Polymarket CLOB V2 trading bot. It computes the total cost of an order — maker/taker fees, dynamic probability-weighted fees, base per-order commission, gas, and slippage — and derives the **net expected value (net_ev)** used as the pre-signing profitability filter in the hot path.

## Integration in ExecutionEngine (Fase 4)

In `core/src/execution_engine.cpp`, the filter replaces the legacy `edge > min_edge` check with a full net-EV calculation:

```cpp
// Fase 4: Net EV Filter (replaces edge > min_edge)
double net_ev = fee_model_.compute_net_ev(
    edge_usd, notional_usd, true,
    probability, spread_bps, available_liquidity
);

if (net_ev < config_.min_net_ev_usd) {
    telemetry_.record_tick_result(TickResult::NOT_PROFITABLE);
    return TickResult::NOT_PROFITABLE;
}
```

**Before:** `if (edge > min_edge)` — only gross edge was considered.

**After:** `net_ev = edge - fees - slippage - gas_cost > min_net_ev` — all execution costs are deducted before the profitability decision, preventing unprofitable orders from reaching the cryptographic signing stage.

---

## Formula

### Dynamic Fee (Probability-Weighted)

```
fee = C × 0.25 × (p · (1 − p))²
```

Where:

| Symbol | Meaning                              | Config Field           | Env Var           | Default |
|--------|--------------------------------------|------------------------|--------------------|---------|
| **C**  | Base commission constant             | `RiskConfig::dynamic_C` | `FEE_DYNAMIC_C`    | `0.075` |
| **p**  | Market probability (0→1)             | `AlphaSignal::q_value`  | *(from signal)*   | —       |
|        | Clamped to [0, 1] at runtime         |                        |                    |         |

**Characteristics:**

- The factor `p · (1 − p)` is the **binary entropy** (Bernoulli variance), maximized at p=0.5 and zero at p=0 and p=1.
- Squaring it `(p·(1−p))²` sharpens the peak, making high-certainty markets (p near 0 or 1) nearly free in dynamic fees.
- The `0.25` multiplier normalizes the peak: at p=0.5, `0.25 × (0.25)² = 0.015625`.
- The dynamic fee scales with notional: `dynamic_fee = C × 0.25 × (p·(1−p))² × notional_usd`.

### Verification Vectors

| Probability (p) | p·(1−p)  | (p·(1−p))² | Dynamic Fee (C=0.075, notional=$100) | Position    |
|----------------|----------|------------|--------------------------------------|-------------|
| 0.0            | 0.0      | 0.0        | $0.000000                             | **Minimum** |
| 0.25           | 0.1875   | 0.03516    | $0.065918                             | Intermediate|
| 0.5            | 0.25     | 0.0625     | $0.117188                             | **Maximum** |
| 0.75           | 0.1875   | 0.03516    | $0.065918                             | Intermediate|
| 1.0            | 0.0      | 0.0        | $0.000000                             | **Minimum** |

### Total Fee (all components)

```
total_fee = base_fee + dynamic_fee + base_commission + gas_cost
```

| Component           | Formula                                      | Description                          |
|---------------------|----------------------------------------------|--------------------------------------|
| **base_fee**        | `notional_usd × fee_rate`                   | Maker (2.0%) or taker (3.5%)       |
| **dynamic_fee**     | `C × 0.25 × (p·(1−p))² × notional_usd`      | Only when `dynamic_fees_enabled`   |
| **base_commission** | `FEE_BASE_USD` (fixed)                       | Per-order flat fee                  |
| **gas_cost**        | `GAS_COST_USD × num_transactions`            | Polygon L2 transaction gas          |

---

## Slippage Estimation

Slippage is estimated from order size, spread, and available L2 liquidity:

```
spread_usd      = order_size_usd × (spread_bps / 10000)
depth_ratio     = order_size_usd / available_liquidity
slippage_factor = 0.5 × depth_ratio²
slippage        = spread_usd × (1 + slippage_factor)
```

**Behavior:**

- **Spread-driven:** Higher spread → higher slippage (linear).
- **Size-driven:** Larger orders relative to depth → higher slippage (quadratic via `depth_ratio²`).
- **Liquidity fallback:** When `available_liquidity ≤ 0`, slippage defaults to 1% of order size.

### Sources of `spread_bps` in ExecutionEngine

```cpp
double spread_bps = (best_bid.price > 0)
    ? static_cast<double>(best_ask.price - best_bid.price) /
      static_cast<double>(best_bid.price) * 10000.0
    : 0.0;
```

Prices are fixed-point (×1e6), so the ratio cancels the scaling.

### Sources of `available_liquidity` in ExecutionEngine

```cpp
double available_liquidity = static_cast<double>(best_ask.size) / 1e6;
```

Size is fixed-point (×1e6), so dividing by 1e6 yields the USD value of the top-of-book depth.

---

## Gas Cost Estimation

```
gas_cost = gas_cost_usd × num_transactions
```

- Default `gas_cost_usd` = $0.005 (Polygon PoS, based on ~70k gas × ~35 gwei).
- Default `num_transactions` = 1 (order submission).
- Full lifecycle (approve + mint + sign + submit + cancel) may involve multiple transactions.

---

## Net Expected Value (net_ev)

```
net_ev = edge_usd − fees − slippage − gas_cost
```

If `net_ev < min_net_ev_usd`, the order is rejected as `TickResult::NOT_PROFITABLE` **before** the EIP-712 signing stage — avoiding wasted cryptographic computation.

### Configuration

| Field                    | Env Var                  | Default | Description                          |
|--------------------------|--------------------------|---------|--------------------------------------|
| `maker_fee_rate`         | `FEE_MAKER_RATE`         | 0.020   | 2.0% maker fee                       |
| `taker_fee_rate`         | `FEE_TAKER_RATE`         | 0.035   | 3.5% taker fee                       |
| `base_commission_usd`    | `FEE_BASE_USD`           | 0.10    | Per-order flat commission            |
| `dynamic_fees_enabled`   | `FEE_DYNAMIC_ENABLED`    | false   | Enable dynamic fee curve (per-market)|
| `dynamic_C`              | `FEE_DYNAMIC_C`          | 0.075   | Base commission constant C           |
| `gas_cost_usd`           | `GAS_COST_USD`           | 0.005   | Per-transaction gas cost             |
| `min_net_ev_usd`         | `FEE_MIN_NET_EV_USD`     | 0.50    | Minimum net EV to execute            |

---

## Implementation Notes

1. **Dynamic fees are opt-in:** The `dynamic_fees_enabled` flag controls whether the dynamic component is applied. Base maker/taker fees are **always** applied regardless of this flag.

2. **Probability source:** The `p` value comes from `AlphaSignal::q_value` (False Discovery Rate), clamped to [0, 1] inside `compute_dynamic_fee()`.

3. **Configurable constants:** All parameters are loaded from environment variables via `RiskConfig::load_from_env()` with conservative defaults. Operators can tune per-environment without recompilation.

4. **Zero-allocation hot path:** `compute_net_ev()`, `compute_fee()`, `compute_dynamic_fee()`, and `estimate_slippage()` are all O(1) with no dynamic allocation — suitable for the ultra-low-latency tick loop.

5. **EIP-712 signer is never modified:** All fee/compliance logic lives in `FeeModel` and `ExecutionEngine`, upstream of the cryptographic signing path in `eip712_signer.hpp`.

## Verification Checklist

- [x] `FeeModel` class with `fee_model.hpp` includes `maker_fee_rate`, `taker_fee_rate`, `base_commission_usd`, `dynamic_fees_enabled` (dynamic_enabled)
- [x] Dynamic fee formula: `fee = C × 0.25 × (p·(1−p))²` is configurable via `dynamic_C`
- [x] Maximum fee at p=0.5 (highest uncertainty), minimum at p=0 or p=1 (highest certainty)
- [x] `GasEstimate`: `gas_cost_usd` and slippage estimated by size and spread
- [x] ExecutionEngine Fase 4: `net_ev = edge - fees - slippage - gas_cost > min_net_ev` replaces `edge > min_edge`
- [x] Order with positive edge but negative net_ev → `NOT_PROFILABLE` returned before signing
- [x] Test suite: `tests/test_fee_model.cpp`
- [ ] Formula verified against Polymarket official documentation (pending final docs review)

## References

- Polymarket CLOB V2 Documentation: https://docs.polymarket.com/
- EIP-712 (Ethereum typed structured data): https://eips.ethereum.org/EIPS/eip-712
- Polygon Gas Station: https://gasstation-matic.polygon.technology/
