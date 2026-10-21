### OBS-08: Sin PnL real, tracker local conservador

- **Origen**: OBS-08
- **Severidad**: HIGH
- **Agente verificador asignado**: AUD-RISK
- **Agente implementador asignado**: IMP
- **Dependencias**: `position_tracker.hpp`, `lightweight_client.hpp`, `main_hot_path.cpp`
- **Criterio de aceptación**: PnL local vs REST reconciliation con drift < tolerancia definida (p. ej. $0.10 o 0.5% del notional); kill_switch se dispara por drift > umbral.
- **Riesgo de regresión**: Añadir mark-to-market puede cambiar cálculos de exposure.
- **Rollback**: feature flag `BOT_ENABLE_MTM_PNL` (default OFF).
- **Deadline (timebox)**: 7 días (HIGH)