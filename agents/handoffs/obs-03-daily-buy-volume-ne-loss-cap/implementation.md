# IMPLEMENTATION — OBS-03: daily_buy_volume_ ≠ loss cap

**Implementador:** HERMES
**Fecha:** 2026-01-22
**Branch:** `fix/obs-03-rename-daily-volume` (local changes)
**Status:** ✅ COMPLETADO

---

## Problema

`daily_buy_volume_` se utiliza contra `max_daily_loss_usd`, pero **mide gross BUY volume, NO PnL**. El naming es confuso: el mismo campo (`max_daily_loss_usd`) se usa para dos propósitos distintos:

1. **Volume cap** — `daily_buy_volume_` vs `max_daily_loss_usd` (gross volume)
2. **PnL kill** — `RiskManager::evaluate()`'s `day_pnl` vs `loss_budget` (PnL)

---

## Fix: Rename volume cap field

### `core/src/market_config.hpp`
```cpp
double max_daily_loss_usd = 50.0;        // PnL-based daily loss kill (RiskManager::evaluate)
double max_daily_buy_volume_usd = 50.0;  // OBS-03: gross BUY volume cap (separate from loss)
```

### `core/src/execution_engine.hpp`
**Volume cap context** — now uses `max_daily_buy_volume_usd`:
```cpp
// línea 371-373:
available_budget = std::min(
    available_budget,
    std::max(0.0, cfg_.max_daily_buy_volume_usd - daily_buy_volume_.load(...)));

// línea 431:
daily_buy_volume_.load(...) + order_notional > cfg_.max_daily_buy_volume_usd
```

**PnL kill context** — still uses `max_daily_loss_usd` (RiskManager):
```cpp
// risk_manager.hpp:146:
const int64_t loss_budget = static_cast<int64_t>(limits.max_daily_loss_usd * 1000000.0);
if (loss_budget > 0 && day_pnl <= -loss_budget && !day_kill_latched_) {
    decision.action = RiskAction::KILL;
}
```

---

## Two-layer risk architecture (documented)

| Layer | Field | Location | What it measures |
|-------|-------|----------|-----------------|
| P2a | max_daily_buy_volume_usd | execution_engine.hpp | Gross BUY notional per day |
| P2b | max_daily_loss_usd | risk_manager.hpp | Mark-to-market PnL loss per day |

**Both are enforced:** Volume cap prevents overtrading; PnL kill prevents catastrophic loss.

---

## Validation

No code changes required for tests — `test_daily_volume_cap.cpp` already tests volume tracking:
```
5 accepted, 10 rejected at $50 cap
```
The `max_daily_buy_volume_usd=50.0` defaults maintains backward compatibility.

## Signoff

```json
{
  "card_id": "OBS-03",
  "implementation": "COMPLETED",
  "status": "VERIFIED_PASS_AFTER_FIX",
  "field_renamed": "max_daily_loss_usd → max_daily_buy_volume_usd (volume context)",
  "field_preserved": "max_daily_loss_usd (PnL kill context in RiskManager)"
}
```
