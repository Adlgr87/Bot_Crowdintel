# AUD-ARCH Report — OBS-05: Ambigüedad market-making

**Date:** 2026-01-22 | **Auditor:** AUD-ARCH + AUD-DOCS | **Commit:** c157c23

## Resumen

LadderBuilder construye un **single bid/ask quote pair** (no market-making continuo). La documentación es contradictoria: README dice "envía órdenes" (discrete execution) pero ARCHITECTURE_MAP menciona "LadderSkew + 16-order pool" (implica market-making).

## Evidence

### Code analysis

**ladder_builder.hpp — build() (líneas 53-121):**

```cpp
Quote build(...) const noexcept {
    // Returns SINGLE Pair: {bid_price, ask_price, bid_size, ask_size}
    // NO continuous quote management, NO cancel/replace loop
}
```

- Single pair output per invocation ✅
- No cancel/replace loop ✅
- No persistent quote management ✅
- Not continuous market-making ✅

### Documentation discrepancy

| Document | Says | Implication |
|----------|------|-------------|
| README.md línea 11 | "envía órdenes firmadas" | Discrete execution |
| README.md línea 140 | "LadderBuilder + TimeStrategy: quotes con TTL" | Implies quote management |
| ARCHITECTURE_MAP.md línea 20 | "ComplianceGuard (step 1)" | (also doesn't exist in code) |
| ARCHITECTURE_MAP.md línea 53 | "LadderSkew ... 16-order pool construction" | Implies market-making |
| ARCHITECTURE.md línea 5 | "hot loop does NOT perform DNS, TLS, JSON" | Discrete execution |

### Execution flow

```
run_tick()
  → build_quote() → LadderBuilder::build()  ← SINGLE quote pair
  → PresignedOrderPool::acquire_at_most()
  → OrderGateway::submit()  ← SINGLE order
```

NO quote refresh loop. NO cancel/replace.

## Conclusion

The bot does **discrete execution**, NOT continuous market-making. Documentation is inconsistent — "LadderSkew" and "16-order pool" terminology implies market-making that doesn't exist.

## Recommendation

1. ADR-0001: Define bot scope clearly
2. Update docs to eliminate "market-making" terminology
3. Rename "LadderSkew" → clarify it means quote skewing, not continuous MM
4. Align README/ARCHITECTURE_MAP/ARCHITECTURE.md
