# 🔥 Hot Path Audit Report — Detailed Findings

> **Author**: AGENTE_AUDITOR (independent verification)  
> **Date**: 2025-01-23  
> **Repo**: `Adlgr87/Bot_Crowdintel` (rama `remediation/compliance`)  
> **Scope**: Hot path determinism, zero-allocation claims, build integrity, cryptographic correctness, documentation accuracy.  
> **Verdict**: **CRITICAL FAIL — NOT APPROVED FOR PRODUCTION**

---

## Summary

| Category | Count | Status |
|---|---|---|
| ✅ Resolved (from prior audit) | 3 | SPSC async submit, O(1) rate window (current bucket), EIP-712 Keccak-256 KAT |
| ❌ NOT Actually Fixed | 12 | Still present in current code |
| 📝 Documented Constraint | 1 | `eip712_signer.hpp` `std::vector` alloc (NEVER MODIFY) |
| ⚠️ Partially Fixed | 3 | Some allocations moved, but hot path still allocates |
| 🆕 Newly Discovered Critical | 5 | Build-breaking, deadlock, domain separator, index bug, payload truncation |

---

## 🔴 CRITICAL — Build Integrity

### Issue 1 (New): Missing `transparent_string_hash.hpp`
**File**: `core/include/order_manager.hpp:17`
**Code**: `#include "transparent_string_hash.hpp"`

**Problem**: This header is `#include`d but **does not exist anywhere** in the repository. The file is absent from all directories (verified via `find`).

**Impact**: Compilation fails for every target that transitively includes `order_manager.hpp`, including `execution_engine.cpp` and `test_order_manager.cpp`. This is a **build-breaking** issue.

**Documentation claim**: `order_manager.hpp:301` says "transparent_string_hash.hpp provides string_view support." This file was never created.

---

### Issue 2 (New): Missing `register_order_no_alloc` Method
**File**: `core/src/execution_engine.cpp:281`
**Code**: `order_mgr_.register_order_no_alloc(params, params.nonce, market_slug, client_order_id_buf);`

**Problem**: This method is **not declared** in the `OrderManager` class. The class only defines `register_order(const OrderParams&, uint64_t, const std::string&)` at `order_manager.hpp:95`. No method named `register_order_no_alloc` exists with any signature.

**Impact**: Compilation fails with "no member named `register_order_no_alloc`."

**Documentation claims**: `README.md:167`, `VERIFICATION_FINAL.md:124,202` all reference this method. These are **false claims**.

---

### Issue 3 (New): CMakeLists.txt Lacks `find_package`
**File**: `core/CMakeLists.txt`

**Problem**: The CMake configuration performs **no `find_package`** for libcurl, OpenSSL, or libsecp256k1, and has **no `target_link_libraries`** invocation. Source files `#include <curl/curl.h>` and `<secp256k1.h>` but the build never locates or links these libraries.

**Impact**: Clean build fails with `fatal error: curl/curl.h: No such file or directory`. CI workflow (`.github/workflows/ci-cd-and-optimize.yml`) installs only `build-essential`, `cmake`, `clang` — no curl/SSL dev headers.

**Cross-reference**: `AUDITOR_VERDICT.json` (prior audit) identified the same issue. It was never resolved.

---

### Issue 4 (New): Dockerfile COPY Target Mismatch
**File**: `infra/docker/Dockerfile.prod:53`
**Code**: `COPY --from=builder /app/build/bot_bin .`

**Problem**: CMake emits `bin/crowdintel_bot`, not `bot_bin`. The Dockerfile references a non-existent path.

**Impact**: Docker build fails at the final stage. Also missing `libcurl4-openssl-dev` in the runtime image.

---

### Issue 5 (New): Test Files Not Registered as CMake Targets
**File**: `core/CMakeLists.txt`

**Problem**: `test_signer.cpp`, `latency_bench.cpp`, `l2_backtester.cpp`, and `test_order_manager.cpp` are **never compiled** by `make`. The glob only covers `src/*.cpp + ../alpha/crowdintel/*.cpp`.

**Impact**: CI cannot verify crypto KAT vectors or hot path correctness. The "test suite" referenced in `README.md:262-266` does not run.

---

## 🔴 CRITICAL — Self-Trade Detection Broken

### Issue 6 (New): Secondary Index Never Populated on Registration
**File**: `core/include/order_manager.hpp:95-113` (`register_order`)

**Problem**: `register_order()` adds a new `ManagedOrder` to `orders_` but **never calls `update_index()`** to increment `open_order_index_`. Only `update_status()` (line 127-128) calls `update_index()` when an order transitions from open to terminal.

**Consequence**: The `open_order_index_` map is **always empty**. The `has_open_order()` method:
```cpp
bool has_open_order(std::string_view market_slug, bool is_buy) const {
    std::string idx_key = make_index_key(market_slug, is_buy ? 0 : 1);
    auto it = open_order_index_.find(idx_key);
    if (it != open_order_index_.end()) {
        return it->second > 0;  // ← NEVER REACHED (index is always empty)
    }
    // Fallback: O(N) linear scan — this is what actually runs
    for (const auto& [id, order] : orders_) { ... }
}
```

**Impact**: Self-trade prevention is **completely broken** — existing open orders are never detected. The "O(1) secondary index" optimization is dead code. Self-trading and duplicate order submission are not prevented.

---

## 🔴 CRITICAL — Cryptographic Correctness

### Issue 7 (New): EIP-712 Domain Separator Is All Zeros in Production
**File**: `core/crypto/eip712_signer.hpp:251-257`

**Code** (constructor):
```cpp
if (!domain_data.empty()) {
    eip712_domain_separator(eip712_domain_type, domain_data, domain_separator_);
} else {
    memset(domain_separator_, 0, 32);  // ← ZEROS
}
```

**Problem**: `ExecutionEngine` constructor (`execution_engine.cpp:95,131`) calls `signer_(load_private_key())` or `signer_(private_key)` — **never passing domain type or domain data**. So `domain_separator_` is **all zeros**.

**Impact**: All EIP-712 signatures are computed with a zero domain separator, which does **not match** Polymarket CLOB V2's domain. Every signature will **FAIL verification** on the exchange. All orders will be rejected silently.

**Note**: `test_signer.cpp:118` correctly passes domain data, but production code does not.

---

### Issue 8 (New): Order Payload JSON Truncated
**File**: `core/src/execution_engine.cpp:538`
**Code**: `append_str("\",\"exp\":\"0\",\"t\":0}", 14);`

**Problem**: The C++ string literal `","exp":"0","t":0}` is **18 bytes**, but `append_str` is called with length **14**. Only 14 bytes are `memcpy`'d, truncating the closing `"0}`.

**Truncated payload** ends with: `...,"exp":"0","t` (unterminated JSON string, missing `:0}"`)

**Impact**: The exchange API will reject the order payload as malformed JSON. This affects **every submitted order**.

---

## 🔴 CRITICAL — Deadlock on Shutdown

### Issue 9 (New): `process_submit_queue()` Infinite Loop With No Exit
**File**: `core/src/execution_engine.cpp:395-449`

**Code**:
```cpp
void process_submit_queue() {
    while (true) {                    // ← no exit condition
        auto task = submit_queue_.try_pop();
        if (!task) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            continue;
        }
        // ... process task ...
    }
}
```

**Problem**: The background thread loop has **no shutdown check**. The destructor calls `submit_thread_.join()` (line 138-139), which **blocks forever** because the thread never exits.

**Impact**: **Deadlock on shutdown.** The program hangs on `SIGINT`/`SIGTERM`. No graceful shutdown possible.

**Fix**: Add `std::atomic<bool> running_` flag; check in loop; set false in destructor before join.

---

## 🔴 CRITICAL — Hot Path Allocations After Remediation Claims

### Issue 10: EIP-712 Struct Hash Allocates `std::vector` in Hot Path
**File**: `core/crypto/eip712_signer.hpp:207-211`

**Code**:
```cpp
std::vector<uint8_t> combined;
combined.reserve(32 + ENCODED_SIZE);
combined.insert(combined.end(), type_hash, type_hash + 32);
combined.insert(combined.end(), encoded, encoded + ENCODED_SIZE);
```

**Problem**: `eip712_order_struct_hash()` allocates a `std::vector` on the heap. This is called from `EIP712Signer::sign_order()`, invoked from `ExecutionEngine::run_tick()` at line 268.

**Documentation claim**: `README.md:114-117` states "these allocations occur once per order (not per tick) and are in the cold/signing path, not the hot path." This is **false** — `sign_order` is called directly from `run_tick()`, which IS the hot path.

**Constraint**: `eip712_signer.hpp` is marked "NEVER MODIFY." See Issue 16 below for the documented constraint status.

---

### Issue 11: `task.order.payload.assign()` Allocates
**File**: `core/src/execution_engine.cpp:294`

**Code**: `task.order.payload.assign(payload_buf.data(), payload_len);`

**Problem**: `SignedOrder::payload` is `std::string`. The `assign()` call **allocates heap memory** on every tick that produces a signal. The `SubmitTask` struct's `order` member holds a `SignedOrder` containing `std::string payload`.

**Documentation claim**: `execution_engine.cpp:283` comment says "ALL fixed-size, ZERO heap allocations." This is **false**.

---

### Issue 12: Implicit `string_view → string` Conversions
**File**: `core/src/execution_engine.cpp:172-173, 220`

**Code**:
```cpp
std::string_view market_slug(signal->market_slug);           // line 169 — ok
std::string_view country_code(operator_jurisdiction_);        // line 170 — ok
TickResult compliance_result = compliance_.check_all(
    market_slug, country_code, market_cache_);                 // line 172-173 — implicit string alloc
int tick_size = market_cache_.get_tick_size(market_slug);      // line 220 — implicit string alloc
```

**Problem**: `check_all()` takes `const std::string&` and `get_tick_size()` takes `const std::string&`. The `string_view` arguments are **implicitly converted** to `std::string` on every call, causing a heap allocation each. These are in the **primary success path** of `run_tick()`.

**Impact**: Two heap allocations on every tick (before the early return for compliance failure).

---

### Issue 13: Telemetry `log_event()` and `log_risk_block()` Allocate
**File**: `core/src/telemetry.hpp:143-186`

**Problem**: `TelemetryEvent` contains three `std::string` members (lines 46-48). `log_event()` constructs a `TelemetryEvent` with heap-allocated strings, then `try_push(event)` **copies** them into the SPSC ring buffer. Each call triggers 3+ allocations.

**Impact**: On the success path (`execution_engine.cpp:327`), `telemetry_.increment_orders_submitted()` is just an atomic (no alloc). But on error paths like the 429 case (line 317-321), `log_event()` is called with explicit `std::string(market_slug)` conversion.

**Verdict**: `increment_orders_submitted()` and `record_tick_result()` are fine (atomic only). `log_event()` and `log_risk_block()` allocate but are only on error/edge paths, not every tick in the success path. Acceptable for now, but should be improved.

---

### Issue 14: `make_index_key()` Allocates Despite Claims
**File**: `core/include/order_manager.hpp:159-169, 308-315`

**Code**:
```cpp
static std::string make_index_key(std::string_view market_slug, uint8_t side) {
    std::string key;
    key.reserve(market_slug.size() + 4);   // ← heap allocation
    key += market_slug;                    // ← heap allocation
    key += ':';
    key += (side == 0) ? '0' : '1';
    return key;
}
```

**Problem**: The comment at line 168 claims "NO std::string allocation (transparent_string_hash.hpp provides string_view support)" — but:
1. `transparent_string_hash.hpp` does not exist (Issue 1)
2. `make_index_key()` explicitly creates a `std::string` with `reserve()` + `+=`
3. `open_order_index_` is `std::unordered_map<std::string, size_t>` — cannot do transparent lookup

**Impact**: `has_open_order()` allocates on every call (hot path). The "O(1) no-alloc" claim is false.

---

## 🔴 CRITICAL — Risk Management Defects

### Issue 15: `market_exposure` and `market_pnl` Hardcoded to Zero
**File**: `core/src/execution_engine.cpp:229-230`

**Code**:
```cpp
double market_exposure = 0.0;   // line 229 — NEVER COMPUTED
double market_pnl = 0.0;        // line 230 — NEVER COMPUTED
```

**Problem**: These are passed to `risk_engine_.pre_trade_check()` (line 251-252). The exposure value is **never computed** from actual position data — it's always zero. The exposure limit check in `RiskEngine::pre_trade_check()` (risk_engine.hpp:78) always passes.

**Impact**: Position exposure limits are **never enforced.** A runaway strategy could exceed all exposure limits without any risk check stopping it.

---

### Issue 16: Rate Window Checks Only 1 Bucket, Not 60
**File**: `core/include/risk_engine.hpp:284-306`

**Code**:
```cpp
bool check_rate_window() {
    // ... computes current bucket index ...
    reset_if_stale(order_buckets_[bucket_idx], now_bucket);
    // O(1) check: reads ONLY the CURRENT bucket's counter
    uint32_t current_orders = order_buckets_[bucket_idx].count.load(std::memory_order_acquire);
    if (current_orders >= static_cast<uint32_t>(config_.max_orders_per_min)) {
        return false;
    }
    return true;
}
```

**Problem**: Only the current 1-second bucket is checked, not the full 60-second window. With `max_orders_per_min=10`, this effectively allows **10 orders per second** (600/min), not 10 per minute as intended. The code never sums across all 60 buckets.

**Documentation claim**: `risk_engine.hpp:269-274` comment says "60-second window in 1s buckets." The implementation is only a **1-second window**.

**Impact**: Rate limiting is 60× more permissive than documented. The bot could trigger exchange-side rate limiting and account bans.

---

### Issue 17: `record_order()` Ignores USD Amount
**File**: `core/include/risk_engine.hpp`

**Problem**: `record_order(double order_usd, bool is_buy)` receives the order's USD value but **does not use it** — the `order_usd` parameter is ignored. The daily notional limit (`max_daily_notional_usd`) is **never accumulated or checked**.

**Impact**: Daily notional cap is unenforced.

---

## ⚠️ HIGH — Other Issues

### Issue 18: `std::atomic<double>` Usage (Portability)
**Files**: `risk_engine.hpp:261`, `telemetry.hpp:334-335`

**Problem**: `std::atomic<double>` is only lock-free if `is_always_lock_free` is true. Not guaranteed on all platforms. On x86-64 it is lock-free, but the C++ standard does not guarantee it.

**Severity**: LOW on x86-64, technically non-portable.

---

### Issue 19: `process_submit_queue` Double Retry + Unprocessed Response
**File**: `core/src/execution_engine.cpp:426-438`

**Problem**:
1. `submit_order_with_response()` already retries with `max_retries_=5` (exponential backoff).
2. `process_submit_queue()` calls it a **second time** at line 436 for 5xx responses — total potential retries: 12.
3. The retry response at line 436 is assigned to `retry_response` but **never processed** — the comment `// ... handle retry response` is a TODO that was never implemented.

**Impact**: Excessive latency under rate limiting; retry results are silently discarded.

---

### Issue 20: SPSC RingBuffer `try_pop` Copies Instead of Moves
**File**: `core/include/spsc_ring_buffer.hpp` (likely)

**Problem**: `try_pop()` does `T item = buffer_[current_tail]` — **copy construction**. For `TelemetryEvent` (3 `std::string` members), this triggers 3 heap allocations per pop in the writer thread.

**Fix**: `T item = std::move(buffer_[current_tail])` before resetting the slot.

---

## 🟡 MEDIUM — Documentation Accuracy

### Issue 21: README Hot Path Diagram References Non-Existent Code
**File**: `README.md:157-170`

**Problem**: The diagram references `register_order_no_alloc` (step 10) and `has_open_order` with "string_view + transparent hash (no alloc)" — neither is true in the current code. See Issues 2 and 14.

---

### Issue 22: `VERIFICATION_FINAL.md` Claims `register_order_no_alloc` Exists
**File**: `VERIFICATION_FINAL.md:124,202`

**Problem**: References a method that is **not implemented**. Status marker "🟡 Important" is misleading.

---

### Issue 23: Stale Build Artifacts
**Files**: `core/build/`

**Problem**: The committed `core/build/` directory contains binaries that do not match the current source tree (per `AUDITOR_VERDICT.json:11`). The binaries are not reproducible from the current CMake configuration.

---

## 🟢 LOW — Minor Issues

### Issue 24: NonceManager Uses `CLOCK_REALTIME`
**File**: `core/include/nonce_manager.hpp`

**Problem**: `clock_gettime(CLOCK_REALTIME, ...)` can go backwards due to NTP adjustments. Polymarket CLOB V2 requires monotonically increasing nonces.

**Fix**: Use `CLOCK_MONOTONIC`.

---

### Issue 25: Private Key Hex Parsing Doesn't Validate Characters
**File**: `core/src/execution_engine.cpp:464-465`

**Code**: `strtol(buf, nullptr, 16)` silently converts invalid hex characters (e.g., 'G', 'X', whitespace) to 0.

**Severity**: LOW — requires user error with a malformed env var.

---

### Issue 26: `CURLOPT_NOBODY` Set With `CURLOPT_POSTFIELDS`
**File**: `core/src/lightweight_client.hpp` (per `AUDITOR_VERDICT.json:36`)

**Problem**: `CURLOPT_NOBODY` (HEAD request flag) is set on a POST request with a body. Some libcurl builds discard the body when NOBODY is set.

**Impact**: Orders may be submitted without a body, causing silent non-submission.

---

## Complete Violation List

| # | Issue | Location | Type | Severity | Status |
|---|---|---|---|---|---|
| 1 | Missing `transparent_string_hash.hpp` | `order_manager.hpp:17` | Build | **CRITICAL** | ❌ Open |
| 2 | Missing `register_order_no_alloc` method | `execution_engine.cpp:281` | Build | **CRITICAL** | ❌ Open |
| 3 | No `find_package(CURL/OpenSSL/secp256k1)` | `CMakeLists.txt` | Build | **CRITICAL** | ❌ Open |
| 4 | Dockerfile COPY target mismatch | `Dockerfile.prod:53` | Build | **CRITICAL** | ❌ Open |
| 5 | Tests not registered in CMake | `CMakeLists.txt` | Build | HIGH | ❌ Open |
| 6 | Secondary index never populated | `order_manager.hpp:95-113` | Logic | **CRITICAL** | ❌ Open |
| 7 | Zero domain separator in production | `eip712_signer.hpp:251` | Crypto | **CRITICAL** | ❌ Open |
| 8 | Payload JSON truncated (len 14 vs 18) | `execution_engine.cpp:538` | Correctness | **CRITICAL** | ❌ Open |
| 9 | Shutdown deadlock (`while(true)` + join) | `execution_engine.cpp:395` | Lifecycle | **CRITICAL** | ❌ Open |
| 10 | `std::vector` in EIP-712 struct hash (hot path) | `eip712_signer.hpp:207` | Allocation | **CRITICAL** | 📝 Constrained |
| 11 | `std::string assign()` on payload | `execution_engine.cpp:294` | Allocation | **CRITICAL** | ❌ Open |
| 12 | Implicit `string_view→string` conversions | `execution_engine.cpp:172,220` | Allocation | **CRITICAL** | ❌ Open |
| 13 | Telemetry `log_event`/`log_risk_block` allocate | `telemetry.hpp:143` | Allocation | ⚠️ Acceptable (edge paths only) | ⚠️ Monitored |
| 14 | `make_index_key()` allocates | `order_manager.hpp:308` | Allocation | **CRITICAL** | ❌ Open |
| 15 | `market_exposure` hardcoded to 0.0 | `execution_engine.cpp:229` | Risk | **CRITICAL** | ❌ Open |
| 16 | Rate window checks 1 bucket, not 60 | `risk_engine.hpp:284` | Risk | **CRITICAL** | ❌ Open |
| 17 | `record_order()` ignores USD amount | `risk_engine.hpp` | Risk | HIGH | ❌ Open |
| 18 | `std::atomic<double>` (portability) | `risk_engine.hpp:261` | Portability | 🟡 LOW | ⚠️ Documented |
| 19 | Double retry + unprocessed response | `execution_engine.cpp:436` | Logic | MEDIUM | ❌ Open |
| 20 | `try_pop` copies instead of moves | `spsc_ring_buffer.hpp` | Allocation | MEDIUM | ❌ Open |
| 21-23 | Documentation accuracy issues | `README.md`, `VERIFICATION_FINAL.md` | Docs | MEDIUM | ❌ Open |
| 24 | `CLOCK_REALTIME` for nonces | `nonce_manager.hpp` | Low | 🟡 LOW | ⚠️ Minor |
| 25 | Private key hex validation missing | `execution_engine.cpp:465` | Low | 🟡 LOW | ⚠️ Minor |
| 26 | `CURLOPT_NOBODY` + POST body | `lightweight_client.hpp` | Network | HIGH | ❌ Open |

---

## Priority Fix List

### P0 (Must fix before any production consideration):
1. **Fix build** (Issues 1-3): Create `transparent_string_hash.hpp`, implement `register_order_no_alloc`, add `find_package` to CMake.
2. **Fix shutdown deadlock** (Issue 9): Add `std::atomic<bool> running_` flag to `process_submit_queue`.
3. **Fix domain separator** (Issue 7): Pass Polymarket CLOB V2 domain data to `EIP712Signer` in `ExecutionEngine` constructor.
4. **Fix payload truncation** (Issue 8): Change `append_str(..., 14)` to `append_str(..., 17)` at `execution_engine.cpp:538`.
5. **Fix secondary index** (Issue 6): Call `update_index()` from `register_order()`.
6. **Fix rate window** (Issue 16): Sum all 60 buckets in `check_rate_window()`, not just the current one.
7. **Fix exposure tracking** (Issue 15): Compute actual `market_exposure` from `PositionTracker`, not hardcode `0.0`.
8. **Fix Dockerfile** (Issue 4): Change `bot_bin` to `bin/crowdintel_bot`.

### P1 (Should fix before production):
9. Fix `make_index_key` allocation (Issue 14) — implement `transparent_string_hash.hpp` and use transparent hash map.
10. Fix implicit `string_view→string` conversions (Issue 12) — change `check_all()` and `get_tick_size()` signatures.
11. Fix `task.order.payload.assign()` allocation (Issue 11) — use fixed-size buffer in `SubmitTask`.
12. Fix double retry (Issue 19) — remove second retry in `process_submit_queue`.
13. Process retry response (Issue 19) — implement the `// ... handle retry response` TODO.
14. Fix `CURLOPT_NOBODY` (Issue 26) — remove from POST requests.

### P2 (Nice to have):
15. Replace `std::atomic<double>` with integer-based atomics.
16. `std::move` in `SPSC_RingBuffer::try_pop`.
17. Use `CLOCK_MONOTONIC` in `NonceManager`.
18. Validate hex characters in `load_private_key()`.
19. Update documentation to accurately reflect hot path status.
