### OBS-07/VER-05: Feed Binance simbólico, WS real no probado

- **Origen**: OBS-07 / VER-05
- **Severidad**: HIGH
- **Agente verificador asignado**: AUD-NET
- **Agente implementador asignado**: IMP
- **Dependencias**: `binance_ws_client.hpp`, `binance_spike_detector.hpp`, `multi_feed_manager.hpp`
- **Criterio de aceptación**: uptime ≥ 99.5%, gaps < 1% de mensajes, latencia p99 < umbral en docs/LATENCY_BUDGET.md, reconexión automática sin pérdida de estado crítico.
- **Riesgo de regresión**: Reconexión mejorada puede cambiar timing de ingestión.
- **Rollback**: feature flag `BOT_BINANCE_REAL_FEED` (default OFF = synthetic).
- **Deadline (timebox)**: 7 días (HIGH)