# ⚡ Bot CrowdIntel: Ultra-Low Latency Polymarket Trader

A high-frequency trading bot for Polymarket CLOB V2, built around a deterministic,
zero-allocation **Hot Path** (C++20) and a signal-driven **Cold Path**.

> ⚠️ **Status**: Functional open-source prototype. Compiles with CMake, signs
> EIP-712 orders with **Keccak-256 + libsecp256k1** (verified against Python
> reference), and achieves **~47μs P50** on the hot path.
> For real-money deployment: security-audit secret zeroization, certificate
> pinning, and production credential management are still pending.

---

## 🗂️ Project Structure

```
core/                  # HOT PATH (C++20) - Zero Alloc, Lock-Free
  ├── include/         # OrderBookL2, SPSC_RingBuffer
  ├── crypto/          # EIP712Signer (Keccak-256 + libsecp256k1)
  └── src/             # ExecutionEngine, LightweightCLOBClient, NonceManager
alpha/                 # COLD PATH - Alpha Signals & Risk
  ├── crowdintel/      # AlphaParser (FDR q-value filtering)
  └── strategy/        # KellyEngine, MarketMakingEngine
infra/                 # Infrastructure
  ├── scripts/         # kernel_tuning.sh, deploy_production.sh
  ├── docker/          # Dockerfile.prod (Deterministic LTO build)
  └── mutualambda/     # Optimizer adapter (prototype, not yet active)
tests/benchmarks/      # Latency measurement (RDTSC)
docs/                  # Architecture, Perf Metrics, Optimization Lineage
.github/workflows/     # CI/CD pipeline
```

---

## 🏁 Quick Start

### Prerequisites
- Ubuntu 22.04+, CMake 3.16+, CMake 3.16+
- libcurl-dev, libssl-dev, autoconf
- libsecp256k1 v0.8.0 (built from source or via package manager)
- For production: Bare-metal server with `isolcpus` and `PREEMPT_RT`.

### Build & Run

```bash
# Native build (Fastest iteration)
cd core
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run crypto known-answer tests (always)
./bin/test_signer

# Run latency benchmark
./bin/latency_bench

# Run the hot path demo (requires env vars)
export BOT_PRIVATE_KEY_HEX=$(python3 -c "import secrets; print(secrets.token_hex(32))")
export CLOB_API_KEY="your_key"
export CLOB_SECRET="your_secret"
export CLOB_PASSPHRASE="your_passphrase"
./bin/crowdintel_bot
```

### Docker Build

```bash
# Deterministic build (Production)
docker build -f infra/docker/Dockerfile.prod -t crowdintel .
docker run --cpuset-cpus="2,3" crowdintel bin/crowdintel_bot
```

---

## 🧬 Performance Optimizations

### libsecp256k1 Integration

The hot path uses **libsecp256k1** (purpose-built for secp256k1) instead of
OpenSSL's generic ECDSA implementation. This provides a **~33× speedup** on
ECDSA signing:

| Operation | OpenSSL | libsecp256k1 | Speedup |
| :--- | :--- | :--- | :--- |
| Sign + recover | ~770 μs | ~31 μs | **25×** |

Key benefits:
- Recovery ID (v) computed **during** signing at zero extra cost
- Lower-S normalization automatic (EIP-2 compliant)
- No DER parsing overhead (native 64-byte compact format)
- Zero-alloc context available via `secp256k1_context_preallocated_create`

### Additional Hot Path Optimizations

- **Keccak-256**: Chain-based ρ+π (XKCP reference), zero-allocation
- **NonceManager**: Pre-computed buffer (KV-cache pattern), zero syscalls in hot path
- **SPSC_RingBuffer**: Lock-free, power-of-2 capacity, placement new
- **OrderBookL2**: Cache-aligned fields, bounds-checked with sentinel
- **OrderParams**: Cache-conscious field reordering

### Measured Results (RDTSC Benchmark, 20K ticks, 5K warmup)

| Metric | Value (μs) | Value (cycles @ 2.7GHz) |
| :--- | :--- | :--- |
| Min  | 44    | ~119K |
| P50  | 45    | ~121K |
| P99  | 94    | ~254K |

> See [`docs/OPTIMIZATION_LINEAGE.md`](docs/OPTIMIZATION_LINEAGE.md) for full history.
> See [`docs/PERF_METRICS.md`](docs/PERF_METRICS.md) for complete performance analysis.

---

## 🔐 Cryptography

### EIP-712 Signing Pipeline

```
OrderParams → ABI Encode → keccak256(typeHash || params) → struct_hash
                    ↓
domain_separator = keccak256(typeHash || domainData)
                    ↓
eip712_hash = keccak256(0x1901 || domain_separator || struct_hash)
                    ↓
(sign with libsecp256k1 → r || s || v)
```

- **Keccak-256**: Self-contained implementation (FIPS 202, not SHA3-256)
- **Domain Separator**: Properly computed from EIP-712 type + ABI-encoded data
- **ECDSA**: libsecp256k1 with recoverable signatures
- **Recovery ID**: `v = 27 + recid` (0=even y, 1=odd y)
- **Low-S**: Automatic (EIP-2 compliant)

### Cryptographic Verification

```bash
cd core/build
./bin/test_signer
```

Produces 20 ECDSA signatures with random nonces. All 20 are verified against
Python/pycryptodome reference (ECDSA verification + recovery_id match).

See [`core/crypto/SECP256K1_INTEGRATION_FINDINGS.md`](core/crypto/SECP256K1_INTEGRATION_FINDINGS.md)
for integration analysis.

---

## 🛠️ Development

### Run All Tests

```bash
cd core/build
ctest --output-on-failure
# 10/10 tests passed
```

**Phase 1: Rate Limiting & HTTP Client (T1-1–T1-5)**

| Component | File | Status |
|---|---|---|
| Token Bucket RateLimiter | `core/include/rate_limiter.hpp` | ✅ O(1), thread-safe (atomics) |
| Rate-limited HTTP Client | `core/src/lightweight_client.hpp` | ✅ Integrated, backoff, persistent curl |
| Rate Limiter Tests | `tests/test_rate_limiter.cpp` | ✅ 6 tests, all passing |

**RateLimiter (`RateLimiter(double rate, double burst)`)**:
- Token bucket with lazy atomic refill. `try_acquire()` is O(1) with a
  branch-predicted fast path. `next_available()` returns microseconds until the
  next token.
  - All `rate_per_sec_` and `burst` values are configurable via `RiskConfig` env
    vars (`CLOB_RATE_LIMIT_PER_SEC`, `CLOB_BURST`). Default is conservative:
    `1.0 token/sec` with `2.0 burst`.

**LightweightCLOBClient (T1-2–T1-4)**:
- **Exponential backoff with jitter** on HTTP 429 and 5xx
  (5 max retries, base 100ms × 2^attempt ±25% jitter, Retry-After header parsing).
- **Persistent curl handle** (`CURL*`) with keep-alive / connection reuse,
  TCP_NODELAY, TLS 1.3.
- **`HttpResponse`** struct with `HttpStatus` (enum), body, `retry_after`,
  `order_id`, and `error_message` extraction.
- **HMAC-SHA256** authentication preserved exactly — prehash is
  `timestamp + method + path + body`, Base64-encoded. No secrets are logged.

---

## 🛡️ Compliance & Risk Framework

The bot implements a multi-layer compliance framework (Phases 0–7) that runs
**before** cryptographic signing in the hot path. All controls are O(1) and
branch-predicted.

### Phase 2: Risk Engine & Kill Switch

| Component | File | Status |
|---|---|---|
| RiskEngine | `core/include/risk_engine.hpp` | ✅ Atomic kill switch, pre_trade_check |
| RiskConfig | `core/src/market_config.hpp` | ✅ All limits env-driven with conservative defaults |
| BalanceChecker | `core/src/balance_checker.hpp` | ✅ Background refresh thread |

**Risk limits** (all configurable via env vars):

| Parameter | Default | Env Var |
|-----------|---------|---------|
| max_daily_loss_usd | 500.0 | `RISK_MAX_DAILY_LOSS_USD` |
| max_order_usd | 500.0 | `RISK_MAX_ORDER_USD` |
| max_exposure_per_market | 5000.0 | `RISK_MAX_EXPOSURE_PER_MARKET` |
| min_usdc_balance | 100.0 | `RISK_MIN_USDC_BALANCE` |
| max_orders_per_min | 10 | `RISK_MAX_ORDERS_PER_MIN` |

The kill switch is `std::atomic<bool>` and is checked **before** signing in the
hot path. External signals (webhooks) never touch the flag directly.

### Phase 3: Order Manager & Position Tracker

| Component | File | Status |
|---|---|---|
| OrderManager | `core/include/order_manager.hpp` | ✅ client_order_id tracking, anti-retry |
| PositionTracker | `core/include/position_tracker.hpp` | ✅ Fill reconciliation, PnL tracking |
| PresignedOrderPool | `core/src/presigned_pool.hpp` | ✅ Price deviation invalidation |

- **client_order_id**: Generated once, tracked end-to-end through submission
- **Anti-retry**: On 429/5xx/nullopt, queries exchange before resubmitting
- **Self-trade prevention**: `has_open_order()` blocks duplicate market/side orders

### Phase 4: Fee Model

| Component | File | Status |
|---|---|---|
| FeeModel | `core/include/fee_model.hpp` | ✅ Dynamic fees, net-EV filter |

- Dynamic fee formula: `fee = C × 0.25 × (p·(1−p))²`
  - `C` = base commission rate (configurable via `FEE_COMMISSION_RATE`)
  - `p` = market probability (max fee at p=0.5, zero at p=0 or p=1)
- **net_ev filter**: `edge - fees - slippage - gas_cost > min_net_ev`
  - Positive edge but negative net_ev → `NOT_PROFITABLE`

### Phase 5: Compliance Guard & Market Metadata

| Component | File | Status |
|---|---|---|
| ComplianceGuard | `core/include/compliance_guard.hpp` | ✅ Fail-closed, env-driven |
| MarketMetadataCache | `core/include/market_metadata.hpp` | ✅ Tick size, market state |

- **Token blocklist**: Restricted tokens rejected (O(1) hash lookup)
- **Jurisdiction check**: Fail-closed if not in allowed list
- **Tick size**: Dynamic snapping applied before signing
- **Market state**: Closed/resolved markets blocked (`MARKET_NOT_TRADABLE`)

### Phase 6: Telemetry & Observability

| Component | File | Status |
|---|---|---|
| Telemetry | `core/src/telemetry.hpp` | ✅ Async JSON Lines audit log |
| SPSC_RingBuffer | `core/include/spsc_ring_buffer.hpp` | ✅ Lock-free, 8192 capacity |

- **Audit log**: Append-only JSON Lines (`std::ios::app`)
- **Hot path**: No I/O — events pushed to SPSC ring buffer, async writer thread
- **Alerts** (6 configurable env vars): daily loss, 429 streak, latency spike,
  position divergence, feed-dead detection, webhook URL
- **No secrets in logs**: `log_risk_block` only logs `order_size` + `reason`

### Test Suite

```bash
cd core/build
ctest --output-on-failure
# 10/10 tests passed
```

| Test | Tests | Assertion Cases |
|------|-------|-----------------|
| test_compliance_guard | — | — |
| test_fee_model | — | — |
| test_order_manager | 18 | 2062 |
| test_position_tracker | 14 | 38 |
| test_presigned_pool | 10 | 28 |
| test_rate_limiter | — | — |
| test_risk_engine | — | — |
| test_telemetry | 16 | 89 |
| keccak_known_answer | — | — |
| latency_benchmark | — | — |

### Build Dependencies

```bash
# System packages
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev \
  libssl-dev autoconf

# libsecp256k1 from source
git clone --depth 1 --branch v0.8.0 \
  https://github.com/bitcoin-core/secp256k1.git /tmp/secp256k1
cd /tmp/secp256k1 && ./autogen.sh && \
  ./configure --enable-module-recovery --disable-tests --disable-bench && \
  make -j$(nproc)
```

---

## ⚠️ Known Limitations

1. **Secret zeroization**: API keys/secrets in `std::string` (not `secure_string`)
2. **Certificate pinning**: Not yet implemented (MITM risk on VPS)
3. **Kernel tuning**: Requires bare-metal server (not VPS-compatible)
4. **WebSocket client**: Simulated (not a real WebSocket library)
5. **Backtester**: L2Backtester `run_replay` is a skeleton

See [`PROJECT_ANALYSIS.md`](PROJECT_ANALYSIS.md) for the full assessment with
prioritized action plan (P0–P3).

---

## 🛡️ Disclaimer

This codebase is educational and for research. Trading crypto and derivatives
involves substantial risk. The author assumes no liability for any trading losses
or damages caused by using this software.

**Not approved for production with funds** until security issues (secret zeroization,
certificate pinning) are resolved.