# NEMESIS Coverage Matrix — VER-04

## Security Audit Coverage Mapping

### Existing NEMESIS audit scope

**File: SECURITY_AUDIT.md (374 lines)**

| Finding ID | Severity | Module | File | Function | Audited? |
|-----------|----------|--------|------|----------|----------|
| MEDIUM-1 | Medium | Rate Limiter | rate_limiter.hpp:182 | TokenBucket::refill() | ✅ Sí |
| LOW-MED | Low-Med | Rate Limiter | rate_limiter.hpp:229 | TokenBucket::ms_until_available() | ✅ Sí |
| MEDIUM-3 | Medium | WebSocket | ws_user_listener.hpp:93 | WsUserListener::run_loop() | ✅ Sí |
| LOW | Low | Rate Limiter | rate_limiter.hpp:318 | RateLimiter::on_rate_limited() | ✅ Sí |

### P1-P6 modules NOT audited by NEMESIS

| Module | File | Lines | Risk | Type |
|--------|------|-------|------|------|
| **PositionTracker** | position_tracker.hpp | 431 | CRITICAL | P1 Eyes — inventory, P&L, reservations |
| **AccountEvents** | account_events.hpp | ~150 | HIGH | P1 Eyes — event parsing, dedup |
| **RiskManager** | risk_manager.hpp | 207 | CRITICAL | P2 Brakes — kill switch, daily loss, stop-loss |
| **VolatilityGate** | volatility_gate.hpp | 198 | CRITICAL | P3 Adverse Sel — shock guard, slippage gate |
| **WindowShield** | window_shield.hpp | ~150 | HIGH | P2/P3 — settlement window lifecycle |
| **CircuitBreaker** | circuit_breaker.hpp | 257 | HIGH | P3 — network health gate |
| **BayesianEngine** | bayesian_engine.hpp | 123 | MEDIUM | P4 Brain — probability calculation |
| **KellySizer** | kelly_sizer.hpp | 124 | HIGH | P4 Brain — position sizing |
| **FeeCalculator** | fee_calculator.hpp | 62 | LOW | P4 — fee computation |
| **LadderBuilder** | ladder_builder.hpp | 122 | MEDIUM | P4 — quote construction |
| **EIP712Signer** | eip712_signer.hpp | ~300 | CRITICAL | Crypto — signing, nonce security |
| **Keccak256** | keccak256.hpp | ~200 | CRITICAL | Crypto — hash collision resistance |

### Audit gap summary

| Layer | Modules | Audited | Coverage % |
|-------|---------|---------|------------|
| P1 Eyes | position_tracker, account_events | 0/2 | 0% |
| P2 Brakes | risk_manager, kill_switch | 0/2 | 0% |
| P3 Adverse Sel | volatility_gate, window_shield, circuit_breaker | 0/3 | 0% |
| P3 Network | rate_limiter, ws_user_listener | 2/2 | 100% |
| P4 Brain | bayesian_engine, kelly_sizer, ladder_builder | 0/3 | 0% |
| Crypto | eip712_signer, keccak256, secure_zero | 0/3 | 0% |

### Critical modules needing adversarial audit

1. **RiskManager.evaluate()** — kill switch logic, daily loss computation, seqlock publication
2. **PositionTracker** — Bloom filter dedup, reservation/release, saturation semantics
3. **VolatilityGate** — shock cooldown bypass, slippage gate edge cases
4. **EIP712Signer** — nonce determinism (RFC 6979), low-S normalization, signature malleability

### Independent audit requirement

| Module | First audit | Second audit needed? |
|--------|-------------|---------------------|
| A2 (fee-inclusive notional) | ✅ Applied | ✅ Yes — verify no regression |
| A3 (daily loss cap) | ✅ Applied | ✅ Yes — verify daily_realized_pnl fix |
| M1 (ceil_to_quantum) | ✅ Applied | ✅ Yes — verify with adversarial rounding |
| RiskManager | ❌ | ✅ Urgent |
| PositionTracker | ❌ | ✅ Urgent |
| VolatilityGate | ❌ | ✅ Urgent |
| EIP712Signer | ❌ | ✅ Urgent |
