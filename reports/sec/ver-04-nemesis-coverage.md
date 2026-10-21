# AUD-SEC Report — VER-04: NEMESIS P1-P6 Coverage Gap

**Date:** 2026-01-22 | **Auditor:** AUD-SEC | **Commit:** c157c23

## Resumen

El SECURITY_AUDIT.md (374 líneas, NEMESIS phase 1) tiene **4 findings** pero **NO cubre los módulos P1-P6**. No hay matriz de cobertura, y los módulos críticos (PositionTracker, RiskManager, VolatilityGate, EIP712Signer) no han sido auditados adversarialmente.

## Evidencia

### Existing audit findings (4 total):

| ID | Module | Severity | Lines in audit |
|----|--------|----------|----------------|
| MEDIUM-1 | rate_limiter.hpp | Medium | ~12 |
| LOW-MED | rate_limiter.hpp | Low-Med | ~8 |
| MEDIUM-3 | ws_user_listener.hpp | Medium | ~15 |
| LOW | rate_limiter.hpp | Low | ~6 |

### P1-P6 coverage matrix:

```
P1 Eyes:     0/2 modules audited   (position_tracker, account_events)
P2 Brakes:   0/2 modules audited   (risk_manager, kill_switch)
P3 AdverseSel: 0/3 modules audited (volatility_gate, window_shield, circuit_breaker)
P3 Network:  2/2 modules audited (rate_limiter, ws_listener)  ← ONLY THIS
P4 Brain:    0/3 modules audited   (bayesian_engine, kelly_sizer, ladder_builder)
Crypto:      0/3 modules audited   (eip712_signer, keccak256, secure_zero)
```

### Critical modules unaudited:

1. **RiskManager.evaluate()** — kill switch logic, daily loss computation
   - Seqlock publication pattern (lines 62-69) — potential torn reads
   - No adversarial test for timing attacks

2. **PositionTracker** — Bloom filter dedup (lines 411-413)
   - False-negative risk under rapid event arrival
   - No adversarial test

3. **EIP712Signer** — nonce determinism, low-S normalization
   - Referenced in SECURITY.md docs only, not adversarially tested

4. **VolatilityGate** — shock cooldown bypass
   - `slippage_ok()` method — no adversarial edge case testing

5. **KellySizer** — fee-inclusive edge computation
   - No adversarial test for fee manipulation via price feeding

## Second audit requirement

The NEMESIS audit was phase 1 only. No phase 2 adversarial pass exists.

## Conclusión

| Criterio | Cumplido | Evidencia |
|----------|---------|-----------|
| Matriz de cobertura P1-P6 | ❌ | No existe |
| 100% módulos críticos auditados | ❌ | Solo 4 findings in rate_limiter + WS |
| Segunda auditoría independiente | ❌ | Solo una pasada |
| Adversarial tests P1-P4 | ❌ | No existen |

## Recomendación

1. ADR-0004: NEMESIS coverage gap
2. New NEMESIS pass: P1 (position_tracker), P2 (risk_manager), P3 (volatility_gate), P4 (bayesian_engine, kelly_sizer)
3. Adversarial tests: test_risk_manager_adversarial.cpp, test_position_tracker_fuzz.cpp
