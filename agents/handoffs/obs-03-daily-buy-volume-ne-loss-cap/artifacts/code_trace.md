# Code Trace — OBS-03: daily_buy_volume_ ≠ loss cap

## Annotated code trace

### File: core/src/execution_engine.hpp

#### Línea 333-339: Available budget computation

```cpp
double available_budget = cfg_.max_order_usd;
if (side == K_SIDE_BUY) {
    available_budget = std::min(
        available_budget,
        std::max(0.0, cfg_.max_exposure_usd - exposure_now_usd()));
    //                                          ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    //                                          exposure_now_usd() = tracker_->exposure_worst_cost() * 1e-6
    //                                          Esto es WORST-COST exposure, NO mark-to-market PnL
    
    available_budget = std::min(
        available_budget,
        std::max(0.0, cfg_.max_daily_loss_usd -       // ← NOMBRE CONFUSO
            daily_buy_volume_.load(std::memory_order_acquire)));  // ← PERO ES VOLUME, NO LOSS
}
```

**Key insight:** `cfg_.max_daily_loss_usd` (default $50) se resta de `daily_buy_volume_` (gross BUY volume), NO de realized/unrealized PnL.

#### Línea 392-396: Hard per-order cap check

```cpp
if (order_notional > cfg_.max_order_usd + 1e-9 ||
    (side == K_SIDE_BUY &&
     (exposure_now_usd() + order_notional > cfg_.max_exposure_usd ||
      daily_buy_volume_.load(std::memory_order_acquire) + order_notional > cfg_.max_daily_loss_usd)))
    return TickResult::RISK_REJECTED;
```

**Same issue:** Uses daily_buy_volume_ (gross volume), not PnL.

#### Línea 458-465: Counter update

```cpp
// T1.1 FIX (ID-C02): daily_buy_volume_ MUST be updated regardless of
// tracker attachment. Previously gated by `if (!tracker_)`, when the
// tracker was attached (always, per main_hot_path.cpp), the counter
// never incremented, making max_daily_loss_usd a no-op.
const double accepted_notional = order_notional;
if (side == K_SIDE_BUY) {
    daily_buy_volume_.fetch_add(accepted_notional, std::memory_order_acq_rel);
}
```

✅ FIX T1.1 aplicado — counter works now. But it's still VOLUME, not LOSS.

#### Línea 797-804: Public accessor

```cpp
// BUG FIX (A3): daily buy-volume accumulator replaces the old
// worst_case_loss_usd_.  Only BUY orders (which consume USD) count
// toward this daily gross-volume cap.
double daily_buy_volume() const noexcept { 
    return daily_buy_volume_.load(std::memory_order_acquire); 
}
```

**The code itself admits it:** "Only BUY orders count toward this daily **gross-volume cap**."

### File: core/include/risk_manager.hpp

#### Líneas 32-39: RiskLimits struct

```cpp
struct RiskLimits {
    double stop_loss_pct = 0.15;         // mark drop vs VWAP entry; 0 disables
    double hedge_trigger_pct = 0.0;      // earlier hedge trigger; 0 disables
    double max_daily_loss_usd = 50.0;    // ← Este es el REAL daily loss limit
    double max_market_exposure_usd = 250.0;
    double max_portfolio_exposure_usd = 250.0;
    uint64_t version = 0;
};
```

#### Líneas 132-151: evaluate() — per-tick mark-to-market

```cpp
RiskDecision evaluate(...) noexcept {
    // ...
    const int64_t loss_budget =
        static_cast<int64_t>(limits.max_daily_loss_usd * 1000000.0);
    // day_pnl is computed from realized + unrealized PnL
    if (day_pnl <= -loss_budget) {
        decision.action = RiskAction::KILL;
        // ...
    }
}
```

✅ `RiskManager::evaluate()` HAS a real daily loss check using `day_pnl`.

#### BUT: evaluate() is NOT called in run_tick()

Searching for `evaluate`:
```
$ grep -n "evaluate" execution_engine.hpp
# Resultado: 0 matches
```

`evaluate()` is defined in RiskManager but **NEVER CALLED** from ExecutionEngine::run_tick(). The hot path only calls `risk_->authorize()` (línea 400), which checks exposure caps and kill switch, but does NOT check daily PnL.

### File: core/include/position_tracker.hpp

#### Línea 43: Snapshot struct

```cpp
struct Snapshot {
    uint64_t net_yes = 0;
    uint64_t net_hedge = 0;
    uint64_t yes_avg = 0;
    uint64_t hedge_avg = 0;
    int64_t realized_pnl = 0;      // ← Solo REALIZADO (SELL)
    uint64_t open_buy = 0;
    // ... NO unrealized_pnl, NO mark_to_market
};
```

**NO mark-to-market.** Realized PnL only updates on SELL fills.

#### No daily PnL tracking

```cpp
// PositionTracker has:
// - realized_pnl (cumulative)
// - NO daily_realized_pnl_
// - NO daily_unrealized_pnl_
// - NO mark-to-market del inventario actual
```

### File: core/src/main_hot_path.cpp

#### reconcile_once_with() (líneas 129-173)

```cpp
void reconcile_once_with(LightweightCLOBClient& client,
                         const MarketConfig& cfg, PositionTracker& tracker,
                         SPSC_RingBuffer<TrackerRestate, 16>& restate_q) {
    // ...
    // Reúne: yes, yes_avg, hedge, hedge_avg desde REST /data/positions
    // Compara: diff_shares (posición), NO PnL comparison
    // TrackerRestate solo contiene: yes_shares, yes_avg, hedge_shares, hedge_avg, drift_exceeded
}
```

**NO PnL reconciliation.** Solo position shares reconciliation.

### Conclusión

1. `daily_buy_volume_` = gross BUY notional cap (funciona ✅)
2. `RiskManager::evaluate()` = daily PnL cap (existe ✅, pero NO se llama desde hot path ❌)
3. No mark-to-market en PositionTracker (❌)
4. No PnL reconciliation via REST (❌)

**Risk:** A trader could accumulate large losses through adverse price movement (unrealized PnL) while daily_buy_volume_ stays under cap, because daily_buy_volume_ only counts gross BUY volume, not net loss.
