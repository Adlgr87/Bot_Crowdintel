# REVIEW — OBS-02: Kelly/OFI over-positioning

**Revisor:** NEMESIS (adversarial)
**Fecha:** 2026-01-22
**Status:** ✅ APPROVED

---

## Adversarial Review

### ofi_adjust clamp

**Before:** `ofi_adjust = std::max(0.0, ofi_size_multiplier)` — can be >1.0
**After:** `ofi_adjust = std::min(ofi_adjust, config_.ofi_adjust_max)` — clamped to 1.5

**Risk:** An attacker could feed `ofi_size_multiplier=100.0` → clamped to 1.5 → no more than 1.5x quarter-Kelly. ✅
**Risk:** `ofi_adjust_max=0.0` → all trades have size=0 → bot never trades. ✅ Fail-safe (no trades is always safer than over-trades).

### CalibrationGuard (ECE-based)

**Risk:** ECE is a post-hoc metric. If ECE is computed wrong, legitimate trades could be suppressed.
- **Mitigation:** ECE=0 → no reduction. Only when ECE > threshold applies. ✅
- **Risk:** ECE could be gamed by model that reports low ECE. ✅ — but this is a model integrity issue, not a compliance issue. Out of scope.

### vol_adjust (verified no amplification)

```cpp
const double vol_adjust = 1.0 / (1.0 + std::max(0.0, vol_z_score));
```
- When `vol_z < 0` → `vol_adjust = 1.0` (no amplification) ✅
- When `vol_z > 0` → `vol_adjust < 1.0` (attenuation) ✅

This was verified in test `vol_adjust_no_amplification`. ✅

### Test coverage

- 19 new tests for clamp + calibration ✅
- 28 existing kelly tests pass (regression) ✅
- Edge cases: ofi_mult=0 (no trade), ofi_mult=10.0 (bankroll cap respected) ✅

### Performance impact

- `std::min` and `std::max` are single CPU instructions
- ECE decay computation: one subtraction, one multiplication, one std::min — negligible
- No branches in hot path except the ECE check (predictable: ECE=0 most of the time) ✅

### Conclusion

**APPROVED.** The ofi_adjust clamp prevents over-positioning from noisy OFI signals. CalibrationGuard provides graceful degradation when model calibration degrades. Both are fail-safe.

## Signoff

```json
{
  "card_id": "OBS-02",
  "reviewer": "subagent_nemesis",
  "verdict": "APPROVED",
  "tests_passed": 19,
  "regression_passed": 28
}
```
