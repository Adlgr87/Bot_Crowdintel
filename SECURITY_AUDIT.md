# Security Audit Report — Bot_Crowdintel

**Project:** Bot_Crowdintel — Low-latency Polymarket CLOB V2 trading bot (C++20)
**Scope:** Core trading engine (`/core`), tests (`/tests`), config (`/infra`)
**Date:** 2026-01-XX
**Reviewer:** OMNISCIENT Agent Team (NEMESIS adversarial audit lead)

---

## Executive Summary

The codebase demonstrates **strong security hygiene** across cryptographic signing, network I/O, configuration validation, and hot-path concurrency. The architecture follows defense-in-depth: fail-closed defaults, strict JSON parsing with duplicate-key rejection, TLS verification enforced everywhere, secret file permission gates, constant-time token comparison, and deterministic ECDSA nonces (RFC 6979).

However, **three medium-severity defects** were identified in the rate-limiter subsystem (integer overflow on clock anomalies, fixed backoff in WebSocket reconnection), and **one low-severity issue** (test private key reuse across files). None of the findings are critical — no hardcoded production secrets, no broken crypto, no authentication bypass. The most impactful issue is the rate-limiter clock-backward overflow, which could theoretically allow rate-limit bypass under adversarial clock conditions.

### Risk Summary

| Severity | Count | Description |
|----------|-------|-------------|
| **Critical** | 0 | — |
| **High** | 0 | — |
| **Medium** | 3 | Rate-limiter overflow, WebSocket fixed backoff, rate-limit backoff overflow |
| **Low** | 5 | Test key reuse, no `mlock`, coarse HMAC timestamp, default token ID, missing `CURLOPT_SSL_OPTIONS` |

---

## 1. Rate Limiter — Integer Overflow on Clock Backward Jump

**Severity:** Medium
**File:** `core/src/rate_limiter.hpp`, line 182
**Function:** `TokenBucket::refill()`

### Description
```cpp
uint64_t elapsed_ms = now - last;    // line 182
```
`now` and `last` are `uint64_t`. If the monotonic clock jumps backward (NTP slew correction, `adjtime`, or hypervisor vDSO anomaly), `now < last`, causing `elapsed_ms` to wrap to a value near `UINT64_MAX`. The subsequent multiplication on line 186-187:
```cpp
uint64_t refill_milli = static_cast<uint64_t>(elapsed_ms) * static_cast<uint64_t>(rate_per_sec_);
```
then overflows. The result may be 0 (causing the bucket to never refill — a self-inflicted DoS) or a wrapped value that refills the bucket to full capacity, **bypassing rate limiting**.

### Impact
An attacker who can manipulate the host clock (e.g., via NTP attack or VM escape) could force the token bucket to instantly refill to full capacity, bypassing order/cancel/query rate limits. At minimum, legitimate clock adjustments cause the bot to lose rate-limit protection for the refill window.

### Recommendation
Add a backward-clock guard before the subtraction:
```cpp
if (now < last) {
    // Clock went backward; reset the window without refilling.
    last_refill_ms_.store(now, std::memory_order_relaxed);
    return;
}
```

---

## 2. Rate Limiter — Overflow in `ms_until_available` Wait Calculation

**Severity:** Low-Medium
**File:** `core/src/rate_limiter.hpp`, line 229
**Function:** `TokenBucket::ms_until_available()`

### Description
```cpp
return (deficit + denom - 1ULL) / denom;
```
`deficit` is at most `TOKEN_SCALE - 1 = 999`. `denom` is `rate_per_sec_`, a `uint32_t` (max ~4.3 billion). The sum `deficit + denom - 1` can overflow `uint64_t` only if `denom` is near `UINT64_MAX`, which requires a pathological `rate_per_sec_` value. While the config parser bounds `rate_per_sec_` through `env_l()`, there is no upper bound check on the rate values in `RateLimitConfig` construction.

### Impact
With an extreme `rate_per_sec_` value, the wait time calculation wraps, potentially returning a near-zero wait time, allowing requests above the configured rate.

### Recommendation
Add a sanity cap on `rate_per_sec_` in `TokenBucket::init()` or use `__int128` for the division:
```cpp
if (rate_per_sec_ > 0 && rate_per_sec_ < (UINT64_MAX / 2))
    return (deficit + denom - 1ULL) / denom;
```

---

## 3. WebSocket Reconnection — Fixed Backoff, No Exponential Increase

**Severity:** Medium
**File:** `core/src/ws_user_listener.hpp`, line 93
**Function:** `WsUserListener::run_loop()`

### Description
```cpp
uint32_t backoff_ms = 250;   // line 93 — never increases
```
The reconnect backoff starts at 250ms and is never increased. Each failed connection attempt retries at 250ms intervals indefinitely.

### Impact
During a venue-side outage or network partition:
- The bot hammeres the Polymarket WebSocket endpoint at 4 connections/second per listener.
- Multiple listeners (market + user channels) compound this to 8 connections/second.
- Could trigger venue-side rate limiting or IP ban.
- No circuit-breaker integration: even if `CircuitBreaker` trips OPEN, the WebSocket reconnect loop continues unabated.

### Recommendation
Implement exponential backoff with jitter (starting at 250ms, doubling up to 30s) and integrate with the `CircuitBreaker` state — if the circuit is OPEN, suspend reconnect attempts entirely.

---

## 4. Rate Limiter Backoff — `retry_ms` Can Set Permanent Backoff

**Severity:** Low
**File:** `core/src/rate_limiter.hpp`, line 318
**Function:** `RateLimiter::on_rate_limited()`

### Description
```cpp
if (retry_ms > new_backoff) {
    new_backoff = retry_ms;      // line 318-320
}
```
The server's `Retry-After` value is trusted without bounds. A malicious or compromised proxy could send `Retry-After: 999999999` (or `UINT64_MAX`), permanently blocking the rate limiter for that request type.

### Recommendation
Cap `retry_ms` to `config_.backoff_max_ms`:
```cpp
if (retry_ms > 0 && retry_ms < config_.backoff_max_ms)
    new_backoff = std::max(new_backoff, retry_ms);
```

---

## 5. Hardcoded Test Private Key Reused Across Multiple Files

**Severity:** Low
**Files:**
- `core/crypto/test_signer.cpp` lines 109, 259
- `tests/unit/test_core.cpp` lines 576-577, 892, 1017

### Description
The test private key `23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b` is hardcoded as a string literal in three test files. It is used for EIP-712 known-answer tests (golden signature vectors) and config loading tests.

### Assessment
This is a **test-only key**, not a production credential. The golden-vector tests require a fixed key to verify byte-for-byte signature output against an independent Python reference. The key is wiped via `secure_zero` after use. **No production secrets are exposed.**

However, the key is used without guard macros — if a test binary were accidentally run with `BOT_MODE=live` and real credentials, the test key would be used for signing. This is mitigated by the config loader requiring `BOT_ENABLE_LIVE_TRADING=1` and real CLOB credentials.

### Recommendation
- Add a `#ifndef CROWDINTEL_TEST_KEY` guard with a comment that this is test-only.
- Consider moving the key to a `tests/crypto/golden_key.hpp` header to centralize it.

---

## 6. Secrets Not Memory-Locked (No `mlock`)

**Severity:** Low
**File:** `core/src/market_config.hpp`, `core/crypto/secure_zero.hpp`

### Description
The `secure_zero` implementation correctly wipes secret buffers on destruction using `volatile`. However, the comment itself notes:
> "For a hardened deployment pair with mlock() to avoid swap."

Secrets (private key, API credentials, HMAC key, bearer token) are stored in `char[]` / `uint8_t[]` buffers in `MarketConfig`, `LightweightCLOBClient`, and `EIP712Signer` lifetimes. These pages are never locked into RAM via `mlock`/`mlockall`.

### Impact
If the OS swaps process memory to disk, cryptographic secrets could land in a swap file, exposing them to anyone with disk access.

### Recommendation
Call `mlockall(MCL_CURRENT | MCL_FUTURE)` at process startup (after loading secrets, before any trading loop begins). For extra hardening, `mmap` secret buffers with `MAP_LOCKED`.

---

## 7. HMAC Timestamp Granularity — Second-Level

**Severity:** Low
**File:** `core/src/lightweight_client.hpp`, line 474
**Function:** `LightweightCLOBClient::now_unix_seconds()`

### Description
The HMAC authentication message uses Unix timestamps with **second** granularity (`CLOCK_REALTIME`). The message is `timestamp + method + path + body`. The Polymarket CLOB API accepts timestamps with a replay window (typically 60 seconds).

### Impact
The replay window is determined by the venue, not the bot. The coarse second granularity does not itself expand the window, but it means the bot's timestamp has 1-second resolution rather than millisecond. This is acceptable for API authentication but slightly less precise than necessary.

### Recommendation
Consider using millisecond timestamps if the venue API supports it, reducing the replay window resolution. Otherwise, this is a non-issue — the venue's own replay protection is the effective control.

---

## 8. Default Token ID in Production Config

**Severity:** Low
**File:** `core/src/market_config.hpp`, line 404

### Description
```cpp
const char* tok = env("BOT_TOKEN_ID",
    "71321045679252212594626395510336467040167069592778062791519851593659551227755");
```
A default market token ID is hardcoded as a fallback. If `BOT_TOKEN_ID` is not set in production, the bot will silently trade the wrong market.

### Impact
Configuration error leading to trading the wrong market, not a direct security vulnerability. However, it is a "fail-open" pattern for a critical parameter that should fail closed.

### Recommendation
Remove the default fallback — require `BOT_TOKEN_ID` to be explicitly set in live mode. Or validate that the default is only used in mock mode:
```cpp
if (mock_mode && !tok) {
    tok = "71321045679252212594626395510336467040167069592778062791519851593659551227755";
} else if (!tok || !*tok) {
    return "BOT_TOKEN_ID is required in live mode";
}
```

---

## 9. Missing `CURLOPT_SSL_OPTIONS` for Blacklisting Weak CAs

**Severity:** Low
**Files:** `core/src/lightweight_client.hpp` lines 447-448, `core/src/ws_market_listener.hpp` lines 285-286

### Description
Both the REST client and WebSocket listeners set `SSL_VERIFYPEER` and `SSL_VERIFYHOST` but do not set `SSL_CTX_set_verify_depth()`, `SSL_OP_NO_TICKET`, `SSL_OP_NO_RENEGOTIATION`, or restrict cipher suites. Additionally, `CURLOPT_SSL_OPTIONS` is not set in the curl configuration.

### Impact
Weak cipher suites or overly deep certificate chains could theoretically be negotiated, though OpenSSL defaults in modern builds are generally safe.

### Recommendation
Add to `configure_common()` and equivalent:
```cpp
curl_easy_setopt(curl_, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE);
```
And in OpenSSL contexts:
```cpp
SSL_CTX_set_verify_depth(context, 3);
SSL_CTX_set_options(context, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_TICKET);
SSL_CTX_set_cipher_list(context, "HIGH:!aNULL:!kRSA:!PSK:!SRP:!MD5:!RC4");
```

---

## 10. `RateLimiter::can_send` Does Not Check Backoff Expiry Correctly for Zero Backoff

**Severity:** Low
**File:** `core/src/rate_limiter.hpp`, line 279

### Description
```cpp
if (bs.backoff_ms > 0 && bs.backoff_until_ms > now) {
    return false;
}
if (bs.backoff_ms > 0) {
    bs.backoff_ms = 0;
    bs.backoff_until_ms = 0;
}
```
On line 279, if `now` hasn't advanced past `backoff_until_ms` but the backoff is still active, requests are blocked. However, there is a TOCTOU window: between the check and the action, the clock could advance. More importantly, the `now` value is captured once and used for both checks, so if the backoff expires *between* the check and the reset, the reset clears it — which is correct. However, if `backoff_until_ms == now` exactly, the second branch clears it, which is the intended behavior. This is actually correct.

**Reassessment:** This is not a bug — the logic is sound. No action needed.

---

## Security Strengths (Confirmed Good Practices)

### Cryptographic Correctness
1. **RFC 6979 deterministic nonces** — `secp256k1_ecdsa_sign_recoverable` with `nullptr` nonce function uses libsecp256k1's built-in RFC 6979 implementation. Nonces are deterministic and never repeated.
2. **Low-S normalization** — secp256k1 enforces low-S by default, preventing signature malleability.
3. **Domain separator cached once** — `compute_domain_separator` is called once at `init()`, not per-signature.
4. **Context randomization** — `secp256k1_context_randomize` is called with OS-entropy at startup, blinding the ecmult table against side-channel cache attacks.
5. **Keccak-256 KATs verified** — Empty string, "abc", and multi-block (300-byte) vectors all match Ethereum known-answer vectors.
6. **HMAC-SHA256 midstate optimization** — Precomputed inner/outer pad midstates, verified against RFC 4231 test vector.

### Input Validation & Injection Defense
7. **Strict JSON parsing** — `bounded_json::Parser` validates syntax (no partial parses), rejects duplicate keys via `duplicate_key()` in `classify_response()`.
8. **HTTP request smuggling prevention** — `alpha_http_receiver.hpp` rejects `Transfer-Encoding` headers, enforces exact `Content-Length` bounds, uses bounded `recv_until()` with deadlines.
9. **JSON string injection prevention** — Credentials are validated with `printable_secret()` which rejects control characters, whitespace, and JSON metacharacters (`"`, `\`).
10. **Decimal/string parsing bounds** — `parse_fixed1e6` rejects overflow beyond `UINT64_MAX`, `parse_uint256_dec` rejects overflow past 256 bits.

### Secret Hygiene
11. **Environment variable unsetting** — `read_secret()` calls `unsetenv()` after reading, preventing secrets from leaking via `/proc/<pid>/environ`.
12. **File permission gates** — Secret files require `S_IRUSR` only (no group/other read), `O_NOFOLLOW` (no symlink traversal), `S_ISREG` (no device files).
13. **`secure_zero` on all secret buffers** — Private keys, HMAC contexts, decoded secrets, and response buffers are all zeroed on destruction.
14. **Ambiguous source rejection** — If both `BOT_PRIVATE_KEY_HEX` and `BOT_PRIVATE_KEY_HEX_FILE` are set, `read_secret()` returns false (fail-closed).

### Network Security
15. **TLS verification enforced** — `CURLOPT_SSL_VERIFYPEER=1L` and `CURLOPT_SSL_VERIFYHOST=2L` in all curl handles.
16. **Protocol whitelisting** — `CURLOPT_PROTOCOLS_STR = "https"` (and `wss://` for WebSockets) prevents SSRF via `file://`, `ftp://`, etc.
17. **TLS pinning support** — `BOT_TLS_PIN` supports `sha256//` public key pinning with up to 2 pins (for rotation).
18. **No HTTP downgrade** — Config loader rejects non-`https://` `CLOB_HOST` and non-`wss://` WebSocket URLs in live mode.

### Hot-Path Safety
19. **Bounded allocations** — No heap allocation in the hot path; all buffers are stack arrays with compile-time sizes.
20. **Overflow-safe arithmetic** — `ceil_cost()` in `position_tracker.hpp` uses saturation (`UINT64_MAX` guard) to prevent unsigned underflow.
21. **SPSC ring buffer** — `SPSC_RingBuffer` uses correct memory ordering (`acq_rel` on CAS, `seq_cst` on seqlock publication).

---

## Detailed File-by-File Findings

### `core/src/rate_limiter.hpp`
| Issue | Severity | Line |
|-------|----------|------|
| Clock backward jump causes uint64 overflow in `elapsed_ms` | Medium | 182 |
| `refill_milli` overflow can zero-out or wrap token refill | Medium | 186-187 |
| `deficit + denom - 1` potential overflow in `ms_until_available` | Low | 229 |
| Unbounded `retry_ms` from server can cause permanent backoff | Low | 318 |

### `core/src/ws_user_listener.hpp`
| Issue | Severity | Line |
|-------|----------|------|
| Fixed 250ms reconnect backoff, no exponential increase | Medium | 93 |
| No circuit-breaker integration in reconnect loop | Low | 92-95 |

### `core/crypto/test_signer.cpp` / `tests/unit/test_core.cpp`
| Issue | Severity | Line |
|-------|----------|------|
| Test private key hardcoded in 3 files | Low | 109, 259, 576, 892, 1017 |

### `core/src/lightweight_client.hpp`
| Issue | Severity | Line |
|-------|----------|------|
| HMAC timestamp uses 1-second granularity | Low | 474 |
| No `CURLSSLOPT_NO_REVOKE` in SSL options | Low | 447 |

### `core/src/market_config.hpp`
| Issue | Severity | Line |
|-------|----------|------|
| Default `BOT_TOKEN_ID` fallback in live mode | Low | 404 |
| No `mlock` for secret pages | Low | (architectural) |

### `core/crypto/secure_zero.hpp`
| Issue | Severity | Line |
|-------|----------|------|
| Comment recommends `mlock` but not implemented | Low | 8 |

### `core/src/circuit_breaker.hpp`
| Issue | Severity | Line |
|-------|----------|------|
| `allow_request()` in HALF_OPEN allows `one extra` probe beyond `half_open_max` | Low | 147-148 |

The HALF_OPEN transition from OPEN allows one extra request (line 147 increments `half_open_allowed_` before returning true, but the check on line 152 `if (half_open_allowed_ >= config_.half_open_max)` allows `half_open_max` requests. With `half_open_max = 1` (default), this allows 1 probe — which is correct. **No bug.**

### `core/src/ws_market_listener.hpp`
| Issue | Severity | Line |
|-------|----------|------|
| Same fixed backoff pattern as ws_user_listener | Medium | — |
| No `SSL_OP_NO_RENEGOTIATION` | Low | 285-292 |

### `core/src/alpha_http_receiver.hpp`
| Issue | Severity | Line |
|-------|----------|------|
| Bearer token comparison leaks header existence (early return) | Info | 251 |

The `authorized()` function returns false early if `header_string()` fails (header not found). An attacker can distinguish "header absent" from "wrong token" via response timing. However, since all 401 responses return the same message and the timing difference is negligible (<1μs), this is informational.

---

## Remediation Priority

| Priority | Issue | Effort |
|----------|-------|--------|
| **P1** | Rate limiter clock-backward overflow (Finding #1) | 5 lines |
| **P2** | WebSocket reconnect exponential backoff (Finding #3) | 20 lines |
| **P3** | `retry_ms` cap in `on_rate_limited` (Finding #4) | 1 line |
| **P4** | Rate limiter overflow guard in `ms_until_available` (Finding #2) | 3 lines |
| **P5** | `mlockall` at startup (Finding #6) | 3 lines |
| **P6** | Remove default token ID in live mode (Finding #8) | 10 lines |
| **P7** | Test key centralization (Finding #5) | 10 lines |

---

## Conclusion

The Bot_Crowdintel codebase demonstrates a mature, security-conscious implementation of a high-frequency trading bot. The core cryptographic primitives (EIP-712 signing, HMAC-SHA256, Keccak-256) are correct and verified against known-answer vectors. Network I/O is well-defended against injection, smuggling, and downgrade attacks. Secret management includes environmental unsetting, file permission gates, and memory zeroization.

The identified issues are primarily in the **rate-limiter subsystem** (integer overflow on clock anomalies) and **operational hardening** (missing `mlock`, fixed reconnect backoff). None constitute a critical vulnerability or would allow unauthorized fund access directly. The highest-priority fix — the rate-limiter clock-backward overflow — should be addressed immediately, as it could theoretically be exploited to bypass rate limiting under adversarial clock manipulation.

**Overall security posture: A-** (strong foundation, minor hardening gaps)
