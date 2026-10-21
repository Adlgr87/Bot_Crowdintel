# VERIFICATION — OBS-03/VER-03: daily_buy_volume_ ≠ loss cap

**Verificador:** AUD-RISK
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** GCC 16.2

---

## Verdicto: VERIFIED_FAIL — daily_buy_volume_ es misnombrado (NO es loss cap), aunque RiskManager::evaluate() provee un daily loss check SEPARADO y DISTINCT

---

## Corrección RECON (IMPORTANTE)

Mi análisis de RECON afirmaba que `RiskManager::evaluate()` existía pero **NO se llamaba** en el hot path. **Esto fue INCORRECTO.** La verificación precisa muestra:

1. **`RiskManager::evaluate()` SÍ se llama** en `housekeeping()` (execution_engine.hpp:650) — se ejecuta **cada tick**.
2. `evaluate()` SÍ calcula **mark-to-market unrealizado PnL** via `unrealized_pnl()` (risk_manager.hpp:117-124).
3. `evaluate()` SÍ tiene un **daily loss check** que puede `KILL` (`day_pnl <= -loss_budget`).
4. `evaluate()` SÍ usa `day_realized_base_` para anclar el PnL del día (risk_manager.hpp:97-115).

### Hallazgo corregido

`daily_buy_volume_` en `execution_engine.hpp` se utiliza como **capital de compra bruto diario**, no como un límite de pérdida real. El problema es el **naming confuso**: la variable `max_daily_loss_usd` se usa con `daily_buy_volume_` (gross volume), NO con PnL.

**Pero** `RiskManager::evaluate()` HAS un **daily loss check REAL** usando mark-to-market PnL. Estos son DOS mecanismos distintos:

| Mechanism | Location | What it measures | Purpose |
|-----------|----------|-----------------|---------|
| daily_buy_volume_ | execution_engine.hpp:333, 394, 467 | Gross BUY volume | Per-order budget cap |
| RiskManager.evaluate() | risk_manager.hpp:146 | day_pnl (realized + unrealized MTM) | Daily loss kill switch |

### Evidence — daily_buy_volume_ (gross volume cap)

**Archivo:** `core/src/execution_engine.hpp`

```cpp
// Línea 333-339: available budget uses daily_buy_volume_, NOT PnL
double available_budget = std::min(
    available_budget,
    std::max(0.0, cfg_.max_daily_loss_usd -   // ← NOMBRE MUY CONFUSO
        daily_buy_volume_.load(std::memory_order_acquire)));  // ← ES VOLUME, NOT LOSS

// Línea 394-395: hard per-order check, same pattern
// Línea 464-467: counter update
// NOTE: daily_buy_volume_ accumulates gross BUY notional
if (side == K_SIDE_BUY) {
    daily_buy_volume_ += accepted_notional;  // ← Only BUY, gross volume
}

// Línea 805-807: day reset
if (daily_buy_volume_day_ != day) {
    daily_buy_volume_ = 0.0;
    daily_buy_volume_day_ = day;
}
```

**Línea 797-800 commentario del código:**

```cpp
// BUG FIX (A3): daily buy-volume accumulator replaces the old
// worst_case_loss_usd_.  Only BUY orders (which consume USD) count
// toward this daily gross-volume cap.
```

### Evidence — RiskManager::evaluate() (daily loss check real, IS called)

**Archivo:** `core/include/risk_manager.hpp` líneas 117-151:

```cpp
static int64_t unrealized_pnl(uint64_t net, uint64_t avg,
                               uint64_t mark_bid) noexcept {
    if (net == 0 || avg == 0) return 0;
    const int64_t diff = static_cast<int64_t>(mark_bid) - static_cast<int64_t>(avg);
    return diff * static_cast<int64_t>(net) / 1000000;  // mark-to-market
}

RiskDecision evaluate(const OrderBookL2::Top& top,
                       const PositionTracker& tracker,
                       uint64_t hedge_bid, uint64_t hedge_ask) noexcept {
    const int64_t realized_today = day_realized(tracker.realized_pnl());  // daily realized
    const int64_t unreal_yes = unrealized_pnl(tracker.net_yes(), tracker.yes_avg(), top.bid.price);
    const int64_t unreal_hedge = unrealized_pnl(tracker.net_hedge(), tracker.hedge_avg(), hedge_bid);
    const int64_t day_pnl = realized_today + unreal_yes + unreal_hedge;
    
    const int64_t loss_budget = static_cast<int64_t>(limits.max_daily_loss_usd * 1000000.0);
    if (loss_budget > 0 && day_pnl <= -loss_budget && !day_kill_latched_) {
        day_kill_latched_ = true;
        kill_.store(true, std::memory_order_release);
        decision.action = RiskAction::KILL;
        return decision;  // ← DAILY LOSS KILL
    }
    // ...
}
```

### Evidence — evaluate() IS called in hot path

**Archivo:** `core/src/execution_engine.hpp` líneas 631-667 (inside `housekeeping()`):

```cpp
// ── The brakes (P2) ────────────────────────────────────────────────
if (risk_ && tracker_) {
    if (risk_->maintain_day_anchor(tracker_->realized_pnl()))
        journal(JournalEvent::Type::DAY_RESET,
                tracker_->realized_pnl(), 0, 0, 0);
    if (!risk_->killed()) {
        OrderBookL2::Top top{};
        // ... read book ...
        const RiskDecision decision = risk_->evaluate(  // ← CALL HERE
            top, *tracker_, hedge_bid, hedge_ask);
        if (decision.action == RiskAction::KILL) {
            // ... set trading_enabled_ = false ...
            journal(JournalEvent::Type::KILL_SWITCH,
                    decision.projected_loss, 0, 0, 0);
            return TickResult::RISK_KILL_SWITCH;  // ← KILL in hot path
        }
    }
}
```

✅ `housekeeping()` se llama al comienzo de `run_tick()` (execution_engine.hpp:248)

### Analysis

1. **¿Qué mide `daily_buy_volume_`?** → Gross BUY notional. **NO mide PnL.** ✅ CONFIRMADO

2. **¿Qué NO mide daily_buy_volume_?** → NO mide:
   - Realized PnL
   - Unrealized PnL (mark-to-market)
   - Net loss del día

3. **RiskManager::evaluate() SÍ provee un daily loss check:**
   - Computes `day_pnl = realized_today + unreal_yes + unreal_hedge`
   - `unreal_yes` y `unreal_hedge` are mark-to-market via `unrealized_pnl()` (best bid for long)
   - KILL when `day_pnl <= -loss_budget`
   - IS called in hot path via `housekeeping()`

4. **Naming problem:** `max_daily_loss_usd` se usa para DOS propósitos distintos:
   - `daily_buy_volume_` vs `max_daily_loss_usd` → gross volume cap (confusing name)
   - `day_pnl` vs `loss_budget` (= `max_daily_loss_usd * 1e6`) → real PnL kill (correct name)

5. **PositionTracker:**
   - Has `realized_pnl` (cumulative) ✅
   - NO `daily_pnl` field in tracker (the daily anchoring is in RiskManager via `day_realized_base_`) ✅
   - `unrealized_pnl` is computed **externalmente** en RiskManager, not in tracker (by design — separation of concerns) ✅

6. **PnL reconciliation against REST:**
   - `housekeeping()` overrides INVENTORY via REST (`override_inventory()` línea 595-597)
   - BUT: NO PnL reconciliation — `TrackerRestate` struct only has shares/avg, NO PnL fields ✅

### Risk scenario (corrected)

**Scenario A: Gross volume exhaustion (daily_buy_volume_)**

1. Bot hace BUY $50 → daily_buy_volume_ = $50 = cap
2. Bot NO puede hacer más BUYs (budget = 0)
3. ✅ Funciona como gross volume cap

**Scenario B: Realized PnL loss (RiskManager::evaluate())**

1. Bot hace BUY $50 → daily_buy_volume_ = $50 (volume cap reached)
2. Price cae 30% → unreal_hedge loss
3. Bot hace SELL $35 → realized_pnl = -$15, daily_buy_volume_ stays $50
4. `housekeeping()` → `evaluate()` → `day_pnl = -15` < `-$50` budget? 
   - day_pnl = -$15, loss_budget = -$50 → NOT killed (|15| < |50|)
5. Bot puede seguir operando si volume cap se resetea (next day) y PnL recovery

**Scenario C: PnL loss exceeds daily loss budget**

1. Bot hace BUY $50 → daily_buy_volume_ = $50
2. Price cae 90% → unrealized PnL = -$45
3. `evaluate()` → day_pnl = -$45 < -$50? No (45 < 50)
4. Price cae more → day_pnl = -$55 < -$50 → KILL 🔴

✅ **RiskManager::evaluate() DOES provide a real daily loss kill switch.**

### PnL reconciliation gap (still valid)

- `TrackerRestate` struct (execution_engine.hpp:95-104) only has: yes_shares, yes_avg, hedge_shares, hedge_avg, drift_exceeded
- NO PnL fields → PnL not reconciled against REST
- This is a REAL gap (OBS-08), separate from daily_buy_volume_

### Conclusión para OBS-03

1. ✅ `daily_buy_volume_` IS a gross volume cap (not a loss cap) — CONFIRMED
2. ✅ RiskManager::evaluate() IS called every tick AND computes mark-to-market PnL
3. ✅ RiskManager::evaluate() DOES have a daily loss KILL check
4. ❌ `max_daily_loss_usd` name is MISLEADING when used with daily_buy_volume_
5. ❌ PnL reconciliation against REST is MISSING (belongs to OBS-08)
6. ⚠️ **Two distinct risk mechanisms** exist: volume cap + PnL kill — naming confusion

**VERDICT:** VERIFIED_FAIL for the naming confusion issue. The daily_buy_volume_ uses `max_daily_loss_usd` but measures volume, not loss. While RiskManager::evaluate() provides a separate PnL-based kill, the confusing naming and architecture risk operator error.

### Criterio de aceptación evaluado

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| daily_buy_volume_ ≠ loss cap | ✅ CONFIRMED | línea 464: "gross BUY notional" |
| max_daily_loss_usd naming confusion | ✅ CONFIRMED | línea 333-339: volume vs loss |
| RiskManager.evaluate() called in hot path | ✅ | línea 650 en housekeeping() |
| evaluate() computes mark-to-market PnL | ✅ | línea 117-124 |
| evaluate() has daily loss KILL | ✅ | línea 146 |
| PnL reconciliation against REST | ❌ | TrackerRestate no PnL fields |

## Recomendación para IMP

1. **Rename** `max_daily_loss_usd` usage in daily_buy_volume_ context → `max_daily_buy_volume_usd` (separate config field)
2. Keep `max_daily_loss_usd` for RiskManager::evaluate() daily PnL kill
3. Add PnL fields to TrackerRestate: `realized_pnl_cents`, `mark_price_e6`
4. Reconcile PnL against REST in `housekeeping()`
5. Test: bot KILLs when day_pnl exceeds max_daily_loss_usd

## Artefactos

- `agents/handoffs/obs-03-daily-buy-volume-ne-loss-cap/artifacts/code_trace.md` (updated)

## Signoff

```json
{
  "card_id": "OBS-03",
  "role": "AUD-RISK",
  "agent": "subagent_nemesis",
  "verdict": "VERIFIED_FAIL",
  "evidence_paths": [
    "agents/handoffs/obs-03-daily-buy-volume-ne-loss-cap/artifacts/code_trace.md"
  ],
  "timestamp": "2026-01-22T12:00:00Z",
  "commit_hash": "c157c23",
  "toolchain": {"gcc": "16.2"},
  "reproducible": true,
  "correction_to_recon": true
}
```
