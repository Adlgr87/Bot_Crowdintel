# Consolidated Report — OBS-03: Daily Buy Volume ≠ Loss Cap

**Auditor:** AUD-RISK
**Status:** VERIFIED_FAIL → VERIFIED_PASS_AFTER_FIX
**Priority:** CRITICAL (canary H3)

---

## Summary

`daily_buy_volume_` (execution_engine.hpp) was confunded with `max_daily_loss_usd`. The field actually tracks **gross BUY volume**, not PnL-based loss. While RiskManager::evaluate() provides a separate, correct daily PnL kill switch, the confusing naming created operator risk.

---

## Two-Layer Risk Architecture

| Layer | Field | Location | What it measures | Action on breach |
|-------|-------|----------|-----------------|-----------------|
| P2a — Volume cap | max_daily_buy_volume_usd | execution_engine.hpp | Gross BUY notional/day | RISK_REJECTED (order blocked) |
| P2b — PnL kill | max_daily_loss_usd | risk_manager.hpp | Mark-to-market PnL/day | KILL_SWITCH (trading disabled) |

---

## Evidence

### ExecutionEngine daily_buy_volume_ (now renamed)

**Before fix:**
```cpp
// execution_engine.hpp línea 373:
available_budget = std::min(
    available_budget,
    std::max(0.0, cfg_.max_daily_loss_usd - daily_buy_volume_.load(...)));
```

**After fix:**
```cpp
// execution_engine.hpp línea 373 (post-fix):
available_budget = std::min(
    available_budget,
    std::max(0.0, cfg_.max_daily_buy_volume_usd - daily_buy_volume_.load(...)));
```

### RiskManager::evaluate() daily PnL kill (unchanged, correct)

```cpp
// risk_manager.hpp línea 146:
const int64_t loss_budget = static_cast<int64_t>(limits.max_daily_loss_usd * 1000000.0);
if (loss_budget > 0 && day_pnl <= -loss_budget && !day_kill_latched_) {
    kill_.store(true, std::memory_order_release);
    decision.action = RiskAction::KILL;
}
```

### Key corrections to RECON

1. ✅ RiskManager::evaluate() IS called in hot path (housekeeping(), execution_engine.hpp:650)
2. ✅ evaluate() DOES compute mark-to-market PnL (unrealized_pnl(), risk_manager.hpp:117-124)
3. ✅ evaluate() DOES have a daily PnL kill switch (risk_manager.hpp:146)
4. ✅ daily_buy_volume_ IS gross volume (only accumulates BUY orders, línea 464)

---

## Risk scenarios

### Scenario A: Gross volume cap reached
- Bot BUY $50 → daily_buy_volume_ = $50 = max_daily_buy_volume_usd cap
- Bot cannot BUY more (budget = 0)
- ✅ Protected by P2a

### Scenario B: PnL loss exceeds daily loss budget
- Bot BUY $50 → daily_buy_volume_ = $50 (volume OK)
- Price drops 90% → unrealized PnL = -$45
- `evaluate()` → day_pnl = -$45 < -$50? No (|-45| < |-50|)
- Price drops more → day_pnl = -$55 < -$50 → KILL 🔴
- ✅ Protected by P2b

### Scenario C: Both caps reached
- Bot BUY $50 → volume cap reached AND PnL loss accumulates
- New BUY → rejected by P2a volume cap
- If PnL < -budget → KILL by P2b
- ✅ Defense in depth

---

## Fix summary

| Change | File | Status |
|--------|------|--------|
| Added `max_daily_buy_volume_usd` field | market_config.hpp | ✅ |
| Changed volume cap to use new field | execution_engine.hpp | ✅ |
| Preserved `max_daily_loss_usd` for RiskManager | risk_manager.hpp | ✅ (unchanged) |
| Updated comment | execution_engine.hpp | ✅ |

## Test status

- ✅ `test_daily_volume_cap.cpp` — 5 accepted, 10 rejected at $50 cap (unchanged, backward compatible)
- ✅ `test_daily_volume_persist.cpp` — save/load round-trip (unchanged)

---

## Gate impact

| Gate | Before | After |
|------|--------|-------|
| H3 (volume/PnL cap) | BLOCKED | UNBLOCKED |
| H7 (PnL tracking) | BLOCKED | UNBLOCKED |
| GATE-FINAL | BLOCKED | UNBLOCKED |
