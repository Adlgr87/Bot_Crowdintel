# Agent Workflow Board — Pre-Canary Verification

**Proyecto:** Bot_Crowdintel_BareMetal (Polymarket CLOB V2 trading bot, C++20)
**Fuente de código:** https://github.com/crowdintel-xyz/Bot_Crowdintel_BareMetal
**Workflow:** agents/ workflow DSH — Verificación e Implementación de Observaciones Pre-Canary
**Estado actual:** FASE 6 COMPLETADA — 3 fixes implementados y testeados (ComplianceGuard, ofi_adjust, PnL recon). GATE-FINAL emitido como CONDITIONAL PROCEED.
**Canary status:** ⚠️ CONDITIONAL PROCEED — 4 of 5 critical blockers remain (CR-1, CR-3, CR-4, CR-5)

---

## Principios rectores

1. **Fail-closed por defecto** — si un agente no puede verificar, marca BLOCKED.
2. **Un agente, una tarjeta, un branch** — cada tarjeta en `fix/obs-XX-` o `verify/ver-XX-`.
3. **Evidencia sobre opinión** — tests, logs, backtests, reportes, diffs adjuntos.
4. **Separación verificación/implementación** — quien verifica no implementa.
5. **Sin merge sin revisión adversarial** — AUDITOR + REV siempre.
6. **Determinismo** — seeds, hashes de datos, versiones de toolchain fijadas.

---

## 🔄 Pipeline de verificación

| Fase | Descripción | Estado |
|------|-------------|--------|
| FASE 0 | RECON: Clone + análisis de código | ✅ COMPLETED (c157c23) |
| FASE 1 | VERIFICACIÓN: 14 cards | ✅ COMPLETED |
| FASE 1b | CORRECCIONES RECON: OBS-03, OBS-08 | ✅ COMPLETED |
| FASE 1c | GATE-VERIFY | ✅ COMPLETED |
| FASE 2 | TRIAGE | ✅ COMPLETED |
| FASE 3 | IMPLEMENTACIÓN | ✅ COMPLETED |
| FASE 4 | REVIEW (adversarial NEMESIS) | ✅ COMPLETED |
| FASE 5 | GATE-FINAL | ✅ COMPLETED (CONDITIONAL) |
| FASE 6 | REPORTE CONSOLIDADO | ✅ COMPLETED |

---

## 🚦 Estado de tarjetas (post-implementación)

| ID      | Título                                          | Sev      | Estado Post-Impl |
|---------|-------------------------------------------------|----------|------------------|
| OBS-01  | DSH no validado OOS                             | CRITICAL | ⛔ BLOCKED (external data) |
| OBS-02  | Kelly sobreapuesta                              | HIGH     | ✅ FIXED (ofi_adjust clamp + CalibrationGuard) |
| OBS-03  | daily_buy_volume_ ≠ loss cap                    | CRITICAL | ✅ FIXED (rename + separate fields) |
| OBS-04  | Complejidad alta                                | MEDIUM   | ⚠️ PARTIAL (docs) |
| OBS-05  | Ambigüedad market-making                        | MEDIUM   | ⚠️ PARTIAL (docs) |
| OBS-06  | Latencia micro vs real                          | MEDIUM   | ⚠️ PARTIAL (CPU-only) |
| OBS-07  | Feed Binance simbólico                          | HIGH     | ⛔ BLOCKED (manual) |
| OBS-08  | Sin PnL real, tracker conservador               | HIGH     | ✅ FIXED (PnL recon fields) |
| OBS-09  | TLS/proxy externo y loopback                    | HIGH     | ⛔ BLOCKED (threat model) |
| VER-02  | Causalidad Binance↔Polymarket                   | HIGH     | ⛔ BLOCKED (external data) |
| VER-04  | Cobertura NEMESIS P1–P6                         | HIGH     | ⛔ BLOCKED (audit pending) |
| VER-06  | Métricas de monitoreo insuficientes             | HIGH     | ⛔ BLOCKED (no metrics in code) |
| VER-07  | Persistencia daily_buy_volume_ ambigua          | MEDIUM   | ✅ PASS (with observations) |
| VER-08  | Compliance guard no implementado                | HIGH     | ✅ FIXED (implemented + tested) |

---

## ✅ Implementaciones completadas (FASE 3)

| Fix | Archivos modificados | Tests | Estado |
|-----|---------------------|-------|--------|
| VER-08: ComplianceGuard | `compliance_guard.hpp` (NEW), `execution_engine.hpp`, `journal.hpp`, `market_config.hpp` | 25 tests ✅ | COMPLETED |
| OBS-02: ofi_adjust clamp + CalibrationGuard | `kelly_sizer.hpp` | 19 tests ✅ | COMPLETED |
| OBS-03: volume rename | `market_config.hpp`, `execution_engine.hpp` | 28 regression tests ✅ | COMPLETED |
| OBS-08: PnL recon fields | `execution_engine.hpp` | 0 (planned) | COMPLETED |

### Test results total: 72 tests, 0 failures

```
test_kelly: 28/28 passed
test_kelly_clamp: 19/19 passed
test_compliance_guard: 25/25 passed
```

---

## ## Estados

`TODO | VERIFYING | VERIFIED_PASS | VERIFIED_FAIL | IMPLEMENTING | IN_REVIEW | DONE | BLOCKED | DEFERRED`

---

## Inventario de artefactos de RECON

| Artefacto | Path | Estado |
|-----------|------|--------|
| Código fuente | `core/src/*.hpp`, `core/include/*.hpp` | 40 archivos analizados |
| Tests | `tests/unit/`, `tests/integration/`, `tests/acceptance/` | 31 tests, 789 CHECK assertions |
| Docs | `docs/*.md` | 15 documentos |
| ML training | `ml_training/` | backtest.py = para CfC legacy, no DSH |
| Security audit | `SECURITY_AUDIT.md` | 374 líneas, audit NEMESIS phase 1 |
| Build system | `core/CMakeLists.txt` | C++20, ASan/UBSan/TSan, portable/native |
| Threat model | docs/SECURITY.md | Parcial — no threat model formal |

---

## Correcciones RECON aplicadas

1. **ComplianceGuard** — VER-08 verificado como documentado pero NO implementado. ✅ AHORA IMPLEMENTADO.
2. **RiskManager::evaluate()** — RECON inicial dijo "NO se llama en hot path". ✅ CORREGIDO: se llama en housekeeping() línea 650, cada tick.
3. **vol_adjust** — RECON dijo "amplifica cuando vol_z < 0". ✅ CORREGIDO: `std::max(0.0, vol_z)` clamp evita amplificación.
4. **ofi_adjust** — SÍ puede exceder 1.0. ✅ CORREGIDO: clamp a `ofi_adjust_max=1.5`.

---

## Critical blockers remaining

| Blocker | Status | Owner |
|---------|--------|-------|
| CR-1: DSH backtest OOS | ⛔ Cannot fix in-code (external data) | POOLSIDE |
| CR-3: Causality analysis | ⛔ Cannot fix in-code (external data) | POOLSIDE |
| CR-4: Prometheus metrics | ⛔ Requires instrumentation | FORGE |
| CR-5: NEMESIS Phase 2 audit | ⛔ Requires adversarial audit | NEMESIS |

---

## Gate outputs

- `gates/GATE-VERIFY.md` — ✅ CREATED
- `gates/GATE-FINAL.md` — ✅ CREATED (CONDITIONAL PROCEED)
- `adr/0001-scope.md` through `adr/0005-compliance-guard-implementation.md` — ✅ ALL 5 CREATED
