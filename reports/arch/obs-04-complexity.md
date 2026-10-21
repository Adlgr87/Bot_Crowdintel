# AUD-ARCH Report — OBS-04: Complejidad alta

**Date:** 2026-01-22 | **Auditor:** AUD-ARCH | **Commit:** c157c23

## Resumen

El hot path acopla 8 módulos de P1-P4. La cobertura de tests en módulos críticos es insuficiente. La configuración de MarketConfig actúa como god-object.

## Evidence

### Hot path coupling

`execution_engine.hpp run_tick()` references **8 modules** simultaneously:

| Module | Layer | Purpose | Coupling type |
|--------|-------|---------|--------------|
| VolatilityGate | P3 | Shock guard | Direct call |
| RiskManager | P2 | Kill switch, exposure | Direct call |
| PositionTracker | P1 | Inventory, reservations | Direct call |
| BayesianEngine | P4 | Probability | Direct call |
| SourceReliability | P4 | Feed weights | Direct call |
| JournalQueue | P1 | Persistence | Direct call |
| PresignedOrderPool | P3 | Order pool | Direct call |
| EvidenceQueue | P4 | Fill evidence | Direct call |

**O(n²) interaction complexity**: Each module can trigger reactions in others.

### MarketConfig — god object

`market_config.hpp` is 671 lines with **25+ configuration fields**. 12+ modules depend on it:
- Risk limits (exposure, daily loss, kill switch path)
- Feed config (URLs, weights, intervals)
- Alpha receiver (bind, port, token)
- CLOB (contracts, chain ID, gas price)
- Logging (log_level, log_path)
- Persistence (state files)

### Test coverage gaps

| Module | Tests exist? | Line coverage |
|--------|-------------|---------------|
| PositionTracker (apply_fill) | ✅ Partial | ~85% |
| RiskManager.evaluate() | ❌ | 0% |
| VolatilityGate.shock_cooldown | ✅ | ~80% |
| BayesianEngine.posterior | ✅ | ~90% |
| KellySizer | ✅ | ~90% |
| OFI full pipeline | ❌ | 0% |
| Evidence reconciliation | ❌ | 0% |

### 2×2 activation matrix (missing)

| Combination | Test exists? |
|-------------|-------------|
| P3 defense OFF + P4 Kelly ON | ❌ |
| P3 defense ON + P4 Kelly OFF | ❌ |
| P2 kill + P3 shock | ❌ |
| Circuit OPEN + vol PAUSE | ❌ |

## Conclusion

| Criterio | Cumplido | Evidencia |
|----------|---------|-----------|
| ≥80% líneas coverage risk modules | ❌ | RiskManager 0% |
| ≥70% ramas risk modules | ❌ | evaluate() no branch tests |
| 2×2 activation matrix | ❌ | 4/4 missing |
| Acoplamiento aceptable | ⚠️ | 8 modules in hot path |

## Recomendación

1. Añadir test_risk_manager.cpp para evaluate()
2. Añadir test de corrupción para archivos
3. Añadir feature flags para P1-P4 modular
4. ADR de configuración modular
