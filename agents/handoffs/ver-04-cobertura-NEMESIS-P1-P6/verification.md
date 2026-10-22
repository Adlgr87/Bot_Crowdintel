# VERIFICATION — VER-04: Cobertura NEMESIS P1–P6

**Verificador:** AUD-SEC
**Fecha:** 2026-01-22
**Commit base:** c157c23

---

## Verdict: VERIFIED_FAIL — NEMESIS audit coverage is partial, no P1-P6 mapping

### Finding

The observation **CONFIRMS** that the existing `SECURITY_AUDIT.md` (374 líneas, NEMESIS phase 1) does **NOT** map findings to P1-P6 modules. There is no coverage matrix, and critical modules (P1 eyes, P2 brakes, P3 adverse selection, P4 brain) have not been adversarially audited.

### Evidence: NEMESIS audit scope

**Archivo:** `docs/SECURITY_AUDIT.md` — 4 findings:

| ID | Module | Severity | Coverage |
|----|--------|----------|----------|
| MEDIUM-1 | `rate_limiter.hpp` (P3) | Medium | ✅ Parcial |
| LOW-MED | `rate_limiter.hpp` (P3) | Low-Med | ✅ Parcial |
| MEDIUM-3 | `ws_user_listener.hpp` (P1) | Medium | ✅ Parcial |
| LOW | `rate_limiter.hpp` (P3) | Low | ✅ Parcial |

**Total findings: 4** (3 medium, 5 low as stated in executive summary)

**NO findings for:**
- P1: `position_tracker.hpp`, `account_events.hpp`, `reconciliation`
- P2: `risk_manager.hpp`, `kill_switch`
- P3: `volatility_gate.hpp`, `window_shield.hpp`, `circuit_breaker.hpp`
- P4: `bayesian_engine.hpp`, `evidence.hpp`
- Crypto: `eip712_signer.hpp`, `keccak256.hpp`, `secp256k1`
- DSH: `kelly_sizer.hpp`, `ladder_builder.hpp`, `fee_calculator.hpp`

### P1-P6 module audit coverage matrix

| Module | File | NEMESIS audited? | Adversarial author? |
|--------|------|-----------------|---------------------|
| P1 Eyes: PositionTracker | position_tracker.hpp | ❌ | N/A |
| P1 Eyes: AccountEvents | account_events.hpp | ❌ | N/A |
| P2 Brakes: RiskManager | risk_manager.hpp | ❌ | N/A |
| P3 AdverseSel: VolatilityGate | volatility_gate.hpp | ❌ | N/A |
| P3 AdverseSel: WindowShield | window_shield.hpp | ❌ | N/A |
| P3 AdverseSel: CircuitBreaker | circuit_breaker.hpp | ❌ | N/A |
| P4 Brain: BayesianEngine | bayesian_engine.hpp | ❌ | N/A |
| P4 Brain: KellySizer | kelly_sizer.hpp | ❌ | N/A |
| P4 Brain: FeeCalculator | fee_calculator.hpp | ❌ | N/A |
| Crypto: EIP712Signer | eip712_signer.hpp | ⚠️ Referido en SECURITY.md | N/A |
| Crypto: Keccak256 | keccak256.hpp | ❌ | N/A |

### Independent audit requirement

The NEMESIS audit was performed by the same agent team that wrote the code. **No second independent adversarial pass** has been conducted.

### Specific concerns identified

1. **RiskManager.evaluate()** (risk_manager.hpp:132-187): The daily loss kill logic (`day_pnl <= -loss_budget`) has **no adversarial test**. The seqlock publication pattern (líneas 62-69) has not been audited for torn reads under adversarial timing.

2. **PositionTracker** (position_tracker.hpp): The two-epoch Bloom filter for event deduplication (líneas 411-413) has not been adversarially tested for false-negative conditions under rapid event arrival.

3. **EIP-712 signing** (eip712_signer.hpp): Only mentioned in SECURITY.md docs, not adversarially audited. The ERC-7739 type 3 handling (market_config.hpp:295) fails closed but has not been penetration-tested.

4. **Slippage gate** (volatility_gate.hpp:155-164): The `slippage_ok()` method is not adversarially tested for edge cases in price manipulation scenarios.

5. **Fee calculation** (fee_calculator.hpp): `edge_after_fees()` computes `p_model - market_price - fee`. No adversarial test for fee manipulation via price feeding.

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| Matriz de cobertura P1-P6 | ❌ | No existe matriz |
| 100% módulos críticos auditados | ❌ | Solo 4 findings en rate_limiter + WS |
| Segunda auditoría independiente A2/A3/M1 | ❌ | Solo una pasada NEMESIS |
| Adversarial tests for risk manager | ❌ | No tests adversariales |
| Adversarial tests for crypto | ❌ | Solo docs, no tests adversariales |

## Recomendación

1. **ADR**: `adr/0004-nemesis-coverage-gap.md`
2. **New NEMESIS pass**: Focus on P1 (position_tracker), P2 (risk_manager), P3 (volatility_gate), P4 (bayesian_engine, kelly_sizer)
3. **Test adversariales**: test_risk_manager_adversarial.cpp, test_position_tracker_fuzz.cpp

## Artefactos

- `agents/handoffs/ver-04-cobertura-NEMESIS-P1-P6/artifacts/coverage_matrix.md`
- `reports/sec/ver-04-nemesis-coverage.md`

## Signoff