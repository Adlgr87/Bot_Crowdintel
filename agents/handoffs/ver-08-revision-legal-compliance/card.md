### VER-08: Revisión legal/compliance

- **Origen**: VER-08
- **Severidad**: HIGH
- **Agente verificador asignado**: AUD-SEC
- **Agente implementador asignado**: IMP
- **Dependencias**: `execution_engine.hpp`, `market_config.hpp`, `eip712_signer.hpp`
- **Criterio de aceptación**: checklist compliance firmada; ComplianceGuard con tests de fail-closed; ADR que documente el alcance legal.
- **Riesgo de regresión**: ComplianceGuard fail-closed puede bloquear trades legítimos si config mala.
- **Rollback**: feature flag `BOT_COMPLIANCE_GUARD` (default ON).
- **Deadline (timebox)**: 5 días (HIGH)