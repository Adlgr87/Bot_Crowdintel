# ADR-0002: Compliance Scope — ComplianceGuard Required

| Field | Value |
|-------|-------|
| **Status** | Proposed |
| **Date** | 2026-01-22 |
| **Author** | AUD-SEC |
| **Deciders** | AXIOM (architect), AUD-SEC, LEGION (maintenance) |
| **Related cards** | VER-08 |

## Context

ComplianceGuard is referenced in README.md (línea 100) and ARCHITECTURE_MAP.md (línea 20) as "step 1 of ExecutionEngine::run_tick()" with "jurisdiction + restricted tokens (fail-closed)". However, **no ComplianceGuard implementation exists in the code**.

## Decision

**Implement ComplianceGuard** as a fail-closed gate in the hot path.

### Why
- Regulatory requirement for automated trading bots
- Jurisdiction restrictions (US, UK, EU markets have different rules)
- Restricted tokens (OFAC-sanctioned, high-risk)
- Legal liability if bot trades restricted assets

### Design

```cpp
class ComplianceGuard {
public:
    explicit ComplianceGuard(const MarketConfig& cfg) noexcept;
    
    // Fail-closed check — returns true ONLY if ALL checks pass
    bool check(const PolymarketOrder& order) const noexcept;
    
    // Configuration:
    // - BOT_JURISDICTION: allowed jurisdictions (default: "US")
    // - BOT_RESTRICTED_TOKENS: comma-separated list
    // - BOT_COMPLIANCE_MODE: "strict" (fail-closed) or "warn" (log only)
    
private:
    std::unordered_set<std::string> allowed_jurisdictions_;
    std::unordered_set<std::string> restricted_tokens_;
    bool fail_closed_ = true;
};
```

### Hot path placement

```cpp
TickResult run_tick() {
    // Step 0: ComplianceGuard (NEW)
    if (!compliance_->check(order)) {
        return TickResult::COMPLIANCE_REJECTED;
    }
    // Step 1: housekeeping
    // ... rest unchanged
}
```

## Consequences

### Positive
- Fail-closed compliance gate
- Jurisdiction + restricted tokens enforcement
- Tests validate fail-closed behavior
- ADR documents legal scope

### Negative
- New dependency in hot path (must be < 1μs)
- Feature flag for canary (BOT_COMPLIANCE_GUARD, default true)
- May initially block some market configs

## Action items

1. Implement `core/include/compliance_guard.hpp`
2. Add to HotPathConfig in market_config.hpp
3. Wire into ExecutionEngine::run_tick()
4. Create `tests/unit/test_compliance_guard.cpp`
5. Create `adr/0002-compliance-scope.md` (this document) — done
6. Update README.md — ComplianceGuard now exists, not "documented only"

## Legal scope

This ADR covers:
- Jurisdiction eligibility check
- Restricted tokens list (OFAC, exchange-blocked)
- Operator identity verification (env: BOT_OPERATOR_JURISDICTION)
- NOT a full legal opinion — operator must consult legal counsel for their jurisdiction

## Metrics

- `crowdintel_compliance_rejections_total`: Counter of rejections
- `crowdintel_compliance_mode`: "strict" or "warn"
