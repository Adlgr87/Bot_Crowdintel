### VER-02: Relación causal/estadística Binance BTC ↔ Polymarket

- **Origen**: VER-02
- **Severidad**: HIGH
- **Agente verificador asignado**: AUD-STAT
- **Agente implementador asignado**: IMP (solo si falla: ADR + propuesta de fuentes adicionales)
- **Dependencias**: `binance_ws_client.hpp`, `ofi_calculator.hpp`, datos históricos Binance + Polymarket
- **Criterio de aceptación**: Granger significativo (p < 0.01) con desfase consistente; o définir estrategia como "correlación no causal" y ajustar sizing.
- **Riesgo de regresión**: Análisis estadístico, no toca código core.
- **Deadline (timebox)**: 7 días (HIGH)