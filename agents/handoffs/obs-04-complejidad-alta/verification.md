# VERIFICATION — OBS-04: Complejidad alta para el estado actual

**Verificador:** AUD-ARCH
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** GCC 16.2, gcov/lcov

---

## Verdict: VERIFIED_FAIL — Cobertura < 80% en módulos críticos, coupling alto

### Finding

The observation **CONFIRMS** high complexity: the codebase has 14 P1-P4 modules in `engine_extensions.hpp` with tight coupling, and the test coverage in critical risk modules is below the 80% threshold.

### Evidence: Module dependency map

**Archivo:** `core/src/engine_extensions.hpp`

The `ExtendedEngineLayers` struct (líneas 53-85) has 14 module pointers:

| Phase | Module | File | Lines | Coupling |
|-------|--------|------|-------|----------|
| P1 | TwapBrownianBridge | twap_brownian_bridge.hpp | 337 | Medium (depends on vol) |
| P1 | VolatilityEstimator | volatility_estimator.hpp | 143 | Low |
| P2 | OfiLinearFilter | ofi_linear_filter.hpp | ~200 | Medium (depends on book) |
| P2 | WindowShield | window_shield.hpp | ~150 | Medium (depends on vol) |
| P3 | RateLimiter | rate_limiter.hpp | ~200 | Low |
| P3 | CircuitBreaker | circuit_breaker.hpp | 257 | Low |
| P3 | RequestPrioritizer | request_prioritizer.hpp | ~100 | Low |
| P4 | FeeCalculator | fee_calculator.hpp | 62 | Low (stateless) |
| P4 | KellySizer | kelly_sizer.hpp | 124 | Low (stateless) |
| P4 | LadderBuilder | ladder_builder.hpp | 122 | Low |
| P4 | TimeStrategy | time_strategy.hpp | 59 | Low |

**Hot path coupling:** `execution_engine.hpp` `run_tick()` (líneas 174-489) references:
- `volatility_` (VolatilityGate — P3)
- `risk_` (RiskManager — P2)
- `tracker_` (PositionTracker — P1)
- `bayes_` (BayesianEngine — P4)
- `sources_` (SourceReliability — P4)
- `journal_q_` (Journal — P1)
- `pool_` (PresignedOrderPool — P3)
- `evidence_q_` (Evidence — P4)

Total: **8 modules coupled in the hot path**, creating O(n²) interaction complexity.

### Evidence: Test coverage

**Tests exist for:**

| Module | Test file | Tests | Line coverage (est) |
|--------|-----------|-------|---------------------|
| TwapBrownianBridge | test_twap_bb.cpp | 10 | ~85% |
| VolatilityEstimator | test_vol_estimator.cpp | 11 | ~80% |
| OfiLinearFilter | test_ofi_filter.cpp | 8 | ~75% |
| CircuitBreaker | test_circuit_breaker.cpp | 8 | ~85% |
| KellySizer | test_kelly.cpp | 7 | ~90% |
| LadderBuilder | test_ladder.cpp | 6 | ~90% |
| FeeCalculator | test_fees.cpp | 4 | ~95% |
| RateLimiter | test_rate_limiter.cpp | 6 | ~85% |

**Tests MISSING for critical modules:**

| Module | Status |
|--------|--------|
| RiskManager (`risk_manager.hpp`) | ❌ NO unit tests for `evaluate()` PnL kill logic |
| WindowShield (`window_shield.hpp`) | ⚠️ test_window_shield.cpp exists but no integration with hot path |
| OFI full pipeline | ❌ test_ofi.cpp tests calculator, not hot-path integration |
| Evidence/TrackerRestate | ❌ No reconciliation tests |
| PnL tracking | ❌ No tests for realized vs restated PnL |

### 2×2 activation matrix (missing combinations)

| Test combination | Exists? |
|-----------------|---------|
| P3 defense OFF + P4 Kelly ON | ❌ |
| P3 defense ON + P4 Kelly OFF | ❌ |
| P2 risk kill + P3 shock | ❌ |
| Circuit OPEN + volatility PAUSE | ❌ |

### Coupling analysis

**Fan-in/fan-out:**
- `execution_engine.hpp` fan-out: 8 modules → high coupling
- `market_config.hpp` fan-in: 12+ modules depend on it → god object smell
- `position_tracker.hpp` fan-out: 3 (order_book, polymarket_order, time_utils) → acceptable

**Cycles:**
- `engine_extensions.hpp` → `execution_engine.hpp` → `engine_extensions.hpp` (circular include via ExtendedEngineLayers)
- Documented as "Standalone struct (not inheriting EngineLayers to avoid circular include)" — but the runtime coupling remains

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| ≥80% líneas coverage en risk modules | ❌ | RiskManager.evaluate() no tested |
| ≥70% ramas en risk modules | ❌ | No branch tests for kill logic |
| Matriz 2×2 de activación | ❌ | Missing 4/4 critical combinations |
| Análisis de acoplamiento | ⚠️ | 8 modules in hot path, god-object config |
| Integration tests critical combos | ⚠️ | test_dsh_pipeline.cpp exists but partial |

## Recomendación para IMP

1. **Tests faltantes**: Añadir test_risk_manager.cpp para evaluate() PnL kill logic
2. **Feature flags**: Añadir feature flags para activar/desactivar P1-P4 por módulo
3. **Matriz 2×2**: Test de integración para combinaciones críticas
4. **Desacoplar config**: Extraer MarketConfig subset por módulo (ADR)

## Artefactos

- `agents/handoffs/obs-04-complejidad-alta/artifacts/dependency_matrix.md`
- `reports/arch/obs-04-complexity.md`

## Signoff