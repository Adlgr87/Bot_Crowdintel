# AUD-RISK Report — OBS-03: daily_buy_volume_ ≠ loss cap

**Date:** 2026-01-22 | **Auditor:** AUD-RISK | **Commit:** c157c23

## Resumen

`daily_buy_volume_` es un **gross BUY volume cap** (gross volume de órdenes BUY), NO un **loss cap** (límite de pérdida). El código lo documenta correctamente en los comentarios, pero el nombre `max_daily_loss_usd` es engañoso.

## Evidencia

### Código: execution_engine.hpp líneas 333-339

```cpp
available_budget = std::min(
    available_budget,
    std::max(0.0, cfg_.max_daily_loss_usd -
        daily_buy_volume_.load(std::memory_order_acquire)));
```

`daily_buy_volume_` acumula el notional de órdenes BUY (gross volume), NO PnL realizado/no realizado.

### Comentario del código (línea 797-800):

```cpp
// BUG FIX (A3): daily buy-volume accumulator replaces the old
// worst_case_loss_usd_.  Only BUY orders (which consume USD) count
// toward this daily gross-volume cap.
```

✅ El código reconoce que es un **gross-volume cap**.

### RiskManager.evaluate() — NO se llama

```cpp
// risk_manager.hpp líneas 144-151:
const int64_t loss_budget = static_cast<int64_t>(limits.max_daily_loss_usd * 1000000.0);
if (day_pnl <= -loss_budget) {
    decision.action = RiskAction::KILL;
}
```

✅ `evaluate()` TIENE un daily loss check real usando PnL.
❌ **PERO `evaluate()` NO SE LLAMA** desde `run_tick()`. El hot path solo llama `risk_->authorize()` que verifica exposure caps y kill switch, NO daily PnL.

### PositionTracker — NO mark-to-market

```cpp
struct Snapshot {
    int64_t realized_pnl = 0;  // Solo REALIZADO (SELL)
    // NO: unrealized_pnl, mark_to_market, daily_pnl
};
```

## Escenario de riesgo

1. Bot ejecuta BUY $50 (daily_buy_volume_ = $50, al límite)
2. Price cae 30%: posición pierde $15 (unrealized PnL)
3. Bot hace SELL: realized_pnl = -$15, daily_buy_volume_ sigue = $50
4. **Bot está "locked" por volumen, no por pérdida**

Si el price sube 30% en lugar de bajar:
1. Bot ejecuta BUY $50
2. Price sube 30%: posición gana $15 (unrealized)
3. Bot hace SELL: realized_pnl = +$15
4. **Bot no se detiene por pérdida** — el cap es de VOLUMEN, no de PÉRDIDA

## Conclusión

- `daily_buy_volume_` funciona como **gross volume cap** ✅ (FIX T1.1 aplicado)
- Pero es **NO es un loss cap** ❌ — no mide PnL real/no realizado
- `RiskManager::evaluate()` tiene el loss check pero no se invoca ❌
- No hay mark-to-market en PositionTracker ❌

## Recomendación

1. Añadir PnL tracker con mark-to-market en PositionTracker
2. Invocar `RiskManager::evaluate()` periódicamente (cada tick o cada N ticks)
3. `daily_buy_volume_` se mantiene como métrica secundaria, no como risk gate
4. Test: bot se detiene al superar max_daily_loss_usd en PnL real
