# AUD-SEC Report — VER-08: Revisión legal/compliance

**Date:** 2026-01-22 | **Auditor:** AUD-SEC | **Commit:** c157c23

## Resumen

ComplianceGuard está **documentado en README.md (línea 100) y ARCHITECTURE_MAP.md (línea 20)** como paso 1 del hot path, pero **NO EXISTE como implementación**. ❌ No hay jurisdiction check, ❌ no hay restricted tokens list, ❌ no hay fail-closed enforcement.

## Evidencia

### Documentation vs Code mismatch

```bash
$ grep -rn "Compliance\|compliance\|jurisdiction\|restricted_token" core/src/
# Resultado: 0 matches — ComplianceGuard is DOCUMENTED but NOT IMPLEMENTED
```

**README.md línea 100:**
> "1. ComplianceGuard — jurisdiction + restricted tokens (fail-closed)"

**ARCHITECTURE_MAP.md línea 20:**
> `├─ 1. ComplianceGuard (jurisdiction + restricted tokens, fail-closed)`

**execution_engine.hpp hot path:**
```cpp
TickResult run_tick() {
    const TickResult housework = housekeeping();  // ← NO ComplianceGuard
    // ... risk checks ...
    // ... volatility gate ...
    // ... sizing ...
    // ... risk authorize ...
    // ... sign + submit ...
}
```

### RiskManager — partial jurisdiction support

```cpp
// risk_manager.hpp líneas 32-39:
struct RiskLimits {
    double stop_loss_pct = 0.15;
    double hedge_trigger_pct = 0.0;
    double max_daily_loss_usd = 50.0;
    double max_market_exposure_usd = 250.0;
    double max_portfolio_exposure_usd = 250.0;
    uint64_t version = 0;
};
```

→ NO `allowed_jurisdictions`, NO `restricted_tokens` field.

### ERC-7739 (type 3)

```cpp
// market_config.hpp línea 295:
return "POLY_1271 requires ERC-7739 wrapping; this build fails closed. Use type 0/1/2 or the official V2 signer sidecar";
```

✅ Type 3 fails closed (rejects the order), but:
- ❌ Not implemented as a working feature
- ❌ No test for fail-closed behavior

### Tests

```bash
$ grep -rn "compliance\|jurisdiction\|restricted" tests/
# Resultado: 0 matches — NO compliance tests
```

### ADR

```bash
$ ls adr/
# Resultado: No adr/ directory — no ADRs exist
```

## Conclusión

| Criterio | Cumplido | Evidencia |
|----------|---------|-----------|
| Jurisdiction guard fail-closed | ❌ | No existe ComplianceGuard |
| Restricted tokens list | ❌ | No existe lista |
| Fail-closed tests | ❌ | No existen tests |
| ADR compliance | ❌ | No existe adr/ |
| ERC-7739 fail-closed + test | ⚠️ | Documented but not tested |

## Recomendación

1. ADR-0002: Compliance scope
2. Implement ComplianceGuard class:
   - Jurisdiction whitelist (env-based config)
   - Restricted tokens list (env-loaded JSON)
   - Fail-closed by default
3. Tests fail-closed
