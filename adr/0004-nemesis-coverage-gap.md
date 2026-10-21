# ADR-0004: NEMESIS Coverage — Phase 2 Adversarial Audit

| Field | Value |
|-------|-------|
| **Status** | Proposed |
| **Date** | 2026-01-22 |
| **Author** | AUD-SEC |
| **Deciders** | AXIOM, NEMESIS, AUD-RISK |
| **Related cards** | VER-04 |

## Context

The existing NEMESIS phase-1 SECURITY_AUDIT.md (374 líneas) only covers RateLimiter (P3) and WebSocket listener (P1). The 14 P1-P4 modules have NOT been adversarially audited. No second independent pass exists.

## Decision

Conduct a **Phase 2 adversarial audit** focused on the un-audited critical modules.

### Phase 1 coverage (done, in SECURITY_AUDIT.md)

| Module | Coverage |
|--------|----------|
| rate_limiter.hpp | ✅ 4 findings |
| ws_user_listener.hpp | ✅ 1 finding |

### Phase 2 targets (required)

| Module | Priority | Risk | Test target |
|--------|----------|------|-------------|
| **P1 Eyes**: position_tracker.hpp | P0 | CRITICAL | test_position_tracker_fuzz.cpp |
| **P1 Eyes**: account_events.hpp | P1 | HIGH | test_account_events_adversarial.cpp |
| **P2 Brakes**: risk_manager.hpp | P0 | CRITICAL | test_risk_manager_adversarial.cpp |
| **P2 Brakes**: kill_switch | P1 | CRITICAL | test_kill_switch_adversarial.cpp |
| **P3 AdverseSel**: volatility_gate.hpp | P0 | CRITICAL | test_volatility_gate_adversarial.cpp |
| **P3 AdverseSel**: window_shield.hpp | P1 | HIGH | test_window_shield_adversarial.cpp |
| **P3 AdverseSel**: circuit_breaker.hpp | P1 | HIGH | test_circuit_breaker_adversarial.cpp |
| **P4 Brain**: bayesian_engine.hpp | P2 | MEDIUM | test_bayesian_engine_adversarial.cpp |
| **P4 Brain**: kelly_sizer.hpp | P1 | HIGH | test_kelly_adversarial.cpp |
| **Crypto**: eip712_signer.hpp | P0 | CRITICAL | test_signer_adversarial.cpp |
| **Crypto**: keccak256.hpp | P1 | HIGH | test_keccak_adversarial.cpp |

### Methodology

1. **Fuzz testing** — property-based tests with Hypothesis-like frameworks (rapidcheck)
2. **Timing attacks** — test seqlock publication under adversarial timing
3. **Edge cases** — overflow, underflow, NaN, inf, denormal
4. **Replay attacks** — test EIP-712 freshness guarantees

### Phase 2 deliverables

| Deliverable | Due | Owner |
|-------------|-----|-------|
| Coverage matrix | Now | AUD-SEC |
| P1-P4 adversarial test suite | Day 14 (HIGH) / Day 7 (CRITICAL) | SWE/HELIX |
| Phase 2 audit report | Day 14 | NEMESIS |

## Consequences

### Positive
- Full P1-P4 adversarial coverage
- Independent second audit
- Fuzz tests in CI

### Negative
- 14 days of additional work before canary
- May find new critical bugs requiring fixes

## Gate requirement

Canary CANNOT proceed until Phase 2 audit report is signed off by:
- NEMESIS (audit conclusion)
- AXIOM (architect approval of fixes)
- AEGIS (QA validation of adversarial tests)
