# 🔍 VERIFICATION REPORT — Bot_Crowdintel `remediation/compliance`

**Auditor:** DeepSeek Harness (autonomous technical reviewer)  
**Branch:** `remediation/compliance`  
**Commit HEAD:** `e48e68cd22e6e8f65a95633bfd3d1f3a61a2bc99`  
**Date:** 2025-09-25  

---

## SUMMARY

| # | Check | Result |
|---|-------|--------|
| 1 | Clean build from scratch | **PASS** |
| 2 | ctest --output-on-failure | **PASS** |
| 3 | Individual test binaries | **PASS** |
| 4 | Secret scan | **PASS** |
| 5 | Hot path audit | **FAIL** (5 issues) |
| 6 | Risk engine verification | **PASS** |
| 7 | Order lifecycle verification | **PASS** |
| 8 | Compliance verification | **PASS** |
| 9 | Telemetry verification | **PASS** |
| 10 | Documentation audit | **FAIL** (5 issues) |
| 11 | eip712_signer.hpp integrity | **PASS** |
| 12 | Code tags (TODO/FIXME) | **PASS** |

**STATUS: FAIL (10/12 PASS)**

Two audit categories failed: **Step 5 (Hot Path)** and **Step 10 (Documentation)**.
While the codebase compiles cleanly and all tests pass, the hot path contains
allocations and non-O(1) operations that were not disclosed, and the documentation
contains performance claims not supported by measured reality.

---

## Step 1: Clean Build From Scratch — PASS ✅

```
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_DEMO=OFF -DSECP256K1_ROOT=/tmp/secp256k1 ..
make -j$(nproc)
```

- **Warnings:** 0 (compiler warnings: 0)
- **Errors:** 0
- **Targets built:** 13
  - `crowdintel_bot` (production binary)
  - `l2_backtester`
  - `latency_bench`
  - `test_compliance_guard`, `test_fee_model`, `test_order_manager`, `test_position_tracker`, `test_presigned_pool`, `test_rate_limiter`, `test_risk_engine`, `test_signer`, `test_telemetry`

**Precondition:** `libsecp256k1` was not present on the system. It was built
from source (`bitcoin-core/secp256k1` master HEAD) and installed to
`/tmp/secp256k1` with `--enable-module-recovery`. Without this, the build
fails with `fatal error: secp256k1.h: No existe el fichero o el directorio`.

**CMake configure warnings (not compiler warnings):** RPATH conflicts between
conda-provided libcrypto/libssl and system OpenSSL — 10 targets affected.
Cosmetic, but indicates a fragile build environment.

---

## Step 2: ctest — PASS ✅

```
ctest --output-on-failure
100% tests passed, 0 tests failed out of 10
Total Test time (real) =   2.98 sec
```

| # | Test | Status | Time |
|---|------|--------|------|
| 1 | test_compliance_guard | ✅ Passed | 0.04s |
| 2 | test_fee_model | ✅ Passed | 0.01s |
| 3 | test_order_manager | ✅ Passed | 0.02s |
| 4 | test_position_tracker | ✅ Passed | 0.01s |
| 5 | test_presigned_pool | ✅ Passed | 0.01s |
| 6 | test_rate_limiter | ✅ Passed | 0.03s |
| 7 | test_risk_engine | ✅ Passed | 0.01s |
| 8 | test_telemetry | ✅ Passed | 1.18s |
| 9 | keccak_known_answer | ✅ Passed | 0.01s |
| 10 | latency_benchmark | ✅ Passed | 1.67s |

---

## Step 3: Individual Test Binaries — PASS ✅

| Binary | Result | Details |
|--------|--------|---------|
| `test_signer` | ✅ Exit 0 | 20/20 ECDSA signatures valid (v=27); 3/3 Keccak-256 KATs pass |
| `latency_bench` | ✅ Exit 0 | Min=44.3μs, P50=45.273μs, P99=94.452μs (CPU 0.372ns/cycle ≈ 2.69GHz) |
| `test_order_manager` | ✅ Exit 0 | 18 tests, 2062 assertion cases — all pass |
| `test_position_tracker` | ✅ Exit 0 | 14 tests, 38 cases — all pass |
| `test_telemetry` | ✅ Exit 0 | 16 tests, 89 cases — all pass; secrets-sanitization verified |
| `test_rate_limiter` | ✅ Exit 0 | 6/6 tests pass |
| `test_compliance_guard` | ✅ Exit 0 | 9/9 tests pass |
| `test_fee_model` | ✅ Exit 0 | 22/22 cases pass |
| `test_risk_engine` | ✅ Exit 0 | 12 tests, 34 cases — all pass |
| `test_presigned_pool` | ✅ Exit 0 | 10 tests, 28 cases — all pass |

---

## Step 4: Secret Scan — PASS ✅

```
grep -rn "YOUR_API_KEY\|YOUR_SECRET\|private_key.*=.*hex\|0x[0-9a-f]\{64\}\|api_key.*=.*['\"]" core/ tests/ --include="*.cpp" --include="*.hpp"
```
**Results:** 2 matches — both are `std::getenv("CLOB_API_KEY")` calls in `main_hot_path.cpp:26`
and `main_prod.cpp:56`. These are legitimate env-var reads, NOT hardcoded secrets.

```
grep -rn "seed_phrase\|mnemonic" core/ tests/ --include="*.cpp" --include="*.hpp"
```
**Results:** 4 matches in `tests/test_telemetry.cpp` lines 627–630 — these are TEST
ASSERTIONS verifying that logs do NOT contain "seed_phrase" or "mnemonic". ✅ Positive control.

No hardcoded secrets, private keys, or seed phrases found in source/test code.

---

## Step 5: Hot Path Audit — FAIL ❌

### 5a. `std::cout` search in hot path files

Searched: `execution_engine.cpp`, `risk_engine.hpp`, `order_manager.hpp`,
`fee_model.hpp`, `compliance_guard.hpp`, `market_metadata.hpp`,
`presigned_pool.hpp`, `telemetry.hpp`

| File | std::cout | Other I/O |
|------|-----------|-----------|
| `execution_engine.cpp` | None in code (comment at line 18 only) | `sprintf` at line 356 (in `build_order_payload`) |
| `risk_engine.hpp` | None | None |
| `order_manager.hpp` | None | `snprintf` at line 82 (in `generate_client_order_id`) |
| `fee_model.hpp` | None | None |
| `compliance_guard.hpp` | None | None |
| `market_metadata.hpp` | None | None |
| `presigned_pool.hpp` | None | None |
| `telemetry.hpp` | None | `std::cerr` at line 118 (constructor only — cold path) |

**Verdict on std::cout:** ✅ No `std::cout` in hot path files.  
**But:** `sprintf` (line 356) and `snprintf` (line 82) ARE in functions called from
the hot path (`build_order_payload` and `generate_client_order_id` respectively).

### 5b. `new`, `malloc`, `std::string` in `run_tick()` BEFORE signing (lines 100–201)

| Line | Code | Issue |
|------|------|-------|
| 110 | `std::string market_slug(signal->market_slug);` | **std::string heap allocation** before compliance check |
| 111 | `std::string country_code = get_operator_jurisdiction();` | **std::string allocation + `getenv()` call** before compliance check |
| 161 | `market_cache_.get_tick_size(market_slug)` | Acquires `shared_mutex` lock for metadata lookup |
| 185 | `risk_engine_.pre_trade_check(...)` | Internally loops 64 entries (see 5d) |
| 194 | `order_mgr_.has_open_order(market_slug, ...)` | **O(N) scan** over all orders (see 5d) |

### 5c. `new`, `malloc`, `std::string` in `run_tick()` AFTER signing (lines 201–274)

| Line | Code | Issue |
|------|------|-------|
| 207 | `final_order.payload = build_order_payload(params);` | `build_order_payload` (line 341) uses `std::string` concatenation with `+=` and `std::to_string` — multiple heap allocations |
| 213 | `std::string client_order_id = order_mgr_.register_order(...)` | `register_order` → `generate_client_order_id` uses `snprintf` + `std::string` allocation |
| 216 | `client_.submit_order_with_response(final_order)` | **NETWORK I/O** on hot path (HTTP request to Polymarket CLOB) |
| 236–237 | `telemetry_.log_event(...)` | Builds JSON string via concatenation, pushes to SPSC (spawns TelemetryEvent with 3× std::string) |

### 5d. Non-O(1) operations in hot path

| Location | Operation | Complexity |
|----------|-----------|------------|
| `risk_engine.hpp:253-283` (`check_rate_window`) | Loops over `WINDOW_SIZE=64` entries twice (orders + cancels) | **O(64)** — code comment admits: "This is O(WINDOW_SIZE)" |
| `order_manager.hpp:154-164` (`has_open_order`) | Iterates over ALL orders in `orders_` map | **O(N)** — N = total registered orders |
| `market_metadata.hpp:88-95` (`get`) | Acquires `shared_mutex` lock + `unordered_map` lookup | O(1) hash lookup but **mutex contention** risk |
| `market_metadata.hpp:110-116` (`is_market_tradable`) | Calls `get()` which acquires `shared_mutex` | O(1) but **mutex lock** in hot path |
| `eip712_signer.hpp:166-170` (`eip712_domain_separator`) | `std::vector<uint8_t> combined` — heap allocation | **std::vector allocation** in signing path |
| `eip712_signer.hpp:207-210` (`eip712_order_struct_hash`) | `std::vector<uint8_t> combined` — heap allocation | **std::vector allocation** in signing path |

### 5e. Telemetry `log_event` allocations

`telemetry.hpp:141-162` — `log_event()` constructs a `TelemetryEvent` containing
3 `std::string` members (`market_slug`, `details_json`, `severity`). The `try_push`
copies these into the ring buffer via placement new of `T(item)`. Each hot-path
call to `log_event` involves 3 heap allocations for the std::string members.

**Hot-path audit verdict:**
- ✅ No `std::cout` in hot path files
- ❌ `std::string` allocations BEFORE signing (lines 110, 111)
- ❌ `sprintf`/`snprintf` in post-signing hot path (lines 356, 82)
- ❌ `std::vector<uint8_t>` allocation in EIP-712 signing path (eip712_signer.hpp lines 166, 207)
- ❌ O(N) `has_open_order` scan before signing (line 194)
- ❌ O(64) `check_rate_window` loop before signing (risk_engine.hpp:253)
- ❌ Network I/O in `run_tick()` (line 216 — `submit_order_with_response`)
- ❌ Mutex `shared_lock` acquisitions in hot path (market_metadata.hpp `get()` calls)

---

## Step 6: Risk Engine Verification — PASS ✅

| Check | Location | Result |
|-------|----------|--------|
| Kill switch BEFORE signing | `execution_engine.cpp:122` (kill switch check), `:201` (signing) | ✅ Kill switch checked 80 lines before signing |
| Kill switch also in `pre_trade_check` | `risk_engine.hpp:54-57` | ✅ Double-protected |
| All risk limits env-driven | `RiskConfig::load_from_env()` in `market_config.hpp:54-114` | ✅ 17 env vars with conservative defaults |
| External signals don't touch kill switch | `risk_engine.hpp:184-192` (`activate_kill_switch()` only called from `add_loss()`, `record_reject()`, `check_position_divergence()`) | ✅ Only internal risk conditions trigger |
| `balance_checker.hpp` / `ws_market_listener.hpp` | No references to `activate_kill_switch` | ✅ Cold-path components don't touch kill switch |

**Kill switch control locations (exact lines):**
1. `risk_engine.hpp:55` — `__builtin_expect(kill_switch_.load(...), 0)` in `pre_trade_check()`
2. `execution_engine.cpp:122` — `if (risk_engine_.is_kill_switch_active())` in `run_tick()`
3. `risk_engine.hpp:68-69` — Daily loss threshold triggers `activate_kill_switch()`
4. `risk_engine.hpp:165-166` — Consecutive rejects triggers `activate_kill_switch()`
5. `risk_engine.hpp:138-140` — Daily loss check in `add_loss()`
6. `risk_engine.hpp:209-214` — Position divergence triggers `activate_kill_switch()`

---

## Step 7: Order Lifecycle Verification — PASS ✅

| Requirement | Location | Status |
|-------------|----------|--------|
| `register_order` returns `client_order_id` | `order_manager.hpp:94-112` returns `client_order_id` | ✅ Line 213 of execution_engine.cpp captures the return value |
| `should_retry` called on nullopt (no response) | `execution_engine.cpp:218-230` | ✅ Line 221: `order_mgr_.should_retry(client_order_id)` |
| `should_retry` called on 429 | `execution_engine.cpp:240-249` | ✅ Line 242: `order_mgr_.should_retry(client_order_id)` |
| `should_retry` called on 5xx | `execution_engine.cpp:250-264` | ✅ Line 252: `order_mgr_.should_retry(client_order_id)` |
| `update_status` called after submit (success) | `execution_engine.cpp:235` | ✅ `order_mgr_.update_status(client_order_id, OrderStatus::OPEN)` |
| `update_status` called after submit (rejected) | `execution_engine.cpp:269` | ✅ `order_mgr_.update_status(client_order_id, OrderStatus::REJECTED)` |

**Order lifecycle sequence (exact lines):**
1. Line 213: `register_order()` → returns `client_order_id`, sets status PENDING
2. Line 216: `submit_order_with_response()` → HTTP call
3. Line 218-230: Handle nullopt → `should_retry()` at line 221
4. Line 232-239: 2xx → `update_status(OPEN)` at line 235
5. Line 240-249: 429 → `should_retry()` at line 242
6. Line 250-264: 5xx → `should_retry()` at line 252
7. Line 265-273: else → `update_status(REJECTED)` at line 269

---

## Step 8: Compliance Verification — PASS ✅

| Check | Location | Status |
|-------|----------|--------|
| `apply_tick_size` is used | `execution_engine.cpp:161-162` | ✅ `params.price = MarketMetadataCache::apply_tick_size(best_ask.price, tick_size)` |
| `is_market_tradable` called before signing | `compliance_guard.hpp:167` (called from `check_all` at line 153-169), invoked from `execution_engine.cpp:113` | ✅ Called at line 113, signing at line 201 |
| Fail-closed: unknown market | `market_metadata.hpp:112` | ✅ Returns `false` if `meta == nullptr` |
| Fail-closed: empty jurisdiction + check enabled | `compliance_guard.hpp:131-135` | ✅ Returns `false` if `allowed_jurisdictions.empty()` and check enabled |
| Fail-closed: restricted token | `compliance_guard.hpp:109-116` | ✅ Returns `false` if token in restricted set |

**Compliance check order in `run_tick()` (all before signing at line 201):**
1. Line 109–119: `ComplianceGuard::check_all` — token blocklist, jurisdiction, market active
2. Line 121–125: Kill switch check
3. Line 127–131: Kelly position sizing
4. Line 133–151: Net EV computation (FeeModel)
5. Line 160–165: Tick size application
6. Line 167–191: RiskEngine::pre_trade_check (price deviation + risk limits)
7. Line 193–197: Self-trade detection

---

## Step 9: Telemetry Verification — PASS ✅

| Check | Location | Status |
|-------|----------|--------|
| `SPSC_RingBuffer<8192>` in hot path | `telemetry.hpp:272-278` | ✅ `QUEUE_CAPACITY = 8192`, `SPSC_RingBuffer<TelemetryEvent, QUEUE_CAPACITY>` |
| JSON Lines output | `telemetry.hpp:322-350` (`write_event`) | ✅ Each event written as `{"ts":...,"type":"...","market":"...","severity":"...","details":...}\n` |
| `std::ios::app` | `telemetry.hpp:115` | ✅ `log_stream_.open(log_file_, std::ios::app \| std::ios::out)` |
| No secrets in logs | `telemetry.hpp:166-184` (`log_risk_block`) | ✅ Only logs `order_size` + `reason`; test_telemetry confirms no private_key, api_key, signature, seed_phrase, mnemonic in logs |
| `AlertConfig::load_from_env` with 6 env vars | `telemetry.hpp:79-107` | ✅ 6 env vars: `TELEMETRY_DAILY_LOSS_THRESHOLD_USD`, `TELEMETRY_CONSECUTIVE_429_THRESHOLD`, `TELEMETRY_LATENCY_SPIKE_US`, `TELEMETRY_POSITION_DIVERGENCE_THRESHOLD`, `TELEMETRY_FEED_DEAD_MS`, `TELEMETRY_WEBHOOK_URL` |

**Caveat:** The `TelemetryEvent` struct (line 41-49) contains 3 `std::string` members,
and `log_event()` (line 141) copies these onto the stack before pushing to SPSC.
This means each hot-path telemetry call involves heap allocations for the std::string
members. The SPSC push itself is O(1), but the event construction is not zero-alloc.

---

## Step 10: Documentation Audit — FAIL ❌

### 10a. AI Agent References in README.md and docs/*.md

Search terms: "agente de IA", "agentes de IA", "DeepShe", "AGENTE_",
"workflow.*agent", "team.*agent"

**Result in README.md and docs/*.md:** None found. ✅

**NOTE — Broader scope (outside audit instructions):** The `team/` and
`team_remediation/` directories (NOT in README.md or docs/*.md per audit spec)
contain extensive AI-agent team infrastructure including `TEAM_ROSTER.md`,
`team_manifest.json`, `llm_assignments.yaml`, `agent_team_workflow.yaml`,
and per-agent prompt directories (`team/agents/`, `team_remediation/agents/`).
These were partially sanitized in commit `57f6136` ("remove AI agent references").
The `AUDITOR_VERDICT.json` (root-level, not in docs/) still references the
baseline audit findings and contains no AI-agent references itself.

### 10b. MutaLambda References

Found in README.md (line 27) and docs/OPTIMIZATION_LINEAGE.md (lines 7-11, 42, 51-71).

| Location | Content | Assessment |
|----------|---------|------------|
| README.md:27 | `infra/mutualambda/` in project tree | ✅ Appropriate — directory structure reference |
| OPTIMIZATION_LINEAGE.md:7-11 | Describes MutaLambda as evolutionary optimizer framework | ✅ Appropriate — descriptive |
| OPTIMIZATION_LINEAGE.md:15 | "integration is wired but **not yet executed**" | ✅ Appropriate — honest status |
| OPTIMIZATION_LINEAGE.md:18 | "no fabricated numbers" | ✅ Appropriate — transparency |
| latency_bench.cpp:123 | "FINAL LATENCY RESULTS (Post-MutaLambda + Warmup)" | ❌ MISLEADING — MutaLambda was never executed; the benchmark uses `BenchExecutionEngine`, not the real `ExecutionEngine` |

### 10c. Fabricated Performance Figures

| Claim Location | Claimed Value | Measured Value | Discrepancy |
|---------------|---------------|----------------|-------------|
| README.md:108 | P99 = 52 μs | P99 = 94.452 μs | ❌ **47% understated** |
| README.md:107 | P50 = 47 μs | P50 = 45.273 μs | ✅ Close (within 5%) |
| README.md:81-82 | "~33× speedup" table | — | Speedup ratio, not throughput; table shows "25×" for sign+recover — internally inconsistent (33× vs 25×) |
| PERF_METRICS.md:49 | "100,000 simulated ticks" | — | ✅ Methodology description, not throughput claim |
| README.md:60-62 | `export CLOB_API_KEY="your_key"` etc. | — | ✅ Placeholders in documentation (not "YOUR_API_KEY") |

**README.md P99 discrepancy:** The README table (line 108) states P99 = 52 μs,
but the actual `latency_bench` binary measures P99 = 94.452 μs. The
`docs/PERF_METRICS.md` (line 31) acknowledges "P99 ~101.8 μs" in the baseline,
and `team_remediation/verification/AUDIT_FINAL_REPORT.md` (line 72) says
"Línea base: P50 ≈ 47 µs, P99 ≈ 52-90 µs". The README's P99=52μs is at the low
end of the acknowledged range and is not representative of measured reality.

### 10d. Hardcoded API Key Placeholder

README.md lines 59-62 use lowercase placeholders (`your_key`, `your_secret`,
`your_passphrase`). No `YOUR_API_KEY` (uppercase) found. ✅
However:
- README.md:59: `export BOT_PRIVATE_KEY_HEX=$(python3 -c "print('AA'*32)")` —
  generates all-0xAA private key as documentation example. While a placeholder,
  0xAA*32 is a well-known insecure test key pattern.
- `bench_engine.hpp:21` uses hardcoded `std::vector<uint8_t>(32, 0xAA)` —
  this is in source code (benchmark file), not documentation.

### 10e. Fabricated/Missing Throughput Claims

No explicit "X orders/sec" or "X req/s" throughput claims found in README.md
or docs/*.md. ✅
The `docs/PERF_METRICS.md` line 54 says "Max Orders/sec: Sujeto a rate limits
publicados de Polymarket CLOB (no se hardcodea throughput)" — accurate. ✅

### 10f. Documentation vs. Audit Report Discrepancies

The `AUDIT_FINAL_REPORT.md` (in `team_remediation/verification/`) makes claims
that my independent verification contradicts:

| AUDIT_FINAL_REPORT.md claim | My verification finding |
|------------------------------|------------------------|
| Line 72: "P50: 41.693 µs" | My run: 45.273 µs (8.7% higher) |
| Line 81: "P99: 60.607 µs" | My run: 94.452 µs (55% higher) |
| Line 83: "P50: 41.693 µs ≤ 51.7 µs ✅" | Measured P50 is within threshold, but P99 is NOT reported |
| Line 132: "todos checks O(1)" | `has_open_order` is O(N), `check_rate_window` is O(64) |
| Line 132: "0 alloc en hot path" | std::string at lines 110, 111; std::vector in signer; sprintf/snprintf |
| Line 158: lists rate window as O(1) | Code loop iterates 64 entries (risk_engine.hpp:263) |
| Line 166: "O(N_orders) but early-exit" | Acknowledges O(N) — contradicts "todos checks O(1)" |
| Line 147: "SPSC_RingBuffer<8192> en hot path ✅" | Telemetry uses 8192, but alpha queue in bench uses 4096 |
| Line 7: "Commit base: 4686ec2" | Actual HEAD: e48e68c (10 commits ahead of baseline) |

---

## Step 11: eip712_signer.hpp Integrity — PASS ✅

```
git diff --stat HEAD -- core/crypto/eip712_signer.hpp
```
Output: **(empty)** — no changes. ✅

- **File:** `core/crypto/eip712_signer.hpp`
- **Commit:** `e48e68cd22e6e8f65a95633bfd3d1f3a61a2bc99` (HEAD of `remediation/compliance`)
- **Hash (tree):** `d2737427e26f564ec8b461f5459bb059249148bb`
- **Status:** UNMODIFIED from `HEAD` — KAT vectors (Keccak-256, EIP-712) pass.

---

## Step 12: Code Tags Audit (TODO/FIXME/HACK/XXX/SKELETON/STUB) — PASS ✅

```
grep -rn "TODO\|FIXME\|HACK\|XXX\|SKELETON\|STUB" core/ tests/ --include="*.cpp" --include="*.hpp"
```
**Result:** 0 matches. ✅ No TODO, FIXME, HACK, XXX, SKELETON, or STUB tags
found in source or test code.

---

## Issues Found

### Critical Issues (Hot Path)

1. **`execution_engine.cpp:110`** — `std::string market_slug(signal->market_slug);`
   heap-allocates a copy of the market slug on the hot path BEFORE signing.
   This contradicts the file's own comment "NO std::cout / I/O in hot path"
   and the AUDIT_FINAL_REPORT's claim of "0 alloc en hot path."

2. **`execution_engine.cpp:111`** — `std::string country_code = get_operator_jurisdiction();`
   heap-allocates a string AND calls `std::getenv("OPERATOR_JURISDICTION")` on
   every tick BEFORE signing. getenv is NOT thread-safe and involves a syscall.

3. **`execution_engine.cpp:216`** — `client_.submit_order_with_response(final_order)`
   performs **network I/O** (HTTP POST to Polymarket CLOB) inside `run_tick()`.
   While this is after signing, it is in the hot path function and involves
   potential seconds-long blocking under network contention.

4. **`risk_engine.hpp:253-283`** — `check_rate_window()` loops over 64 entries
   (two loops of 64) called from `pre_trade_check()` BEFORE signing. The code
   explicitly comments "This is O(WINDOW_SIZE)" — it is O(64), not O(1).
   The AUDIT_FINAL_REPORT incorrectly lists this as O(1).

5. **`order_manager.hpp:154-164`** — `has_open_order()` iterates over ALL orders
   in `orders_` map (O(N)). Called from `run_tick()` line 194 BEFORE signing.
   The AUDIT_FINAL_REPORT acknowledges this at line 166 ("O(N_orders)") but
   contradicts it at line 132 ("todos checks O(1)").

### High Issues (Hot Path)

6. **`eip712_signer.hpp:166`** and **`:207`** — `eip712_domain_separator()` and
   `eip712_order_struct_hash()` both allocate `std::vector<uint8_t> combined`
   on the stack-heap. These functions are called from `sign_order()` which IS
   the hot path signing operation. Two heap allocations per signature.

7. **`execution_engine.cpp:341-361`** (`build_order_payload`) — Uses `std::string`
   with `+=` and `std::to_string` (6 calls) plus `sprintf` for hex encoding.
   Called at line 207 AFTER signing but still in `run_tick()`.

8. **`order_manager.hpp:76-87`** (`generate_client_order_id`) — Uses
   `snprintf` + `std::string` allocation. Called from `register_order()` at
   line 213 AFTER signing.

9. **`telemetry.hpp:141-162`** (`log_event`) — Constructs `TelemetryEvent` with
   3 `std::string` members, copies them into the SPSC buffer via the `T(item)`
   copy constructor in `try_push` (line 62 of spsc_ring_buffer.hpp). Each hot-path
   `log_event` call involves 3 heap allocations for std::string members.

### Medium Issues (Documentation)

10. **`README.md:108`** — Claims P99 = 52 μs, but actual measured P99 = 94.452 μs
    (47% understating). The README table presents this as a measured result
    ("Value (cycles @ 2.7GHz)") but no actual benchmark supports it.

11. **`README.md:27`** — References `infra/mutualambda/` in the project tree.
    The directory exists but the `optimizalambda_optimize.py` script is
    referenced in PROJECT_ANALYSIS.md as a "fake mock script" that should
    be deleted. (PROJECT_ANALYSIS.md is outside documentation audit scope
    but reflects a known code-quality issue.)

12. **`README.md:59`** — `export BOT_PRIVATE_KEY_HEX=$(python3 -c "print('AA'*32)")`
    uses the all-0xAA test private key as a documented example. While labeled
    as a demonstration, this key is trivially guessable and should not be
    presented as a realistic example.

13. **`bench_engine.hpp:21`** — Hardcoded private key `0xAA*32` and
    **`bench_engine.hpp:32`** — Hardcoded salt `0xCAFEBABE` in the benchmark
    engine source code. While benchmark-only (not in production binary),
    these are hardcoded cryptographic secrets in source.

14. **`latency_bench.cpp:123`** — Output header says "Post-MutaLambda + Warmup"
    but MutaLambda was never executed (per OPTIMIZATION_LINEAGE.md line 15:
    "not yet executed"). The benchmark measures `BenchExecutionEngine` which
    omits ALL compliance/risk controls — it does not measure the production
    hot path.

### Low Issues

15. **`AUDITOR_VERDICT.json`** — Dated `2025-09-25` verification date but the
    file content describes the BASELINE (pre-remediation) state. The file
    should be updated with the post-remediation verdict or clearly labeled
    as the baseline audit.

16. **`execution_engine.cpp:157-158`** — `memset(params.maker, 0x00, 20)` and
    `memset(params.taker, 0x00, 20)` — zero addresses are hardcoded for
    maker/taker. The comment says "should come from config in production"
    but they don't.

17. **`execution_engine.cpp:168-169`** — `cached_usdc_balance_` and
    `cached_pol_balance_` are initialized to fixed defaults (10000.0, 1000.0).
    If `BalanceChecker` is not started, the hot path uses stale defaults.

---

## Recommended Fixes

### Immediate (must fix before any production use)

1. **Eliminate std::string allocations before signing in `run_tick()`**
   - Replace `std::string market_slug(signal->market_slug)` with a `std::string_view`
     (C++20) or pass the raw `const char*` to `check_all()` / `get_tick_size()`.
   - Replace `get_operator_jurisdiction()` (which calls `getenv()` per tick) with
     a cached `std::string` populated once in the constructor.
   - Move `market_cache_.get_tick_size()` and `is_market_tradable()` lookups
     to use the signal's `const char* market_slug` directly, avoiding the copy.

2. **Remove heap allocations from EIP-712 signing path**
   - In `eip712_domain_separator()`: replace `std::vector<uint8_t> combined`
     with a stack-allocated `uint8_t combined[32 + MAX_DOMAIN_DATA]`.
   - In `eip712_order_struct_hash()`: the `std::vector` is unnecessary — use
     a stack buffer or inline the hash.

3. **Fix `check_rate_window` O(64) → true O(1)**
   - Replace the 2×64-element scan with a monotonic atomic counter that tracks
     orders in the current second, resetting once per second. Or use a
     `std::atomic<uint32_t>` sliding-window counter with a 1-second epoch.

4. **Fix `has_open_order` O(N) → O(1)**
   - Maintain a secondary index: `unordered_map<std::pair<market_slug, side>, bool>`
     or a small fixed-size set keyed by (market_slug, side).

5. **Move network I/O out of `run_tick()`**
   - Either (a) push the signed order to a submit queue consumed by a separate
     thread, or (b) restructure so `run_tick()` returns after signing and the
     caller handles submission asynchronously.

### Documentation fixes

6. **`/home/adlg/.../README.md:108`** — Correct P99 from 52 μs to the actual
   measured value (~94 μs) or remove the specific P99 claim and reference
   `docs/PERF_METRICS.md` which already carries the correct caveat.

7. **`/home/adlg/.../README.md:59`** — Replace the all-0xAA private key example
   with a clearly-labeled "DO NOT USE — generate a real key" warning and omit
   the specific key value.

8. **`latency_bench.cpp:123`** — Remove "Post-MutaLambda" from the output
   header since MutaLambda was never executed. Use "Post-Warmup" instead.

9. **`AUDIT_FINAL_REPORT.md`** — Update the latency numbers (P50=41.693, P99=60.607)
   to match actual measured values, or note the hardware/environment dependency.

10. **`bench_engine.hpp:21`** and **`:32`** — Replace hardcoded `0xAA*32`
    private key and `0xCAFEBABE` salt with a clearly-labeled test fixture
    comment, or parameterize via a constant.

### Verification alignment

11. The AUDIT_FINAL_REPORT.md (Nivel 6, line 132) claims "todos checks O(1)" but
    the same document (line 166) acknowledges `has_open_order` is O(N). The
    report should accurately characterize the O(64) rate-window check and the
    O(N) self-trade scan as non-O(1) with justification, rather than claiming
    they are O(1).

12. The `docs/ARCHITECTURE.md` (line 18) claims "EIP-712 Signing using AVX2/SIMD"
    but the actual implementation uses libsecp256k1 (no AVX2/SIMD). This claim
    should be corrected.

---

## Per-Check Summary

| Check | PASS/FAIL | Notes |
|-------|-----------|-------|
| 1 | PASS | 0 compiler warnings, 0 errors, 13 targets |
| 2 | PASS | 10/10 tests pass, 2.98s total |
| 3 | PASS | All 10 binaries pass individually |
| 4 | PASS | No hardcoded secrets (only getenv calls) |
| 5 | **FAIL** | 5 critical, 4 high issues — std::string, sprintf, network I/O, O(N), O(64) in hot path |
| 6 | PASS | Kill switch before signing, env-driven limits, no external kill-switch access |
| 7 | PASS | register_order returns ID, should_retry on 429/5xx/nullopt, update_status after submit |
| 8 | PASS | apply_tick_size used, is_market_tradable before signing, fail-closed confirmed |
| 9 | PASS | SPSC<8192>, JSON Lines + ios::app, 6 AlertConfig env vars, no secrets in logs |
| 10 | **FAIL** | P99 misreported (52μs vs 94μs actual), "Post-MutaLambda" misleading, AI-agent dirs outside scope |
| 11 | PASS | git diff --stat empty; file unmodified from HEAD |
| 12 | PASS | No TODO/FIXME/HACK/XXX/SKELETON/STUB tags |

---

## Final Status

**STATUS: FAIL (10/12 PASS)**

While the remediation branch represents significant improvement over the baseline
(the build compiles, all tests pass, crypto is verified, and secrets are externalized),
two verification steps FAILED:

- **Step 5 (Hot Path):** The code itself and the accompanying AUDIT_FINAL_REPORT
  both claim "0 allocations in hot path" and "all checks O(1)", but the code
  contains `std::string` allocations before signing (lines 110, 111),
  `std::vector` allocations in the EIP-712 signing path, `sprintf`/`snprintf`
  in post-signing hot path, network I/O in `run_tick()`, an O(N) order scan,
  and an O(64) rate-window check. These are not disclosed as limitations.

- **Step 10 (Documentation):** The README states P99=52μs but the actual
  benchmark measures 94.5μs — a 47% discrepancy. Additionally, the latency
  benchmark header claims "Post-MutaLambda" optimization that was never executed,
  and the benchmark measures a stripped-down `BenchExecutionEngine` that omits
  all compliance/risk controls present in the production `ExecutionEngine`.

The bot is NOT ready for production with funds until the hot-path allocations
and non-O(1) operations are addressed, and the documentation is corrected to
reflect measured reality rather than aspirational claims.
