# AUD-RISK Report — OBS-08: No real PnL, tracker conservador

**Date:** 2026-01-22 | **Auditor:** AUD-RISK | **Commit:** c157c23

## Resumen

El PositionTracker solo mantiene PnL realizado (cumulative), sin mark-to-market (unrealized PnL) ni reconciliación de PnL contra REST. El `mmap_state.hpp` define `pnl_cents` pero **nunca se escribe** desde el hot path.

## Evidencia

### PositionTracker — NO mark-to-market

```cpp
struct Snapshot {
    int64_t realized_pnl = 0;  // ✓ Realizado (SELL)
    // ✗ NO unrealized_pnl
    // ✗ NO mark_to_market
    // ✗ NO daily_pnl
};
```

### MMapStateManager — pnl_cents never written

```cpp
// mmap_state.hpp línea 29:
std::atomic<int64_t> pnl_cents;  // Defined but NEVER WRITTEN
```

```bash
$ grep -rn "pnl_cents" core/src/execution_engine.hpp
# Resultado: 0 matches — NEVER WRITTEN
```

### Reconciliación — solo position, no PnL

```cpp
// main_hot_path.cpp líneas 129-173:
void reconcile_once_with(...) {
    // Recolecta: yes_shares, yes_avg, hedge_shares, hedge_avg
    // Compara: diff_shares (position)
    // NO: PnL comparison, NO: mark-to-market
}
```

### TrackerRestate — no PnL fields

```cpp
struct TrackerRestate {
    uint64_t yes_shares;
    uint64_t yes_avg;
    uint64_t hedge_shares;  // NO PnL fields
};
```

## Drift reconciliation

```cpp
const uint64_t max_drift = cfg.reconcile_max_drift_shares * 1000000.0;
// = 0.01 shares × 1e6 = 10,000 micro-shares
// Solo verifica posición, NO PnL
```

## Conclusión

| Component | Position reconciliation | PnL reconciliation |
|-----------|------------------------|--------------------|
| PositionTracker | ✅ net_yes, net_hedge | ❌ No PnL fields |
| RestState struct | ✅ yes_shares | ❌ No PnL fields |
| MMapStateManager | ✅ daily_buy_volume_cents | ❌ pnl_cents never written |
| Reconcile function | ✅ position drift | ❌ No PnL drift |

## Recomendación

1. Añadir `unrealized_pnl` y `daily_pnl` a PositionTracker
2. Añadir PnL fields a TrackerRestate
3. Reconciliar PnL contra REST: `/positions` endpoint should include realized PnL
4. kill_switch on PnL drift > $0.10 or 0.5% notional
