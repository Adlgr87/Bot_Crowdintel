### OBS-04: Complejidad alta para el estado actual

- **Origen**: OBS-04
- **Severidad**: MEDIUM
- **Agente verificador asignado**: AUD-ARCH
- **Agente implementador asignado**: IMP
- **Dependencias**: P1-P6, DSH modules, execution_engine.hpp
- **Criterio de aceptación**: cobertura ≥ 80% líneas y ≥ 70% ramas en módulos de riesgo; matriz de activación con test de integración por combinación crítica.
- **Riesgo de regresión**: Refactoring puede romper hot path.
- **Deadline (timebox)**: 10 días (MEDIUM)