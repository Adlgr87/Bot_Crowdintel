# PnL Gap Analysis — OBS-08

## CORRECCIÓN RECON: Mark-to-market parcialmente existe

**Initial RECON claim:** "No mark-to-market PnL exists"
**Corrected:** `RiskManager::evaluate()` (risk_manager.hpp:117-152) SÍ calcula mark-to-market.

## Current state: Mark-to-market PnL

### Where it lives (outside PositionTracker)

| Component | Location | Purpose |
|-----------|----------|---------|
| `RiskManager::unrealized_pnl()` | risk_manager.hpp:117-124 | Static MTM helper (best bid for long) |
| `RiskManager::evaluate()` | risk_manager.hpp:129-187 | Daily PnL + kill/stop/hedge |
| `RiskManager::day_realized()` | risk_manager.hpp:113-115 | Daily realized from cumulative |
| `RiskManager::maintain_day_anchor()` | risk_manager.hpp:97-111 | UTC midnight reset |
| `RiskManager::day_kill_latched_` | risk_manager.hpp:157 | Kill latch (prevents reset races) |

### Mark-to-market formula (risk_manager.hpp:117-124):

```cpp
static int64_t unrealized_pnl(uint64_t net, uint64_t avg, uint64_t mark_bid) noexcept {
    if (net == 0 || avg == 0) return 0;
    const int64_t diff = static_cast<int64_t>(mark_bid) - static_cast<int64_t>(avg);
    return diff * static_cast<int64_t>(net) / 1000000;  // mark-to-market
}
```

**Mark:** `top.bid.price` (best bid for long positions, mark-to-market conservative)
**Formula:** `(mark_bid - avg_entry) × net_shares / 1e6` = unrealized PnL in USD × 1e6

### Daily PnL (risk_manager.hpp:136-141):

```cpp
const int64_t realized_today = day_realized(tracker.realized_pnl());
// day_realized: tracker_realized - day_realized_base_
// day_realized_base_ anchored at UTC midnight + startup

const int64_t unreal_yes = unrealized_pnl(tracker.net_yes(), tracker.yes_avg(), top.bid.price);
const int64_t unreal_hedge = unrealized_pnl(tracker.net_hedge(), tracker.hedge_avg(), hedge_bid);
const int64_t day_pnl = realized_today + unreal_yes + unreal_hedge;  // FULL MTM DAILY PNL
```

### Daily loss KILL (risk_manager.hpp:146-151):

```cpp
const int64_t loss_budget = static_cast<int64_t>(limits.max_daily_loss_usd * 1000000.0);
if (loss_budget > 0 && day_pnl <= -loss_budget && !day_kill_latched_) {
    day_kill_latched_ = true;
    kill_.store(true, std::memory_order_release);
    decision.action = RiskAction::KILL;
    return decision;
}
```

✅ **Kill on daily loss IS implemented and called in hot path.**

## What's MISSING — PnL Reconciliation vs REST

### PositionTracker (core/include/position_tracker.hpp)

```cpp
struct Snapshot {
    int64_t realized_pnl = 0;  // ✓ Cumulative realized (on SELL)
    // ✗ NO unrealized_pnl field
    // ✗ NO daily_pnl field  
    // ✗ NO mark_to_market field
};
```

The PositionTracker does NOT track mark-to-market. This is computed **externally** in RiskManager. This is a **design separation** (P1 = position tracking, P2 = risk/PnL evaluation), not a bug.

### TrackerRestate (execution_engine.hpp:95-104) — NO PnL fields

```cpp
struct TrackerRestate {
    uint64_t yes_shares = 0;
    uint64_t yes_avg = 0;
    uint64_t hedge_shares = 0;
    uint64_t hedge_avg = 0;
    uint8_t drift_exceeded = 0;
    // ✗ NO: realized_pnl_cents, mark_price, PnL drift check
};
```

### housekeeping() reconciliation (execution_engine.hpp:590-608) — position only

```cpp
tracker_->override_inventory(
    restate.yes_shares, restate.yes_avg,
    restate.hedge_shares, restate.hedge_avg);
if (restate.drift_exceeded && risk_) {
    risk_->latch_kill();  // Position drift kills, but NO PnL drift check
}
```

## PnL drift risk scenario

1. Local PositionTracker: net_yes = 100, yes_avg = 50, realized_pnl = $50
2. REST reports: net_yes = 100, yes_avg = 50, realized_pnl = $30 (missed $20 fill)
3. `housekeeping()` checks shares drift → SHARES MATCH (both 100) → NO kill
4. `RiskManager::evaluate()` uses LOCAL realized_pnl = $50 (stale by $20)
5. **PnL drift of $20 NOT detected → kill switch may not trigger**

## Gap summary

| Feature | Location | Status |
|---------|----------|--------|
| Unrealized PnL (mark-to-market) | risk_manager.hpp:117-124 | ✅ |
| Daily PnL computation | risk_manager.hpp:136-141 | ✅ |
| Daily PnL kill | risk_manager.hpp:146-151 | ✅ |
| Called in hot path | execution_engine.hpp:650 | ✅ |
| PositionTracker MTM | position_tracker.hpp | ❌ (by design — external) |
| PnL reconciliation vs REST | TrackerRestate + housekeeping() | ❌ |
| PnL drift detection | housekeeping() | ❌ |
| Kill on PnL drift | housekeeping() | ❌ |

## Recommendation

1. Add PnL fields to `TrackerRestate`:
   - `int64_t realized_pnl_cents` (from REST /positions)
   - `uint64_t mark_price_yes_e6` (from REST mid)
2. PnL drift check in `housekeeping()`:
   ```cpp
   int64_t local_pnl = tracker_->realized_pnl();
   int64_t pnl_drift = std::abs(local_pnl - restate.realized_pnl_cents * 100);
   if (pnl_drift > cfg_.max_pnl_drift_usd * 1000000.0) {
       risk_->latch_kill();
   }
   ```
3. Test: PnL drift > $0.10 → kill switch triggers
