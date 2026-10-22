### OBS-05: Ambigüedad "no market-making" vs LadderBuilder

- **Origen**: OBS-05
- **Severidad**: MEDIUM
- **Agente verificador asignado**: AUD-ARCH + AUD-DOCS
- **Agente implementador asignado**: IMP
- **Dependencias**: `ladder_builder.hpp`, `time_strategy.hpp`, `engine_extensions.hpp`
- **Criterio de aceptación**: definición inequívoca en docs/ARCHITECTURE.md de qué hace el bot (discrete execution) y qué no (quote management). Si hay re-quoting, se documenta como scope real y se revisa por AUD-SEC.
- **Riesgo de regresión**: Docs-only change, no risk.
- **Deadline (timebox)**: 10 días (MEDIUM)