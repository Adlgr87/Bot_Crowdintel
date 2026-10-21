# Adversarial Security Audit Report
## Bot_Crowdintel — Compliance Guard & Execution Engine

**Audit Date:** 2025
**Auditor:** OMNISCIENT Subagent
**Files in Scope:**
- `core/include/compliance_guard.hpp` (131 lines)
- `core/src/execution_engine.hpp` (963 lines)
- `tests/unit/test_compliance_guard.cpp` (224 lines)

**Integration Files Reviewed:**
- `core/src/market_config.hpp`, `alpha/crowdintel/alpha_parser.hpp`,
- `core/src/alpha_http_receiver.hpp`, `core/src/main_hot_path.cpp`,
- `tests/unit/test_core.cpp`, `tests/unit/test_layers.cpp`,
- `core/include/spsc_ring_buffer.hpp`, `core/include/position_tracker.hpp`,
- `core/include/order_book.hpp`, `core/include/risk_manager.hpp`,
- `core/src/polymarket_order.hpp`

**Classification:** Confidential — Security Audit

---

## Executive Summary

**2 CRITICAL**, **4 HIGH**, **7 MEDIUM**, **4 LOW** vulnerabilities identified.

The most severe finding is that the compliance guard is **completely non-functional in production** — the `ComplianceConfig` struct is defined and the `ComplianceGuard` class is implemented but is **never instantiated or wired into the ExecutionEngine** in any production code path. All compliance checks are silently skipped, creating a fail-open system despite documentation claiming fail-closed semantics.

---

## CRITICAL Findings

### CRITICAL-1: Compliance guard is never wired up in production — complete bypass

**Files:** `core/src/execution_engine.hpp` (lines 168, 184–189, 274)
**Also:** `core/src/main_hot_path.cpp` (lines 514–516, 521–523)

**Description:**
The `ExecutionEngine` constructor accepts a `const ComplianceConfig* compliance = nullptr` parameter (line 168). In production code (`main_hot_path.cpp`), the engine is instantiated **without** passing this parameter, so it defaults to `nullptr`. The constructor only sets `compliance_guard_` when `compliance != nullptr` (lines 187–189):

```cpp
if (compliance) {
    compliance_guard_ = &compliance->guard;
}
```

Since `compliance_guard_` remains `nullptr`, the compliance check at line 274:

```cpp
if (compliance_guard_ && cfg_.compliance_enabled) {
```

**always evaluates to `false`**, regardless of the `cfg_.compliance_enabled` flag (which defaults to `true` in `MarketConfig`, market_config.hpp line 71). All orders are processed through the full signal pipeline with **zero compliance validation**.

**Evidence:** Grep across the entire codebase shows `ComplianceConfig` appears only in `execution_engine.hpp` (struct definition + constructor param). No `.cpp` file — not `main_hot_path.cpp`, not any test — ever constructs or passes a `ComplianceConfig`. The `ComplianceGuard::parse_restricted()` method exists but is called **only** in `test_compliance_guard.cpp` (line 147).

**Impact:** The entire compliance infrastructure (jurisdiction checks, restricted market lists, restricted condition IDs) provides **zero protection in production**. A bot operator in a restricted jurisdiction or trading restricted markets would have no enforcement whatsoever. This is a complete fail-open condition.

**Fix:** Either:
1. Make compliance mandatory (remove the nullptr default, require explicit opt-out), or
2. Construct `ComplianceConfig` from `MarketConfig` at engine startup, parsing `restricted_condition_ids` and `restricted_market_hashes` strings into a `RestrictedMarketsConfig` via `parse_restricted()`.

---

### CRITICAL-2: Condition ID restriction bypass via empty/null input

**Files:** `core/include/compliance_guard.hpp` (lines 79–84)

**Description:**
The condition ID check is guarded by a null/empty check:

```cpp
if (market_condition_id && *market_condition_id) {
    if (restricted_.condition_ids.find(market_condition_id) !=
        restricted_.condition_ids.end()) {
        return {false, "compliance: restricted market (condition_id)"};
    }
}
```

When `market_condition_id` is `nullptr` or an empty string `""`, the **entire condition ID restriction check is skipped**. The market hash check (lines 87–90) still runs, but only if the hash is in the restricted set. If the operator does not set `BOT_MARKET_CONDITION_ID` (which defaults to `""` in `MarketConfig::load()` at market_config.hpp line 326):

```cpp
!copy_env(market_condition_id, "BOT_MARKET_CONDITION_ID", "") ||
```

…then **all condition ID restrictions are silently bypassed**. The hash check remains, but condition IDs are the canonical market identifier on Polymarket — an operator who omits the condition ID entirely evades that restriction layer.

The test at `test_compliance_guard.cpp` line 89 even verifies this bypass as "correct" behavior:
```cpp
auto r3 = guard.check("", 999);
CHECK(r3.allowed, "empty condition_id allowed");  // line 89
```

**Impact:** An operator can bypass condition ID-based market restrictions simply by not configuring `BOT_MARKET_CONDITION_ID`. The compliance comment at market_config.hpp line 461 acknowledges that `operator_jurisdiction` is required when compliance is enabled, but makes no equivalent requirement for `market_condition_id`.

**Fix:** When compliance is enabled, treat a missing/empty `market_condition_id` as a compliance violation (fail-closed), or at minimum require it to be set when `compliance_enabled` is true.

---

### CRITICAL-3: MarketConfig loads restriction config as strings but never parses them

**Files:** `core/src/market_config.hpp` (lines 73–74, 446–460)
**Also:** `core/src/execution_engine.hpp` (lines 151–155, 184–189)

**Description:**
`MarketConfig` stores the compliance restriction data as raw CSV strings:

```cpp
char restricted_condition_ids[512]{};      // line 73
char restricted_market_hashes[512]{};      // line 74
```

These are populated from environment variables at lines 449–454:
```cpp
!copy_env(restricted_condition_ids, "BOT_RESTRICTED_CONDITION_IDS", ...)
!copy_env(restricted_market_hashes, "BOT_RESTRICTED_MARKET_HASHES", ...)
```

And validated for control characters (lines 455–460):
```cpp
if (!safe_config_text(restricted_condition_ids)) return "...";
if (!safe_config_text(restricted_market_hashes)) return "...";
```

However, **these strings are never parsed into a `RestrictedMarketsConfig`**. There is no call to `ComplianceGuard::parse_restricted()` anywhere in production code. The `RestrictedMarketsConfig` struct (compliance_guard.hpp lines 31–34) exists but is only ever populated in test code. Even if a `ComplianceConfig` were provided to `ExecutionEngine` (which it currently isn't — see CRITICAL-1), the restricted lists would be **empty** because the strings from `MarketConfig` are never transformed into the hash set data structure.

**Impact:** Even if CRITICAL-1 were fixed by wiring up the `ComplianceConfig`, the restriction lists would still be empty. An operator setting `BOT_RESTRICTED_CONDITION_IDS=0xabc...` would have zero effect — the restricted IDs are loaded into a string buffer and then never read.

**Fix:** Add a method to `MarketConfig` or `ExecutionEngine` constructor that calls `ComplianceGuard::parse_restricted()` on both `restricted_condition_ids` and `restricted_market_hashes`, then constructs a properly populated `JurisdictionConfig` and `RestrictedMarketsConfig` for the `ComplianceGuard`.

---

## HIGH Findings

### HIGH-1: Default ComplianceGuard constructor is fail-open, contradicting documentation

**Files:** `core/include/compliance_guard.hpp` (lines 49–51, 92); `tests/unit/test_compliance_guard.cpp` (lines 169–175)

**Description:**
The header comment at lines 49–51 states:
```
// Default constructor — fail-closed (all checks enabled but no allowlist).
// In this mode, the guard will reject everything — safe for canary "off".
```

This is **factually incorrect**. The default constructor `ComplianceGuard() noexcept = default;` produces a guard where:
- `jur_.enabled` defaults to `false` (JurisdictionConfig line 26: `bool enabled = false`)
- `restricted_.condition_ids` is empty
- `restricted_.market_hashes` is empty

The `check()` method (line 67–93) will therefore **allow everything** — jurisdiction check is disabled, no restricted IDs, no restricted hashes. The return at line 92 is `{true, nullptr}`.

The test `test_fail_closed_default` (test_compliance_guard.cpp lines 169–179) verifies this fail-open behavior while being misleadingly named:
```cpp
auto r1 = guard.check("", 0, nullptr);
CHECK(r1.allowed, "default guard allows when everything disabled");  // line 175
```

**Impact:** Any code path that relies on the default constructor (including the unwired `ComplianceConfig` whose `ComplianceGuard guard` member uses default construction — execution_engine.hpp line 154) will allow all markets and jurisdictions without any checks.

**Fix:** Either make the default constructor fail-closed (set `jur_.enabled = true` with an empty allowlist), or remove the misleading comment and clearly document that the default is permissive. Better: remove the default constructor entirely and require explicit configuration.

---

### HIGH-2: Compliance check is opt-in via nullptr — fail-open by default

**Files:** `core/src/execution_engine.hpp` (lines 184–189, 274)

**Description:**
The constructor comment at lines 184–186 states:
```
// VER-08: ComplianceGuard is wired from the cold-path config.  When
// compliance is not provided (nullptr), the guard is NOT active — callers
// must explicitly opt in so fail-closed behavior is intentional.
```

While this is intentional design, it creates a **fail-open default**: the `compliance` parameter defaults to `nullptr`, and `cfg_.compliance_enabled` defaults to `true` (market_config.hpp line 71). The check at line 274 requires **BOTH** conditions:
```cpp
if (compliance_guard_ && cfg_.compliance_enabled) {
```

This two-factor gate means compliance can be silently disabled by either:
1. Not passing a `ComplianceConfig` (current production state — see CRITICAL-1)
2. Setting `compliance_enabled = false` in config

The `ComplianceConfig` struct (execution_engine.hpp lines 153–155) contains a default-constructed `ComplianceGuard` that is fail-open (see HIGH-1). Even a properly passed `ComplianceConfig` would provide no protection unless its `guard` member is explicitly configured with `JurisdictionConfig` and `RestrictedMarketsConfig`.

**Impact:** Two independent failure modes can silently disable compliance. The current production code hits failure mode #1.

**Fix:** Make compliance mandatory. Remove the `compliance = nullptr` default and require callers to explicitly pass a configured `ComplianceConfig*` (or `nullptr` with documented intent). At minimum, when `cfg_.compliance_enabled == true`, require a non-null `compliance` pointer and fail to start if it's not provided.

---

### HIGH-3: Protective orders bypass compliance check entirely

**Files:** `core/src/execution_engine.hpp` (lines 729–817)

**Description:**
The `execute_protective()` method handles stop-loss and hedge orders. It performs `compute_order_amounts`, `signer_.sign_order()`, `build_wire_body()`, and `client_.submit()` **without ever calling `compliance_guard_->check()`**. This path is reached via `housekeeping()` (line 711) which runs at the start of every tick, before the main signal pipeline.

If the compliance guard were properly wired (see CRITICAL-1), protective orders would still **bypass** the jurisdiction check and restricted market list. The method goes directly to signing and submitting:

```cpp
// execution_engine.hpp line 442-466 (signal pipeline):
if (compliance_guard_ && cfg_.compliance_enabled) {
    const auto result = compliance_guard_->check(...);
    if (!result.allowed) { return TickResult::COMPLIANCE_REJECTED; }
}
// ... then sign and submit ...

// execution_engine.hpp line 729-817 (protective path):
// No compliance check — goes straight to sign_order() and client_.submit()
```

**Impact:** If a bot is in a restricted jurisdiction or trading a restricted market, protective exits would still send signed orders to the venue, potentially violating regulatory compliance. While this may be intentional (protecting existing positions), the bypass is undocumented and unchecked.

**Fix:** Add a compliance check to `execute_protective()`, or document explicitly that protective orders are exempt from compliance (with risk analysis justifying the exemption).

---

## MEDIUM Findings

### MEDIUM-1: Non-constant-time comparisons enable timing attacks

**Files:** `core/include/compliance_guard.hpp` (lines 73, 80, 87-88)

**Description:**
The compliance check uses `std::unordered_set` lookups which are **not constant-time**:

1. **Jurisdiction check** (line 73): `jur_.allowed.find(operator_jurisdiction)` — hash-based lookup with variable-time string comparison. The timing difference between "not found" (traversal of collision chain + final mismatch) and "found" (successful match) could leak whether a jurisdiction is allowlisted.

2. **Condition ID check** (line 80): `restricted_.condition_ids.find(market_condition_id)` — same timing vulnerability. An attacker measuring compliance check latency could determine whether a condition ID is in the restricted set.

3. **Market hash check** (line 87): `restricted_.market_hashes.find(market_hash)` — uint64_t comparison is fast, but the hash lookup still has variable timing based on collision chain length.

While the inputs currently come from `MarketConfig` (not directly attacker-controlled), the `AlphaHttpReceiver` accepts signals over HTTP, and if the compliance check were extended to validate incoming signal fields (rather than only config fields), these timing leaks would become exploitable.

**Impact:** Low probability of exploitation given current data flow, but violates defense-in-depth and security best practices for compliance-critical checks.

**Fix:** Use a constant-time lookup for jurisdiction and condition_id checks (e.g., always compare against all entries, or use HMAC-based set membership).

---

### MEDIUM-2: parse_restricted accepts malformed condition IDs without validation

**Files:** `core/include/compliance_guard.hpp` (lines 106–109)

**Description:**
The heuristic for parsing restricted condition IDs accepts **any** token starting with "0x" or "0X" without validating it's a proper condition ID (0x + 64 lowercase hex characters):

```cpp
if (token.size() >= 2 && token[0] == '0' &&
    (token[1] == 'x' || token[1] == 'X')) {
    rm.condition_ids.emplace(token);  // no format validation!
}
```

This means entries like `"0x"`, `"0xGGGG"`, `"0x123"` (too short), or `"0X1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF1234567890ABCDEF"` (65 hex chars — too long) would all be stored in the restricted set. A malformed entry like `"0xGG"` would **never match** a real `market_condition_id` (which must be 0x + 64 valid lowercase hex chars per `MarketConfig::valid_condition_id()` at market_config.hpp lines 536–544), making the restriction entry a **no-op** — a false sense of security.

The test at line 147 uses valid-looking IDs (`"0x1234,0x5678"`) but no test verifies malformed input handling.

**Impact:** Operators may believe certain markets are restricted when they are not, due to typos or format mismatches in the config string.

**Fix:** Validate that condition ID tokens match the expected format (0x + exactly 64 hex chars, lowercase) before inserting into the restricted set. Reject invalid tokens with an error.

---

### MEDIUM-3: parse_restricted uses base-0 numeric parsing (octal surprise)

**Files:** `core/include/compliance_guard.hpp` (line 113)

**Description:**
```cpp
uint64_t h = std::stoull(std::string(token), nullptr, 0);
```

Using base `0` means `std::stoull` auto-detects the base from the prefix:
- `"0x1234"` → parsed as hex
- `"010"` → parsed as **octal** (value 8, not 10)
- `"10"` → parsed as decimal

If an operator intends to restrict market hash `10` (decimal) but writes it as `"010"` in the CSV, or if a hash happens to start with a leading zero, the hash would be misinterpreted. The hash value stored in the restricted set would not match the actual `market_hash` computed by `alpha_hash_bytes()`, causing the restriction to **silently fail**.

Additionally, `std::string(token)` creates a temporary allocation on every non-0x token, which is wasteful and could fail in constrained environments (see MEDIUM-4).

**Impact:** Restricted market hashes may not actually be restricted due to base interpretation mismatches.

**Fix:** Use base `10` explicitly: `std::stoull(std::string(token), nullptr, 10)`.

---

### MEDIUM-4: parse_restricted is noexcept but can trigger std::terminate

**Files:** `core/include/compliance_guard.hpp` (lines 96, 109, 113)

**Description:**
The method `parse_restricted` is declared `noexcept` (line 96), but:
1. Line 109: `rm.condition_ids.emplace(token)` — `std::unordered_set<std::string>::emplace` can throw `std::bad_alloc` on memory allocation failure. Since the function is `noexcept`, this would call `std::terminate()`.
2. Line 113: `std::stoull(std::string(token), nullptr, 0)` — while wrapped in `try { ... } catch (...) { }` (line 112), the `std::string(token)` constructor itself could throw `std::bad_alloc` **before** entering the try block. Wait — actually, `std::string(token)` is inside the try block, so `std::bad_alloc` from the string constructor WOULD be caught. But `rm.condition_ids.emplace(token)` at line 109 is **outside** any try/catch and can throw.

Looking more carefully at the code structure:
```cpp
// Line 109: emplace is NOT in a try block
rm.condition_ids.emplace(token);  // can throw bad_alloc

// Lines 112-117: stoull IS in a try block  
try {
    uint64_t h = std::stoull(std::string(token), nullptr, 0);
    rm.market_hashes.insert(h);
} catch (...) {
    // Not a valid hash — skip
}
```

The `emplace` for condition IDs (line 109) is not protected against allocation failure. In a `noexcept` function, `std::bad_alloc` triggers `std::terminate()`.

**Impact:** In memory-constrained environments, parsing a long CSV of condition IDs could crash the process — a DoS vector during startup.

**Fix:** Remove `noexcept`, or pre-allocate the unordered_set with `reserve()`, or wrap all allocations in try/catch.

---

### MEDIUM-5: Evidence dedup uses weak 32-entry linear scan vulnerable to adversarial flooding

**Files:** `core/src/execution_engine.hpp` (lines 573–578)

**Description:**
The evidence deduplication uses a simple 32-entry ring buffer with linear scan:

```cpp
bool seen_evidence_before(uint32_t hash) noexcept {
    for (const uint32_t seen : seen_evidence_)
        if (seen == hash) return true;
    seen_evidence_[evidence_seen_idx_++ & 31] = hash;
    return false;
}
```

Unlike the signal dedup which uses a 65,536-bit Bloom filter (lines 872–908), evidence dedup uses only 32 slots. An adversary sending >32 unique evidence events per drain cycle would cause every event to be treated as "not seen before" and processed by the Bayesian engine. Each call to `bayes_->update_count()` or `bayes_->update_lr()` (lines 563, 565) performs floating-point computation. Flooding the evidence queue could cause sustained CPU consumption in the hot path.

The SPSC ring buffer size limits the damage per drain cycle, but the evidence queue has no independent rate limiting.

**Impact:** Medium — could cause CPU exhaustion if evidence events are flooded, degrading latency guarantees for order execution.

**Fix:** Use a Bloom filter for evidence dedup (similar to signal dedup), or add rate limiting on evidence processing.

---

### MEDIUM-6: Tests do not cover execution_engine compliance integration

**Files:** `tests/unit/test_compliance_guard.cpp` (entire file)

**Description:**
All 7 tests in `test_compliance_guard.cpp` test `ComplianceGuard` in isolation. None of them:
1. Test that `ExecutionEngine` correctly rejects orders when a `ComplianceConfig` is provided with restrictive settings
2. Test that `ExecutionEngine` works correctly when `compliance_enabled` is set
3. Test that protective orders handle compliance
4. Test the `cfg_.compliance_enabled` flag interaction with `compliance_guard_`

Meanwhile, `test_core.cpp` (line 690–691) and `test_layers.cpp` (lines 313–314, etc.) instantiate `ExecutionEngine` without passing a `ComplianceConfig`, meaning **none of the existing tests would detect the compliance bypass** described in CRITICAL-1.

**Impact:** The CRITICAL-1 bypass has persisted undetected because no integration test covers the compliance path through `ExecutionEngine`.

**Fix:** Add integration tests in `test_core.cpp` or a new `test_compliance_integration.cpp` that verify compliance rejection through the full engine pipeline.

---

### MEDIUM-7: test_fail_closed_default misnames and misverifies fail-closed behavior

**Files:** `tests/unit/test_compliance_guard.cpp` (lines 169–179)

**Description:**
The test `test_fail_closed_default` verifies that the default-constructed guard **allows everything**:

```cpp
static void test_fail_closed_default() {
    ComplianceGuard guard;  // default: disabled, no restrictions
    auto r1 = guard.check("", 0, nullptr);
    CHECK(r1.allowed, "default guard allows when everything disabled");
```

The test name ("fail_closed_default") contradicts its assertion (allows everything). The header comment (compliance_guard.hpp line 49) claims the default constructor is "fail-closed" and "will reject everything," but the test proves it does the opposite. This confusion likely contributed to the production bypass (CRITICAL-1) — developers may have assumed the default was fail-closed.

**Impact:** Misleading test naming and documentation created false confidence in the compliance system's safety.

**Fix:** Rename the test to `test_default_allows_everything` and update the header comment to accurately describe the default behavior. Better: make the default constructor fail-closed.

---

### MEDIUM-8: No validation of operator_jurisdiction format

**Files:** `core/include/compliance_guard.hpp` (lines 70–72)

**Description:**
The jurisdiction check at line 70-72 only verifies the jurisdiction is non-null and non-empty:
```cpp
if (!operator_jurisdiction || !*operator_jurisdiction) {
    return {false, "compliance: jurisdiction not provided"};
}
```

It does **not** validate that the jurisdiction is a well-formed ISO 3166-1 alpha-2 code (2 uppercase letters). A jurisdiction like `"US123"` or `"XYZ"` would pass the presence check and then fail the set lookup with a misleading error message ("jurisdiction restricted" — the real issue is malformed input, not a restriction).

The `operator_jurisdiction` field in `MarketConfig` (market_config.hpp line 72) is a `char[8]` with `safe_config_text` validation (no control characters), but the `check()` method itself performs no format validation.

**Impact:** Misconfigured jurisdiction strings produce confusing error messages, making debugging difficult. Operators might incorrectly assume their jurisdiction is blocked when the real issue is a typo.

**Fix:** Add format validation (exactly 2 uppercase ASCII letters, or validate against ISO 3166-1) in the `check()` method.

---

### MEDIUM-9: Market hash check uses config hash, not signal hash

**Files:** `core/src/execution_engine.hpp` (lines 274–282 vs. line 290)

**Description:**
The compliance check (lines 274–282) uses `cfg_.market_hash` (the bot's configured market hash), NOT `signal.market_hash` (the incoming signal's market hash). The signal's market is validated separately at line 290:

```cpp
// Line 274-282: Compliance checks cfg_.market_hash (config)
if (compliance_guard_ && cfg_.compliance_enabled) {
    const auto result = compliance_guard_->check(
        cfg_.market_condition_id, cfg_.market_hash,
        cfg_.operator_jurisdiction);
    if (!result.allowed) { ... }
}

// Line 290: Signal's market checked separately
if (signal.market_hash != cfg_.market_hash)
    return TickResult::WRONG_MARKET;
```

The compliance guard checks whether the **bot's configured market** is restricted, not whether the **specific signal's market** is restricted. Since `AlphaParser` (alpha_parser.hpp lines 42–44) already validates that the signal's market string matches the expected market, the signal's `market_hash` always equals `cfg_.market_hash`. So the compliance check effectively runs once per engine (checking the config), not once per signal.

This is a design issue: if the bot ever handled multiple markets (e.g., a multi-market deployment), the compliance check would not protect against signals targeting restricted markets that differ from the configured one.

**Impact:** Low in current single-market architecture, but prevents future multi-market compliance.

**Fix:** Document that the guard checks the configured market, not per-signal markets. If multi-market support is planned, add signal-level condition_id and hash checks.

---

## LOW Findings

### LOW-1: Journal silently drops events when ring buffer is full

**Files:** `core/src/execution_engine.hpp` (lines 820–831)

**Description:**
The `journal()` method uses `try_push` with the return value explicitly discarded:
```cpp
(void)journal_q_->try_push(event);  // bounded; drops are never fatal
```

SPSC_RingBuffer (spsc_ring_buffer.hpp line 37–43) silently returns `false` when full. Since `COMPLIANCE_REJECT` events are journaled (execution_engine.hpp line 279), a full journal could cause **compliance rejection events to be silently lost**. There is no overflow counter or error reporting.

**Impact:** Audit trail incompleteness. During high-load periods, compliance rejection events may not be recorded, making forensic analysis difficult.

**Fix:** Track journal drop count and expose it as a metric. Consider a larger journal buffer or a dedicated compliance event log with guaranteed delivery.

---

### LOW-2: check() accepts unbounded string input without length parameter

**Files:** `core/include/compliance_guard.hpp` (lines 64–66)

**Description:**
```cpp
Result check(const char* market_condition_id,
             uint64_t market_hash,
             const char* operator_jurisdiction = nullptr) const noexcept
```

The `market_condition_id` and `operator_jurisdiction` parameters are `const char*` without length parameters. The `check()` method relies on null-termination for these strings. If a caller passes a non-null-terminated string (e.g., a buffer without a trailing `\0`), `std::unordered_set<std::string>::find` would read beyond the buffer — **undefined behavior / potential buffer over-read**.

Current call sites use `MarketConfig` fixed-size arrays (always null-terminated), so this is not immediately exploitable. But the API is unsafe for general use.

**Impact:** Not directly exploitable with current callers, but represents a fragile API that could cause memory safety issues if used differently.

**Fix:** Accept length parameters (`string_view` or explicit `size_t len`) and validate bounds.

---

### LOW-3: No test coverage for parse_restricted edge cases

**Files:** `tests/unit/test_compliance_guard.cpp` (lines 143–165)

**Description:**
The `test_parse_restricted` test covers only valid inputs:
- `"0x1234,0x5678"` — valid condition IDs
- `"42,100"` — valid decimal hashes
- `""` — empty string
- `nullptr` — null pointer

**Not tested:**
- Malformed condition IDs: `"0x"`, `"0xGG"`, `"0x123"` (too short)
- Octal surprises: `"010"` (parsed as octal = 8)
- Empty tokens in CSV: `"0x1234,,0x5678"` (double comma)
- Trailing comma: `"42,100,"`
- Overflow: `"99999999999999999999999999"` (exceeds uint64_t)
- Mixed valid/invalid: `"0xvalid,notanumber"`

**Impact:** Without edge case tests, the parsing bugs described in MEDIUM-2 and MEDIUM-3 could go undetected.

**Fix:** Add comprehensive edge case tests for `parse_restricted`.

---

### LOW-4: Two-factor compliance gate creates split failure modes

**Files:** `core/src/execution_engine.hpp` (lines 184–189, 274)

**Description:**
The compliance check requires two independent conditions:
```cpp
if (compliance_guard_ && cfg_.compliance_enabled) {
```

- `compliance_guard_` is set only when a `ComplianceConfig*` is passed to the constructor (defaults to nullptr)
- `cfg_.compliance_enabled` defaults to `true` in `MarketConfig`

These two conditions can be set independently, creating two failure modes (see HIGH-2). A cleaner design would have a single source of truth: the `ComplianceGuard` itself determines whether it's active.

**Impact:** Increases the surface for misconfiguration. Currently exploited by CRITICAL-1.

**Fix:** Consolidate the two flags. If `compliance_enabled` is true in config, the engine should construct and activate a `ComplianceGuard` internally, failing to start if the config is invalid.

---

## Summary Table

| ID | Severity | File | Lines | Description |
|----|----------|------|-------|-------------|
| CRITICAL-1 | CRITICAL | execution_engine.hpp | 168, 184-189, 274 | Compliance guard never wired in production |
| CRITICAL-2 | CRITICAL | compliance_guard.hpp | 79-84 | Condition ID bypass via empty/null |
| CRITICAL-3 | CRITICAL | market_config.hpp | 73-74, 446-460 | Restriction strings never parsed into config |
| HIGH-1 | HIGH | compliance_guard.hpp | 49-51, 92 | Default constructor is fail-open, not fail-closed |
| HIGH-2 | HIGH | execution_engine.hpp | 184-189, 274 | Opt-in via nullptr — fail-open default |
| HIGH-3 | HIGH | execution_engine.hpp | 729-817 | Protective orders bypass compliance |
| MEDIUM-1 | MEDIUM | compliance_guard.hpp | 73, 80, 87-88 | Non-constant-time comparisons (timing attacks) |
| MEDIUM-2 | MEDIUM | compliance_guard.hpp | 106-109 | parse_restricted accepts malformed condition IDs |
| MEDIUM-3 | MEDIUM | compliance_guard.hpp | 113 | Base-0 numeric parsing (octal surprise) |
| MEDIUM-4 | MEDIUM | compliance_guard.hpp | 96, 109 | noexcept but can trigger std::terminate |
| MEDIUM-5 | MEDIUM | execution_engine.hpp | 573-578 | Weak evidence dedup — adversarial flooding |
| MEDIUM-6 | MEDIUM | test_compliance_guard.cpp | — | No execution_engine compliance integration tests |
| MEDIUM-7 | MEDIUM | test_compliance_guard.cpp | 169-179 | Misnamed/misverified fail-closed test |
| MEDIUM-8 | MEDIUM | compliance_guard.hpp | 70-72 | No operator_jurisdiction format validation |
| MEDIUM-9 | MEDIUM | execution_engine.hpp | 274-282 | Compliance uses config hash, not signal hash |
| LOW-1 | LOW | execution_engine.hpp | 820-831 | Journal silently drops compliance events |
| LOW-2 | LOW | compliance_guard.hpp | 64-66 | Unbounded string input, no length parameter |
| LOW-3 | MEDIUM | test_compliance_guard.cpp | 143-165 | No parse_restricted edge case tests |
| LOW-4 | MEDIUM | execution_engine.hpp | 184-189, 274 | Two-factor compliance gate split failure modes |

| Priority | Action |
|----------|--------|
| **1** | Wire up ComplianceConfig in main_hot_path.cpp — parse MarketConfig restriction strings via parse_restricted() and pass to ExecutionEngine |
| **2** | Make ComplianceConfig mandatory when compliance_enabled=true (remove nullptr default or validate at startup) |
| **3** | Make default ComplianceGuard constructor fail-closed (enable jurisdiction with empty allowlist) |
| **4** | Add compliance check to execute_protective() |
| **5** | Add integration tests for compliance through ExecutionEngine |
| **6** | Validate condition_id format in parse_restricted |
| **7** | Fix base-0 parsing to base-10 |
| **8** | Remove noexcept from parse_restricted or use reserve() |
| **9** | Add overflow tracking to journal |
