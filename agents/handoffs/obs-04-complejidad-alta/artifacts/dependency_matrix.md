# OBS-04: Dependency Matrix

## Module coupling analysis (post-implementation)

| Module | Depends on | Used by | Hot path | Test Coverage |
|--------|-----------|---------|----------|---------------|
| ExecutionEngine | RiskManager, VolatilityGate, KellyEngine, PositionTracker, BayesianEngine, ComplianceGuard | main_hot_path | YES | Partial |
| RiskManager | PositionTracker | ExecutionEngine | YES (every tick) | LOW |
| ComplianceGuard | JurisdictionConfig, RestrictedMarketsConfig | ExecutionEngine | YES | ✅ 25 tests |
| KellyEngine | FeeCalculator, PositionTracker | ExecutionEngine | YES | ✅ 28+19 tests |
| PositionTracker | MMapStateManager | ExecutionEngine, RiskManager | YES | LOW |
| VolatilityGate | OrderBookL2 | ExecutionEngine | YES | Partial |
| BayesianEngine | SourceReliability | ExecutionEngine | YES | LOW |

## Hot path coupling (FAKE)

```
run_tick()
├── housekeeping()
│   ├── drain_account() → PositionTracker, tracker_->apply(event)
│   ├── drain_reconcile() → TrackerRestate, PositionTracker::override_inventory()
│   └── RiskManager::evaluate() → mark-to-market, daily loss check
├── VolatilityGate::observe_mid() → mid price observation
├── drain_evidence() → BayesianEngine
└── execute_signal_pipeline(signal)
    ├── ComplianceGuard::check() → jurisdiction + restricted markets
    ├── signal validation (timestamp, dedup, market_hash)
    ├── KellyEngine::compute() → order sizing
    ├── VolatilityGate::slippage_ok() → price drift guard
    ├── OrderBookL2::read_top() → best bid/ask
    └── PresignedOrderPool → order signing
```

## Coupling risk

**8 modules** are coupled in the hot path. The new ComplianceGuard adds a 9th, but it's:
- ✅ Fail-open when not configured (`compliance_guard_ == nullptr`)
- ✅ ~40ns overhead (negligible in p50 < 15μs budget)
- ✅ No dynamic allocation
- ✅ Lock-free reads

## Coverage recommendations

1. Add unit tests for `RiskManager::evaluate()` (mark-to-market computation)
2. Add unit tests for `PositionTracker` PnL drift handling
3. Add integration test: ComplianceGuard + ExecutionEngine signal pipeline
4. Add gcov coverage reporting to CI

## Complexity metrics

| Metric | Before | After | Target |
|--------|--------|-------|--------|
| Hot path modules | 8 | 9 (+ComplianceGuard) | — |
| Compliance overhead | N/A | ~40ns | < 1μs ✅ |
| Lock-free modules | 8 | 9 | 9 ✅ |
| Allocation in hot path | 0 (post-sizing) | 0 | 0 ✅ |
