# Test Coverage Audit — Bot_Crowdintel
**Scope:** `core/include/` + `core/src/` + `core/crypto/` + `alpha/` vs `tests/unit/`, `tests/integration/`, `tests/acceptance/`
**Date:** 2025-10-21
**Total source modules:** ~57 testable units
**Total test files:** 29 (17 unit, 4 integration, 8 acceptance)

---

## 1. UNTESTED MODULES (No coverage whatsoever)

### P0 — CRITICAL

| # | Module | Lines | Risk | Details |
|---|--------|-------|------|---------|
| 1 | `core/src/ws_user_listener.hpp` | 647 | **Critical** | Private WebSocket user channel (L2 API-key auth, SSL/TLS handshake, WebSocket frame masking/unmasking, SHA-1 handshake key, reconnect backoff). Has a `handle_message()` test hook but **zero tests**. Security-critical: handles order fills/cancellations/trade events. Missing test file: `test_user_listener.cpp`. |
| 2 | `core/src/binance_spike_detector.hpp` | 72 | **Critical** | Spike detection ring buffer with cooldown + 500ms confirmation. `update()` is **DECLARED but NOT DEFINED** in the header — the implementation is missing entirely. `has_spike()`, `clear_spike()`, `in_cooldown()` inline. No test file exists. |
| 3 | `core/include/bounded_json.hpp` | 155 | **High** | Allocation-free JSON syntax validator used by `ws_user_listener`, `ws_market_listener`, `alpha_http_receiver`, `user_event_parser`, `evidence_ingress`. Tested only **indirectly** through those consumers. No direct KAT for: `\uXXXX` escapes (lines 95-97), depth-limit rejection, control-char rejection (line 87), number edge cases (leading zeros, `e+`/`e-`, `+inf`/`NaN`), empty document, trailing garbage. Missing test file: `test_bounded_json.cpp`. |
| 4 | `core/src/json_fields.hpp` | 124 | **High** | JSON field extraction (`key_occurrences`, `extract_string`, `find_array`, `fnv_hash`). Used by `user_event_parser`, `evidence_ingress`. No direct table-driven tests for: duplicate key rejection, nested-object key isolation, escape-sequence handling in string values, malformed JSON rejection, `fnv_hash` correctness against known FNV-1a vectors. Missing test file: `test_json_fields.cpp`. |

### P1 — HIGH

| # | Module | Lines | Risk | Details |
|---|--------|-------|------|---------|
| 5 | `core/src/twap_tracker.hpp` | 120 | **High** | TWAP convergence tracker. `update()`, `check()`, `reset()`, `current_twap()`, `deviation()`. **Zero test coverage** — not in unit, integration, or acceptance suites. The `check()` method computes convergence triggers (10 bps threshold at 80% window elapsed) and manipulation alerts (30 bps). TODO comment at line 19: "TODO(P4-T2): Implement convergence trigger + anti-manipulation guard" — may be partially unimplemented. Missing test file: `test_twap_tracker.cpp`. |
| 6 | `core/crypto/keccak256.hpp` | 108 | **High** | Keccak-f[1600] permutation + sponge. Has `keccak256_hash()` and `keccak_f1600()`. **Tested only indirectly** via `EIP712Signer::hash_keccak256()` in `test_signer.cpp` (crypto_kat). The known-answer checks (`keccak256("")`, `keccak256("abc")`) go through the EIP712Signer wrapper, not the raw function directly. Missing direct KAT edge cases: boundary at exactly RATE=136 bytes (single vs. double block), multi-block >2 blocks, max-length input. Missing test file: `test_keccak256_kat.cpp`. |
| 7 | `alpha/crowdintel/alpha_receiver.hpp` | 42 | **Medium** | `AlphaSignal` struct (64-byte cache-line, static_asserts) + `alpha_hash_bytes()` (FNV-1a 64-bit). Tested only **indirectly** through `alpha_parser.hpp` (used by test_core, test_layers). No direct test of hash correctness against known FNV-1a vectors, no test of struct layout/alignment beyond static_asserts. Missing test file: `test_alpha_signal.cpp`. |
| 8 | `core/src/bench_engine.hpp` | 65 | **Low** | Benchmark adapter wrapping ExecutionEngine. `bench_inline_sign()` and `bench_pool_acquire()` helper methods untested. **Low priority** — this is a benchmarking utility, not production path. Missing test file: `test_bench_engine.cpp`. |
| 9 | `core/src/main_hot_path.cpp` | 605 | **Medium** | Entry point + PnL reconciliation (`reconcile_once_with`). No integration test for the full hot-path topology (market WSS → atomic book → alpha HTTP → SPSC → presigner → ladder → execution → network gateway). The `reconcile_once_with` method compares on-chain REST positions vs. local PositionTracker — **critical financial function with no test**. Missing test file: `test_hot_path_reconciliation.cpp` (or `test_pnl_reconciliation.cpp`). |

---

## 2. CMAKE TEST REGISTRATION GAPS

Two test executables are **defined** in `core/CMakeLists.txt` but **NOT registered** with `add_test()` — they compile but are **never executed by `ctest`**:

| Executable | Defined at | Purpose | Status |
|-----------|------------|---------|--------|
| `test_compliance_guard` | line 207 | VER-08: Jurisdiction allowlist, restricted markets, signature-type fail-closed | ❌ **Not in ctest** |
| `test_kelly_clamp` | line 212 | OBS-02: ofi_adjust clamp, CalibrationGuard, vol_adjust no-amplification | ❌ **Not in ctest** |

**Root cause:** `add_test()` calls are missing for these two targets. CI silently skips them.

---

## 3. MISSING TEST FILES (referenced or expected)

| Expected File | Module(s) | Priority |
|--------------|-----------|----------|
| `tests/unit/test_pnl_reconciliation.cpp` | `main_hot_path.cpp` (reconcile_once_with), `position_tracker.hpp`, `risk_manager.hpp` (PnL/MtM) | P0 |
| `tests/unit/test_twap_tracker.cpp` | `core/src/twap_tracker.hpp` | P1 |
| `tests/unit/test_bounded_json.cpp` | `core/include/bounded_json.hpp` | P1 |
| `tests/unit/test_json_fields.cpp` | `core/src/json_fields.hpp` | P1 |
| `tests/unit/test_keccak256_kat.cpp` | `core/crypto/keccak256.hpp` | P1 |
| `tests/unit/test_alpha_signal.cpp` | `alpha/crowdintel/alpha_receiver.hpp` | P1 |
| `tests/unit/test_user_listener.cpp` | `core/src/ws_user_listener.hpp` | P0 |
| `tests/unit/test_bench_engine.cpp` | `core/src/bench_engine.hpp` | P3 |
| `tests/integration/test_pyth_integration.cpp` | `core/src/pyth_client.hpp` (parse path beyond inject_price) | P2 |
| `tests/integration/test_hot_path_e2e.cpp` | Full pipeline: alpha → execution → gateway | P1 |
| `tests/integration/test_websocket_parsing.cpp` | `ws_market_listener.hpp`, `binance_ws_client.hpp` message dispatch (parse_depth_update, parse_trade, parse_kline) | P2 |

---

## 4. MISSING EDGE CASES IN EXISTING TESTS

### P0 — Critical Edge Cases

#### `core/crypto/eip712_signer.hpp` (tested via crypto_kat)
- **No negative-input tests**: null private key, all-zero key, boundary values
- **No signature type 3 (ERC-7739) rejection test**: compliance guard requires rejecting sig type 3, but no test confirms EIP712Signer rejects/handles it
- **No overflow tests**: `maker_amount`/`taker_amount` at UINT64_MAX
- **No replay/sequence edge cases**: timestamp = 0, timestamp far future

#### `core/src/kelly_sizer.hpp` (tested via test_kelly + test_kelly_clamp)
- **Missing**: `ofi_size_multiplier` exceeding `ofi_adjust_max` AND crossing bankroll cap simultaneously (combined clamp)
- **Missing**: `inventory` at exactly ±0.8 boundary (flattening threshold is strict `> 0.8`, need boundary test at 0.7999 and 0.8001)
- **Missing**: `vol_z_score` at exactly 0 (vol_adjust = 1.0 / (1 + max(0,0)) = 1.0) — tested? No explicit boundary
- **Missing**: `calibration_ece` at exactly `ece_threshold` (0.05) — boundary where reduction = 0
- **Missing**: Negative `market_price` (degenerate, should → no trade)
- **Missing**: `market_price` at exactly 1.0 (boundary, should → no trade per `p_model >= price` in kelly_engine)

#### `core/src/fee_calculator.hpp` (tested via test_fees)
- **Missing**: `breakeven_winrate()` — not tested at all (4 functions exist, 2 tested)
- **Missing**: Price clamping edge cases: price = -0.5, price = 1.5, price = NaN
- **Missing**: `edge_after_fees()` with NaN inputs

#### `core/src/ladder_builder.hpp` (tested via test_ladder)
- **Missing**: Skew at exactly ±0.7 clamp boundary (bb_skew + ofi_skew + inv_skew could exceed ±0.7)
- **Missing**: Spread at exactly 1.0 and 10.0 clamps
- **Missing**: Price at exactly 0.01 and 0.99 clamps (bid/ask at boundaries)
- **Missing**: `inventory` at exactly ±0.8 boundary for flattening
- **Missing**: Negative `ofi_multiplier` (should be active=false? currently: `ofi_multiplier > 0.0` → false)
- **Missing**: `vol_multiplier` = 0 (spread = spread_base × 0 = 0 → clamped to 1.0)

#### `core/src/rate_limiter.hpp` (tested via test_rate_limiter)
- **Missing**: Query bucket rate limiting (tests only ORDER and CANCEL)
- **Missing**: `backoff_ms()` method not directly tested (only inferred through `can_send` behavior)
- **Missing**: `RateStatus::status()` — returns RateStatus struct, not tested
- **Missing**: Multiple sequential 429s → backoff sequence (200→400→800→1600→3200→6400→10000 cap) — only single backoff tested

#### `core/src/circuit_breaker.hpp` (tested via test_circuit_breaker)
- **Missing**: `reset()` method — never called or verified in tests
- **Missing**: `ms_until_retry()` boundary (exactly at TTL expiry)
- **Missing**: `on_success()` in CLOSED state (resets failure count — not verified)
- **Missing**: `half_open_max` > 1 (tests use default = 1, multi-trial HALF_OPEN path untested)
- **Missing**: `log_fn_` callback — declared in header (line 93) but no test injects a custom log callback; default is stderr but never verified

#### `core/src/cfc_network.hpp` (tested via test_cfc_kat)
- **Missing**: `load_weights()` from binary file (tests only use `load_kat_weights()` which sets vectors directly)
- **Missing**: `verify_hash()` negative case (wrong hash → returns false)
- **Missing**: Input vector with size mismatch (D_INPUT != 6)
- **Missing**: NaN propagation through all 24 rounds (test only checks single NaN input)

#### `core/src/window_shield.hpp` (tested via test_window_shield)
- **Missing**: `snapshot()` method — not tested (only `update()` is)
- **Missing**: OfiPressure::MODERATE level transition (test only checks EXTREME)
- **Missing**: Time boundary at exactly 250s/275s/295s (tests use values away from boundaries)
- **Missing**: Recovery after HALT (set_window_start again after window expiry)

#### `core/src/ofi_linear_filter.hpp` (tested via test_ofi_filter)
- **Missing**: `process_market_state()` overload — the method `on_book_update` is tested but the MarketState adapter path is not
- **Missing**: Consecutive freeze states (already frozen → still frozen)
- **Missing**: z-score decay between updates (EWMA relaxation when no new data)

#### `core/src/twap_brownian_bridge.hpp` (tested via test_twap_bb)
- **Missing**: `on_price()` method — tests use `set_twap_so_far()` directly, never call `on_price()` for incremental updates
- **Missing**: Zero `time_remaining` (τ=0) → division by zero guard
- **Missing**: NaN price inputs
- **Missing**: `TradeDecision` evaluation with negative edge

#### `core/src/volatility_estimator.hpp` (tested via test_vol_estimator)
- **Missing**: `reset()` — test uses setter injection, never tests reset from steady state
- **Missing**: Negative log returns of varying magnitudes (test uses ±r only)
- **Missing**: Regime transitions (NORMAL→SHOCK→CALM→NORMAL) in sequence
- **Missing**: `on_log_return()` with dt=0 (division by zero risk)

---

## 5. MISSING INTEGRATION TESTS

### P0 — Critical

| Integration Test | Modules | Gap |
|----------------|----------|-----|
| `test_pnl_reconciliation.cpp` | `position_tracker` ↔ `risk_manager` ↔ REST reconciliation | **No test for `reconcile_once_with()`** in `main_hot_path.cpp`. The build_verify dir had `test_reconciliation_integration` but the source is **gone**. This is the only test that reconciles local PnL vs. on-chain REST. |
| `test_alpha_to_execution_e2e.cpp` | `alpha_parser` → `execution_engine` → `order_gateway` | No end-to-end test from AlphaSignal push through ExecutionEngine::run_tick() to gateway submission. Test_core tests pieces in isolation but never the full chain. |

### P1 — High

| Integration Test | Modules | Gap |
|----------------|----------|-----|
| `test_user_channel_flow.cpp` | `ws_user_listener` → `user_event_parser` → `position_tracker` | ws_user_listener has a `handle_message()` hook but no test exercises message parsing → AccountEvent → position accounting. |
| `test_market_ws_parsing.cpp` | `ws_market_listener` (parse_book_snapshot, parse_price_change, parse_tick_size_change) | test_core calls `test_ws_parsing()` but only under `#ifdef CROWDINTEL_HAVE_NETWORK`. Offline builds skip all WS parsing tests. |

### P2 — Medium

| Integration Test | Modules | Gap |
|----------------|----------|-----|
| `test_pyth_parse_integration.cpp` | `pyth_client.hpp` (parse_pyth_update, reconnect logic) | test_multi_feed only tests `inject_price()`. The actual `parse_pyth_update()` and reconnect backoff logic are untested. |
| `test_cfc_network_integration.cpp` | `cfc_network.hpp` (infer, load_weights, weight verification) | test_cfc_kat only tests KAT vectors. Real weight loading from binary file, error handling on corrupt weights, and `verify_hash` integration are untested. |
| `test_ofi_adapter_integration.cpp` | `ofi_calculator` + `binance_ws_client` | test_ofi tests OFICalculator directly but never tests the BinanceWSClient's `parse_depth_update` → `process_event` → `OFICalculator::on_event` chain. |

### P3 — Low

| Integration Test | Modules | Gap |
|----------------|----------|-----|
| `test_execution_engine_hot_path.cpp` | `execution_engine` (full tick cycle with all layers) | test_pipeline.cpp tests `evaluate_signal` end-to-end but does NOT test `run_tick()` with real market data + order submission. |

---

## 6. REGRESSED TEST TARGETS (from build_verify)

The `core/build_verify/CMakeFiles/` directory contains build artifacts for tests that **existed in a prior version** but are **absent from the current CMakeLists.txt and tests/ tree**:

| Removed Test | Last Known Purpose | Status |
|---|---|---|
| `test_reconciliation_integration` | PnL vs REST reconciliation | Source file missing — **must be recreated** |
| `test_position_tracker` | Position accounting, VWAP, dedup | Source file missing — **must be recreated** |
| `test_risk_engine` | RiskManager kill/stop-loss/hedge | Source file missing — partially covered by test_layers |
| `test_fee_model` | FeeCalculator extended | Source file missing — partially covered by test_fees |
| `test_order_manager` | Order gateway/order book integration | Source file missing — partially covered by test_core |
| `test_p0_fixes` | P0 bug fixes verification | Source file missing |
| `test_mempool_listener` | Pyth mempool listener | Source file missing |
| `test_sdk_comparison` | SDK vs raw implementation comparison | Source file missing |
| `test_telemetry` | Telemetry/metrics emission | Source file missing |
| `test_presigned_pool` | PresignedOrderPool (also a CMake target? checking) | Source file missing — partially covered by test_core |

These represent coverage that was **lost** during the P4/Phase-5 refactoring. Investigation needed: check git history for `tests/test_reconciliation_integration.cpp` and `tests/test_position_tracker.cpp`.

---

## 7. SUMMARY BY PRIORITY

### P0 (Immediate — security/critical financial):
1. **Create `test_user_listener.cpp`** — ws_user_listener.hpp has zero coverage (647 lines of WSS+SSL+auth code)
2. **Register test_compliance_guard and test_kelly_clamp in CMake add_test()** — 2 tests compile but never run
3. **Create `test_pnl_reconciliation.cpp`** — PnL reconciliation is financial-critical, was previously tested

### P1 (High — core trading logic):
4. **Create `test_twap_tracker.cpp`** — twap_tracker.hpp completely untested
5. **Create `test_bounded_json.cpp`** — JSON validator safety-critical, only indirect coverage
6. **Create `test_json_fields.cpp`** — field extraction has no direct tests
7. **Create `test_keccak256_kat.cpp`** — dedicated KAT for raw hash function
8. **Create `test_alpha_signal.cpp`** — hash function correctness
9. **Fix missing edge cases** in kelly_sizer, fee_calculator, ladder_builder, circuit_breaker, rate_limiter, window_shield

### P2 (Medium):
10. **Create integration tests** for Pyth parse, CFC weight loading, OFI adapter chain
11. **Add offline tests** for WebSocket parsing (currently `#ifdef CROWDINTEL_HAVE_NETWORK` only)

### P3 (Low — non-critical):
12. **Create `test_bench_engine.cpp`** — benchmark utilities
13. **Add offline test for main_hot_path.cpp** topology

---

## 8. TEST COVERAGE MATRIX

```
Legend: ✅ = unit tested | 🔄 = integration tested | 🔒 = acceptance tested | ❌ = untested

Module                                    | Unit | Integ | Accept | Overall
──────────────────────────────────────────|──────|───────|────────|────────
core/include/bounded_json.hpp             |  ❌  |   –   |   –    | ❌ INDIRECT ONLY
core/include/account_events.hpp           |  ✅  |   –   |   –    | ✅
core/include/bayesian_engine.hpp          |  ✅  |   –   |   –    | ✅
core/include/compliance_guard.hpp         |  ✅* |   –   |   –    | ⚠️  COMPILES, NOT IN CTEST
core/include/evidence.hpp                 |  ✅  |   –   |   –    | ✅
core/include/journal.hpp                  |  ✅  |   –   |   –    | ✅
core/include/order_book.hpp               |  ✅  |   –   |   –    | ✅
core/include/position_tracker.hpp         |  ✅  |   –   |   –    | ✅
core/include/risk_manager.hpp             |  ✅  |   –   |   –    | ✅
core/include/source_reliability.hpp       |  ✅  |   –   |   –    | ✅
core/include/spsc_ring_buffer.hpp         |  ✅  |   –   |   –    | ✅
core/include/time_utils.hpp               |  ✅  |   –   |   –    | ✅
core/include/volatility_gate.hpp          |  ✅  |   –   |   –    | ✅

core/src/alpha_http_receiver.hpp          |  ✅  |   –   |   –    | ✅ (network path)
core/src/binance_spike_detector.hpp       |  ❌  |   –   |   –    | ❌ STUB (update() undefined)
core/src/binance_ws_client.hpp            |  🔄  |  🔄  |   –    | ✅ (mock feed only)
core/src/bench_engine.hpp                 |  ❌  |   –   |   –    | ❌ (benchmark util)
core/src/circuit_breaker.hpp              |  ✅  |  🔄  |  ✅  | ✅
core/src/engine_extensions.hpp            |  –   |  🔄  |   –    | ✅ (single path)
core/src/evidence_ingress.hpp             |  ✅  |   –   |   –    | ✅
core/src/execution_engine.hpp             |  ✅  |  🔄  |  ✅  | ✅
core/src/fee_calculator.hpp               |  ✅  |  🔄  |  ✅  | ✅
core/src/json_fields.hpp                  |  ❌  |   –   |   –    | ❌ INDIRECT ONLY
core/src/kelly_sizer.hpp                  |  ✅  |  🔄  |  ✅  | ✅
core/src/ladder_builder.hpp               |  ✅  |  🔄  |  ✅  | ✅
core/src/main_hot_path.cpp                |  ❌  |   –   |   –    | ❌ NO HOT-PATH TEST
core/src/market_config.hpp                |  ✅  |   –   |   –    | ✅
core/src/mock_client.hpp                  |  ✅  |   –   |   –    | ✅
core/src/multi_feed_manager.hpp           |  –   |  🔄  |   –    | ✅ (integration only)
core/src/ofi_calculator.hpp               |  ✅  |   –   |   –    | ✅
core/src/ofi_linear_filter.hpp            |  ✅  |  🔄  |  ✅  | ✅
core/src/order_gateway.hpp                |  ✅  |   –   |   –    | ✅
core/src/presigned_pool.hpp               |  ✅  |   –   |   –    | ✅
core/src/pyth_client.hpp                  |  –   |  🔄  |   –    | ✅ (inject_price only)
core/src/rate_limiter.hpp                 |  ✅  |  🔄  |  ✅  | ✅
core/src/request_prioritizer.hpp          |  ✅  |   –   |   –    | ✅
core/src/time_strategy.hpp                |  🔄  |  🔄  |  ✅  | ✅
core/src/twap_brownian_bridge.hpp         |  ✅  |  🔄  |  ✅  | ✅
core/src/twap_tracker.hpp                 |  ❌  |   –   |   –    | ❌
core/src/user_event_parser.hpp            |  ✅  |   –   |   –    | ✅
core/src/volatility_estimator.hpp         |  ✅  |  🔄  |  ✅  | ✅
core/src/window_shield.hpp                |  ✅  |  🔄  |  ✅  | ✅
core/src/ws_market_listener.hpp           |  ✅*  |   –   |   –    | ⚠️  NETWORK-ONLY
core/src/ws_user_listener.hpp             |  ❌  |   –   |   –    | ❌

core/crypto/eip712_signer.hpp             |  ✅  |   –   |   –    | ✅
core/crypto/fast_random.hpp               |  ✅  |   –   |   –    | ✅
core/crypto/keccak256.hpp                 |  🔄  |   –   |   –    | ⚠️  INDIRECT KAT ONLY
core/crypto/secure_zero.hpp               |  ✅  |   –   |   –    | ✅
core/crypto/sha256_engine.hpp             |  ✅  |   –   |   –    | ✅

alpha/crowdintel/alpha_parser.hpp         |  ✅  |   –   |   –    | ✅
alpha/crowdintel/alpha_receiver.hpp       |  ❌  |   –   |   –    | ❌ (struct + hash, indirect only)
alpha/strategy/kelly_engine.hpp           |  ✅  |  🔄  |   –    | ✅
```

### Coverage Gaps Summary:
- **9 truly untested modules** (P0/P1 critical)
- **2 CMake registration failures** (tests compile but CI never runs them)
- **9 missing test files** (including PnL reconciliation)
- **5 regresses test targets** lost from build_verify (source files missing)
- **~30 missing edge cases** across tested modules (boundary, clamp, NaN, zero-value, callback)
- **3 missing critical integration tests** (PnL reconciliation, alpha→execution E2E, user-channel WS flow)
