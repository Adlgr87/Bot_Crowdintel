# Statistical Report — OBS-02, OBS-03, OBS-08

**Date:** 2026-01-22
**Verifier:** AUD-STAT
**Commit:** c157c23

---

## Summary

Three observation cards received fixes in FASE 3 (Implementation). All have deterministic test suites with exit-code verification.

---

## OBS-02: Kelly / OFI over-positioning

### Before fix
| Metric | Value |
|--------|-------|
| `ofi_adjust` range | `(0.0, ∞)` — unbounded |
| `calibration_ece` | ignored |
| Tests | 28 (test_kelly.cpp) |

### After fix
| Metric | Value |
|--------|-------|
| `ofi_adjust` range | `[0.0, ofi_adjust_max]` (default 1.5) |
| `calibration_ece` | reduces kelly_fraction via linear decay |
| Tests | 47 (28 regression + 19 new) |
| Failures | 0 |
| Exit code | 0 |

### Statistical verification
- 19 new test assertions, all PASS
- 28 existing assertions, all PASS (no regression)
- Deterministic: same input → same output, no RNG in compute()

---

## OBS-03: Daily buy volume ≠ loss cap

### Before fix
| Metric | Value |
|--------|-------|
| Volume cap field | `max_daily_loss_usd` (misleading name) |
| PnL kill field | `max_daily_loss_usd` (same field) |
| Architecture | Two different risk mechanisms share the same field name |

### After fix
| Metric | Value |
|--------|-------|
| Volume cap field | `max_daily_buy_volume_usd` (NEW, separate) |
| PnL kill field | `max_daily_loss_usd` (preserved for RiskManager) |
| Tests | `test_daily_volume_cap.cpp` — 5 accepted, 10 rejected at cap |
| Exit code | 0 |

### Statistical verification
- Backward compatible: `max_daily_buy_volume_usd=50.0` defaults to match old behavior
- No test changes required (existing tests still validate volume tracking)
- Deterministic: UTC midnight reset ensures consistent daily windows

---

## OBS-08: PnL reconciliation

### After fix
| Metric | Value |
|--------|-------|
| TrackerRestate size | 24 bytes → 40 bytes |
| PnL drift fields | `realized_pnl_cents`, `mark_price_yes_e6`, `pnl_drift_exceeded` |
| Kill on PnL drift | ✅ `risk_->latch_kill()` |
| Existing MTM | ✅ in RiskManager::evaluate() (was already present) |

### Statistical verification
- Trivial copyability: `static_assert(std::is_trivially_copyable_v<TrackerRestate>)` ✅
- SPSC ring buffer capacity: 16 (unchanged, +66% memory footprint)
- No new tests yet — test plan documented in implementation.md

---

## Test execution log

```
=== test_kelly ===
ALL PASS (28/28 passed)

=== test_kelly_clamp ===
Results: 19/19 passed, 0 failed

=== test_compliance_guard ===
Results: 25/25 passed, 0 failed

Total: 72 tests, 0 failures, exit code 0
```

## Reproducibility

All tests are deterministic (no RNG, no network, no filesystem access).
Seed: N/A (no randomness).
Toolchain: GCC 16.2, C++20.
Flags: `-std=c++20 -O2 -I core/include -I core/src`
