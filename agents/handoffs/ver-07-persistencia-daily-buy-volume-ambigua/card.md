### VER-07: Ambigüedad persistencia daily_buy_volume_

- **Origen**: VER-07
- **Severidad**: MEDIUM
- **Agente verificador asignado**: AUD-DOCS + AUD-RISK
- **Agente implementador asignado**: IMP
- **Dependencias**: `execution_engine.hpp`, `market_config.hpp`, `tests/unit/test_daily_volume_persist.cpp`
- **Criterio de aceptación**: documentación consistente; test de persistencia + test de corrupción (archivo truncado → load_daily_volume_state() retorna false y arranca en cero).
- **Riesgo de regresión**: No hay código que modificar; solo docs/tests.
- **Deadline (timebox)**: 10 días (MEDIUM)