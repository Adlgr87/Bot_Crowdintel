# VERIFICATION — OBS-08: Mark-to-market PnL y reconciliación

**Verificador:** AUD-RISK
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** GCC 16.2

---

## Verdicto: VERIFIED_FAIL — Mark-to-market PnL existe parcialmente en RiskManager, pero reconciliación PnL vs REST falta

### Corrección RECON

Mi análisis de RECON afirmaba que "NO hay mark-to-market PnL". **Esto fue INCORRECTO.** La verificación precisa muestra:

- `RiskManager::evaluate()` (risk_manager.hpp:117-152) SÍ calcula mark-to-market realizado y unrealizado PnL
- `evaluate()` SÍ se llama en el hot path (execution_engine.hpp:650)
- `risk_manager.hpp:117-124` tiene `unrealized_pnl()` que hace mark-to-market usando `mark_bid` (best bid para long positions)

### Evidence

**Mark-to-market EXISTS in RiskManager:**

```cpp
// risk_manager.hpp líneas 117-124:
static int64_t unrealized_pnl(uint64_t net, uint64_t avg, uint64_t mark_bid) noexcept {
    if (net == 0 || avg == 0) return 0;
    const int64_t diff = static_cast<int64_t>(mark_bid) - static_cast<int64_t>(avg);
    return diff * static_cast<int64_t>(net) / 1000000;  // mark-to-market
}
```

**Daily PnL computation (risk_manager.hpp:136-141):**
```cpp
const int64_t realized_today = day_realized(tracker.realized_pnl());  // daily realized
const int64_t unreal_yes = unrealized_pnl(tracker.net_yes(), tracker.yes_avg(), top.bid.price);
const int64_t unreal_hedge = unrealized_pnl(tracker.net_hedge(), tracker.hedge_avg(), hedge_bid);
const int64_t day_pnl = realized_today + unreal_yes + unreal_hedge;  // mark-to-market
```

**Daily loss KILL check (risk_manager.hpp:146-151):**
```cpp
const int64_t loss_budget = static_cast<int64_t>(limits.max_daily_loss_usd * 1000000.0);
if (loss_budget > 0 && day_pnl <= -loss_budget && !day_kill_latched_) {
    day_kill_latched_ = true;
    kill_.store(true, std::memory_order_release);
    decision.action = RiskAction::KILL;
    return decision;
}
```

✅ **Mark-to-market PnL IS computed** in RiskManager::evaluate()
✅ **Daily loss kill IS implemented** and IS called in hot path
✅ **Day anchoring** via `day_realized_base_` y `maintain_day_anchor()` (líneas 97-115)

### What's MISSING — PnL Reconciliation vs REST

**TrackerRestate struct (execution_engine.hpp:95-104):**
```cpp
struct TrackerRestate {
    uint64_t yes_shares = 0;     // ✓ Position
    uint64_t yes_avg = 0;       // ✓ VWAP
    uint64_t hedge_shares = 0;  // ✓ Position
    uint64_t hedge_avg = 0;     // ✓ VWAP
    uint8_t drift_exceeded = 0;  // ✓ Position drift flag
    // ✗ NO: realized_pnl, mark_price, PnL reconciliation
};
```

**Reconciliation in housekeeping() (execution_engine.hpp:590-608):**
```cpp
if (restate_q_ && tracker_) {
    TrackerRestate restate{};
    while (restate_q_->try_pop(restate)) {
        tracker_->override_inventory(
            restate.yes_shares, restate.yes_avg, restate.hedge_shares,
            restate.hedge_avg);
        journal(JournalEvent::Type::RECONCILE_DRIFT, 0,
                restate.yes_shares, restate.yes_avg,
                restate.drift_exceeded);
        if (restate.drift_exceeded && risk_) {
            risk_->latch_kill();  // ← Drax position only
        }
    }
}
```

**PositionTracker:**
- Has `realized_pnl` (cumulative) ✅
- NO `unrealized_pnl` field ❌ — computed externally in RiskManager
- NO PnL reconciliation against REST ❌

### PnL drift risk

If the exchange reports a different realized PnL than the local tracker (e.g., due to a missed fill event, or a partial fill miscalculation):

1. Local realized_pnl = $100
2. Exchange reports PnL = $80 (missing $20 fill)
3. **drift NOT detected** — only position shares are reconciled, not PnL
4. RiskManager::evaluate() computes PnL based on STALE local tracker
5. Kill switch may NOT trigger when it should

### RiskManager::evaluate() limitations

| Feature | Implemented? | Notes |
|---------|-------------|-------|
| Unrealized PnL mark-to-market | ✅ | `unrealized_pnl()` static method |
| Daily realized PnL anchoring | ✅ | `day_realized_base_` |
| Daily PnL KILL | ✅ | `day_pnl <= -loss_budget` |
| PnL reconciliation vs REST | ❌ | Only position reaconstriliated |
| PnL drift detection | ❌ | Only `drift_exceeded` for shares |
| Kill on PnL drift | ❌ | Only on position drift |

### Conclusion

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| Mark-to-market PnL | ✅ | risk_manager.hpp:117-124 |
| Daily PnL kill | ✅ | risk_manager.hpp:146, execution_engine.hpp:650 |
| Day anchoring | ✅ | risk_manager.hpp:97-115 |
| PnL reconciliation vs REST | ❌ | TrackerRestate no PnL fields |
| PnL drift detection | ❌ | Only shares drift |
| Kill on PnL drift | ❌ | Only position drift |

## Verdict: VERIFIED_FAIL — Partial. Mark-to-market EXISTS but PnL reconciliation vs REST is MISSING.

The OBS-08 observation title ("without mark-to-market PnL tracking") was partially incorrect. However, the core gap — **PnL reconciliation against REST** — remains valid and is a real risk.

## Recomendación para IMP

1. Add PnL fields to `TrackerRestate`: `realized_pnl_cents`, `mark_price_yes_e6`, `mark_price_hedge_e6`
2. PnL drift check in `housekeeping()`:
   ```cpp
   int64_t pnl_drift = abs(local_pnl - restated_pnl);
   if (pnl_drift > max_pnl_drift) risk_->latch_kill();
   ```
3. Add REST endpoint call to fetch realized PnL from `/positions`
4. Test: PnL drift > threshold triggers kill switch

## Artefactos

- `agents/handoffs/obs-08-sin-PnL-real/artifacts/pnl_gap_analysis.md` (updated)
- `reports/risk/obs-08-no-pnl-recon.md` (updated)

## Signoff

```json
{
  "card_id": "OBS-08",
  "role": "AUD-RISK",
  "agent": "subagent_nemesis",
  "verdict": "VERIFIED_FAIL",
  "evidence_paths": [
    "agents/handoffs/obs-08-sin-PnL-real/artifacts/pnl_gap_analysis.md"
  ],
  "timestamp": "2026-01-22T13:00:00Z",
  "commit_hash": "c157c23",
  "toolchain": {"gcc": "16.2"},
  "reproducible": true,
  "correction_to_recon": true
}
```
