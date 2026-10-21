# ADR-0005: ComplianceGuard Implementation

**Status:** ACCEPTED
**Date:** 2026-01-22
**Deciders:** AUD-SEC (NEMESIS), AUD-RISK
**Consulted:** AXIOM (architecture review)

## Context

VER-08 verification found that `ComplianceGuard` was **documented** in README.md and ARCHITECTURE_MAP.md as "step 1 of ExecutionEngine::run_tick()" but **did not exist** in the source tree. Only stale build artifacts in `build_*/` directories referenced it.

This created a compliance gap: without a jurisdiction/marker check, the bot could potentially trade restricted markets or accept orders from unauthorized jurisdictions.

## Decision

Implement ComplianceGuard as a **fail-closed** compliance gate integrated into `execute_signal_pipeline()`, operating on:

1. **Jurisdiction allowlist** (ISO 3166-1 alpha-2 country codes)
2. **Restricted condition IDs** (Polymarket market condition IDs)
3. **Restricted market hashes** (alpha_hash_bytes values)

### Key design decisions:

1. **Fail-closed by default:** `compliance_enabled=true` by default. If `operator_jurisdiction` is null/empty when jurisdiction check is enabled → REJECT.

2. **Opt-in at engine level:** The guard is wired via `ComplianceConfig` pointer. If `nullptr`, trading remains unrestricted. This allows canary to start without compliance restrictions if needed, while making it explicit.

3. **Lock-free hot path:** `unordered_set` is constructed at startup; hot path only reads. O(1) lookup, ~20-40ns.

4. **Auditable:** Every rejection is journaled via `JournalEvent::Type::COMPLIANCE_REJECT`.

5. **NOT in housekeeping():** Placed in `execute_signal_pipeline()` AFTER `run_tick()`'s housekeeping() but BEFORE any sizing/signing. This ensures compliance checks don't interfere with risk management (P2), but run before any economic action.

## Alternatives Considered

1. **Compile-time gate (template):** Rejected — reduces flexibility; can't toggle without rebuild.

2. **REST-based dynamic check:** Rejected for hot path; too slow (~ms). Kept as future enhancement.

3. **Signature type validation in ComplianceGuard:** Rejected — signature_type is set during order construction, not signal processing. Already validated in `sign_order()`.

4. **Case-insensitive condition_id matching:** Rejected — Polymarket uses lowercase hex by convention. Added documentation note.

## Consequences

- **Positive:** Compliance violations cannot result in trades. Fail-closed semantics protect against misconfiguration.
- **Positive:** ~40ns overhead on hot path (within p50 < 15μs budget).
- **Negative:** If `operator_jurisdiction` env var is not set, ALL trades are rejected (fail-closed). Documented in validation.
- **Negative:** Case-sensitive condition_id matching (documented; Polymarket convention is lowercase).

## References

- README.md (documents ComplianceGuard as step 2 of hot path)
- docs/ARCHITECTURE_MAP.md (documents ComplianceGuard at step 1)
- docs/CANARY_DEPLOYMENT.md (H12: compliance gate)
- tests/unit/test_compliance_guard.cpp (25 tests)
- core/include/compliance_guard.hpp (implementation)
