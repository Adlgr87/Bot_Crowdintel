### OBS-03/VER-03: daily_buy_volume_ no es un loss cap

- **Origen**: OBS-03 / VER-03
- **Severidad**: CRITICAL
- **Agente verificador asignado**: AUD-RISK
- **Agente implementador asignado**: IMP
- **Dependencias**: `execution_engine.hpp`, `risk_manager.hpp`, `position_tracker.hpp`
- **Criterio de aceptación**: existe un contador `daily_realized_pnl_ + daily_unrealized_pnl_` (con mark-to-market reconciliado vía REST) que alimenta `max_daily_loss_usd`. Test que demuestre que el bot se detiene al superar el cap.
- **Riesgo de regresión**: El daily_buy_volume_ actual funciona como gross volume cap. Cambiar a PnL real puede reducir el límite efectivo.
- **Rollback**: feature flag `BOT_RISK_USE_REAL_PNL` (default OFF en canary).
- **Deadline (timebox)**: 5 días (CRITICAL)