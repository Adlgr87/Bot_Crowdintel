# 🔥 Hot Path Audit Report — Detailed Findings

> **Author**: AGENTE_AUDITOR (independent verification)  
> **Date**: 2025-09-25  
> **Repo**: `Adlgr87/Bot_Crowdintel` (rama `remediation/compliance`)  
> **Commit**: `e48e68cd22e6e8f65a95633bfd3d1f3a61a2bc99`

---

## Summary

| Category | Count | Status |
|---------|-------|--------|
| ✅ Resolved (from remediation claims) | 3 | Claims string_view + async submit |
| ❌ NOT Actually Fixed | 8 | Still present in code |
| 📝 Documented Constraint | 1 | eip712_signer.hpp std::vector alloc |
| ⚠️ Partially Fixed | 3 | Some allocations moved, others remain |

---

## 🔴 NOT Fixed — Hot Path Allocations Before Signing

### Issue 1: `std::string(market_slug)` at line 294
**File**: `core/include/execution_engine.hpp`  
**Line 294**: `std::string client_order_id = order_mgr_.register_order(params, params.nonce, std::string(market_slug));`

**Problem**: Although `market_slug` at line 178 is a `std::string_view`, it is converted to `std::string` when passed to `register_order()`. The `market_slug` parameter of `register_order()` is typed as `const std::string&`, requiring a heap allocation. This happens AFTER signing but is still in the `run_tick()` hot path function.

**Fix Required**: Change `register_order()` to accept `std::string_view` and avoid the conversion. The `ManagedOrder` struct can store the `std::string` internally (only constructed once per order, not per tick).

---

### Issue 2: `std::string(market_slug)` at line 301
**File**: `core/include/execution_engine.hpp`  
**Line 301**: `task.market_slug = std::string(market_slug);`

**Problem**: Same as above — `std::string_view` is converted to `std::string` for the `SubmitTask` struct. This is AFTER signing but within `run_tick()`.

**Fix Required**: `SubmitTask` should use `std::string_view` or `const char[32]` instead of `std::string` for `market_slug`. However, since `SubmitTask` is pushed to a queue consumed by a background thread, the backing `AlphaSignal` memory may not be valid there. This requires careful design — either copy to a fixed buffer or ensure the queue keeps the signal alive.

---

### Issue 3: `std::string(market_slug)` at line 314
**File**: `core/include/execution_engine.hpp`  
**Line 314**: `telemetry_.log_event(EventType::ORDER_REJECTED, std::string(market_slug), ...)`

**Problem**: Another `std::string_view → std::string` conversion for telemetry. This is in the error path (queue full), but still in the hot path function.

**Fix Required**: `log_event()` should accept `std::string_view` and internally handle the copy in the SPSC push.

---

### Issue 4: `std::string(market_slug)` at line 326
**File**: `core/include/execution_engine.hpp`  
**Line 326**: `telemetry_.log_event(EventType::ORDER_SUBMITTED, std::string(market_slug), ...)`

**Problem**: Same issue — `std::string_view → std::string` conversion for telemetry in the success path.

**Fix Required**: Change `TelemetryEvent` to use fixed-size buffers or `string_view` for `market_slug`.

---

### Issue 5: `snprintf` + `std::string` in `generate_client_order_id` (order_manager.hpp:80-91)
**File**: `core/include/order_manager.hpp`  
**Lines 80-91**: Uses `snprintf` (stack buffer) but returns `std::string` (heap alloc).

**Problem**: Called from `register_order()` at line 294, AFTER signing but within `run_tick()`.

**Fix Required**: Return the ID into a caller-provided buffer (char[64]), or accept that this allocation is post-signing (still hot path function scope). The comment at line 68 claims "Hot path: register_order() and generate_client_order_id() are O(1) hash operations" — but this is misleading: the hash insert is O(1) amortized but the `std::string` construction for the key is a heap allocation.

---

### Issue 6: `std::string` concatenation in `build_order_payload` (execution_engine.hpp:487-507)
**File**: `core/include/execution_engine.hpp`  
**Lines 487-507**: `build_order_payload()` uses `std::string` with `+=` and `std::to_string`.

**Problem**: Called at line 289, AFTER signing but within `run_tick()`. Creates multiple heap allocations via string concatenation.

**Fix Required**: Use a stack-based char buffer with `snprintf` or `std::to_chars`. The payload size is bounded (known format), so a fixed buffer of 512 bytes (already reserved) is sufficient.

---

### Issue 7: `std::vector<uint8_t>` in `SubmitTask` (execution_engine.hpp:304-305)
**File**: `core/include/execution_engine.hpp`  
**Lines 304-305**: 
```cpp
task.maker_addr.assign(params.maker, params.maker + 20);
task.taker_addr.assign(params.maker, params.maker + 20);
```

**Problem**: `SubmitTask` uses `std::vector<uint8_t>` for maker/taker addresses (20 bytes each). `assign()` may cause heap allocation. This is AFTER signing but within `run_tick()`.

**Fix Required**: Use `std::array<uint8_t, 20>` instead of `std::vector<uint8_t>` in `SubmitTask`.

---

## 🔴 NOT Fixed — Network I/O in Hot Path

### Issue 8: Network I/O at line 216 (VERIFICATION FINDING)
**File**: `core/include/execution_engine.hpp`

The VERIFICATION_REPORT_FINAL.md states:
> `client_.submit_order_with_response(final_order)` performs **network I/O** (HTTP POST to Polymarket CLOB) inside `run_tick()`.

**Current State**: Line 216 in VERIFICATION report. Let's check the actual code.

The AUDIT_FINAL_REPORT.md claims:
> Line 216: `submit_queue_.try_push(std::move(task));` — O(1) SPSC, no network I/O

**Analysis**: The ACTUAL code (lines 312-319) shows:
```cpp
if (!submit_queue_.try_push(std::move(task))) {
    // ...
    return TickResult::RISK_BLOCKED;
}
// Order signed and queued for async submission — hot path complete.
```

**Verdict**: The network I/O HAS been moved to the background thread via `submit_queue_`. The VERIFICATION_REPORT was referencing the OLD code (line 216). The remediation appears to have actually fixed this. **✅ RESOLVED**

However, there's a subtle issue: if `submit_queue_.try_push()` fails (queue full), it falls back to logging and returns RISK_BLOCKED — this is an error path, not hot path.

---

## 🔴 NOT Fixed — TelemetryEvent std::string Allocations

### Issue 9: TelemetryEvent with 3 std::string members
**File**: `core/src/telemetry.hpp`  
**Lines 42-50**: `TelemetryEvent` contains 3 `std::string` members.

**Problem**: `log_event()` is called from `run_tick()` at lines 314 and 326. Each call constructs a `TelemetryEvent` with 3 `std::string` members, then copies it into the SPSC ring buffer via `try_push(item)` which does `T(item)` (copy constructor). Each hot-path `log_event` call involves 3 heap allocations for the std::string members.

**Fix Required**: Either:
- Use fixed-size char arrays in `TelemetryEvent` (e.g., `char market_slug[32]`)
- Only call `log_event` from error/edge cases, not every tick
- Use `std::string_view` and ensure the backing store is stable

The current code calls `log_event` from the SUCCESS path (line 326) — this should be minimized or removed from the absolute hot path.

---

## 🔴 NOT Fixed — Mutex/Lock in Hot Path

### Issue 10: `shared_mutex` lock in `market_metadata.get()`
**File**: `core/include/market_metadata.hpp`  
**Lines 92-99**: `get()` acquires `std::shared_lock<std::shared_mutex>`.

**Problem**: Called from hot path. While `shared_lock` allows concurrent reads, it still has overhead (atomic increment/decrement of the lock counter). The VERIFICATION_REPORT flags this as "mutex contention risk."

**Fix Required**: Use lock-free alternatives for the hot path. Options:
- Copy the cache atomically at startup intervals (immutable snapshot pattern)
- Use a read-copy-update (RCU) pattern
- Accept that this is called once per tick and is O(1) with shared_lock (contention is only an issue under high thread contention)

**Verdict**: This is O(1) with shared_lock. It's not zero-overhead, but it's not a data race. The impact depends on thread count. For a single hot-path thread (likely the case for ultra-low latency), the shared_lock contention is minimal. **⚠️ Acceptable but not optimal**

---

## 🔴 NOT Fixed — eip712_signer.hpp std::vector Allocations

### Issue 11: `std::vector<uint8_t> combined` at lines 166 and 207
**File**: `core/crypto/eip712_signer.hpp`

**Problem**: Both `eip712_domain_separator()` and `eip712_order_struct_hash()` allocate `std::vector<uint8_t> combined` on the heap.

**Constraint**: This file is marked **NEVER MODIFY** per the compliance mandate. The EIP-712 KAT vectors must continue to pass.

**Fix**: Cannot modify this file. The allocation happens inside `sign_order()` which IS the signing hot path. This is a documented constraint.

**Verdict**: 📝 **Documented Constraint** — Cannot be fixed without violating the "never modify eip712_signer.hpp" rule. Alternative: pre-compute the domain separator (already done via constructor) and eliminate the `std::vector` in `eip712_order_struct_hash` by using a stack buffer — but this requires modifying the protected file.

---

## 🔴 NOT Fixed — check_rate_window Complexity

### Issue 12: O(1) rate window check vs documented O(64)
**File**: `core/include/risk_engine.hpp`  
**Lines 288-310**: `check_rate_window()`

**Claim in VERIFICATION**: The AUDIT_FINAL_REPORT claims this is O(1) (line 158: "atomic counter per time-bucket, lazy reset — O(1) ✅"). But VERIFICATION_REPORT says (line 158): "Code loop iterates 64 entries (risk_engine.hpp:263)."

**Analysis**: Reading the actual code at lines 257-268:
```cpp
static constexpr size_t WINDOW_SIZE = 60;   // 60-second window, 1 bucket/sec
static constexpr uint64_t BUCKET_NS = 1'000'000'000ULL;
struct alignas(64) TimeBucket {
    std::atomic<uint32_t> count{0};
    std::atomic<uint64_t> epoch{0};
};
TimeBucket order_buckets_[WINDOW_SIZE];
```

And `check_rate_window()` at lines 288-310:
```cpp
bool check_rate_window() {
    uint64_t now_ns = ...;
    uint64_t now_bucket = now_ns / BUCKET_NS;
    uint64_t bucket_idx = now_bucket % WINDOW_SIZE;
    reset_if_stale(order_buckets_[bucket_idx], now_bucket);
    reset_if_stale(cancel_buckets_[bucket_idx], now_bucket);
    uint32_t current_orders = order_buckets_[bucket_idx].count.load(...);
    if (current_orders >= config_.max_orders_per_min) return false;
    uint32_t current_cancels = cancel_buckets_[bucket_idx].count.load(...);
    if (current_cancels >= config_.max_cancels_per_min) return false;
    return true;
}
```

**Verdict**: The code IS O(1) — it accesses a single bucket by index, not looping over all 64 entries. The VERIFICATION_REPORT was referencing OLD code (before remediation). The comment at line 254 still says "replaces the previous O(WINDOW_SIZE=64) full-scan loop" which is just documentation of what was replaced. **✅ RESOLVED**

---

## 🔴 NOT Fixed — has_open_order O(N) vs O(1)

### Issue 13: `has_open_order` complexity
**File**: `core/include/order_manager.hpp`  
**Lines 185-190**:

```cpp
bool has_open_order(std::string_view market_slug, bool is_buy) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::string idx_key = make_index_key(market_slug, is_buy ? 0 : 1);
    auto it = open_order_index_.find(idx_key);
    return it != open_order_index_.end() && it->second > 0;
}
```

**Problem**: While the lookup is O(1) (hash map), `make_index_key()` at line 320 creates a `std::string` (heap alloc) for the key. This is called from hot path.

**Fix Required**: Use a fixed-size buffer or `std::string_view` lookup with transparent hash. The `open_order_index_` already uses `TransparentStrHash` so it supports `string_view` lookup — but `make_index_key()` still allocates.

**Verdict**: ❌ **NOT FIXED** — the `std::string` key allocation in `has_open_order` is still a hot-path allocation.

---

## Complete Hot Path Violation List

| # | Issue | Location | Type | Status |
|---|-------|----------|------|--------|
| 1 | `std::string(market_slug)` → register_order | exec_engine.hpp:294 | alloc (post-sign) | ❌ NOT FIXED |
| 2 | `std::string(market_slug)` → SubmitTask | exec_engine.hpp:301 | alloc (post-sign) | ❌ NOT FIXED |
| 3 | `std::string(market_slug)` → log_event (error) | exec_engine.hpp:314 | alloc (edge case) | ❌ NOT FIXED |
| 4 | `std::string(market_slug)` → log_event (success) | exec_engine.hpp:326 | alloc (hot path) | ❌ NOT FIXED |
| 5 | `snprintf` + `std::string` in generate_client_order_id | order_manager.hpp:86-90 | alloc (post-sign) | ❌ NOT FIXED |
| 6 | `std::string +=` in build_order_payload | exec_engine.hpp:488-506 | alloc (post-sign) | ❌ NOT FIXED |
| 7 | `std::vector<uint8_t>` in SubmitTask | exec_engine.hpp:304-305 | alloc (post-sign) | ❌ NOT FIXED |
| 8 | TelemetryEvent with 3 std::string | telemetry.hpp:42-50 | alloc (hot path) | ❌ NOT FIXED |
| 9 | `std::string` key in make_index_key | order_manager.hpp:320 | alloc (pre-sign) | ❌ NOT FIXED |
| 10 | `std::vector<uint8_t>` in eip712_signer | eip712_signer.hpp:166,207 | alloc (signing) | 📝 CONSTRAINT (never modify) |
| 11 | `shared_mutex` lock in get() | market_metadata.hpp:92-99 | lock overhead | ⚠️ Acceptable |
| 12 | Network I/O | exec_engine.hpp:216 (old) | I/O | ✅ RESOLVED |
| 13 | O(64) rate window | risk_engine.hpp (old) | non-O(1) | ✅ RESOLVED |

---

## Priority Fix List

### P0 (Must fix before production):
1. Issue 6: Replace `build_order_payload` string concatenation with fixed buffer + `std::to_chars`
2. Issue 7: Replace `std::vector<uint8_t>` in `SubmitTask` with `std::array<uint8_t, 20>`
3. Issue 10: Fix `make_index_key` to avoid `std::string` — use `std::string_view` with transparent hash

### P1 (Should fix):
4. Issue 5: Return `client_order_id` via stack buffer instead of `std::string`
5. Issue 8: Minimize `log_event` calls from hot path success path
6. Issue 1-4: Pass `std::string_view` to all functions in post-signing path, construct `std::string` only at the boundary (background thread)

### P2 (Nice to have):
7. Issue 11: Document `eip712_signer.hpp` constraint clearly
8. Issue 12: Monitor `shared_mutex` contention in production, optimize if needed