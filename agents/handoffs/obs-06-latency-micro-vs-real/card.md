### OBS-06: Latencia micro vs latencia real

- **Origen**: OBS-06
- **Severidad**: MEDIUM
- **Agente verificador asignado**: AUD-NET
- **Agente implementador asignado**: IMP
- **Dependencias**: `tests/benchmarks/latency_bench.cpp`, `engine_extensions.hpp`
- **Criterio de aceptación**: p99 end-to-end documentada y bajo el SLO; el hot-path <12μs se reporta como métrica secundaria, no como SLO.
- **Riesgo de regresión**: Medir E2E requiere infraestructura de red.
- **Deadline (timebox)**: 10 días (MEDIUM)