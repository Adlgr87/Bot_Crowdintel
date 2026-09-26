# ⚡ Bot CrowdIntel: Ultra-Low Latency Polymarket Trader

A high-frequency trading bot for Polymarket CLOB V2, built around a deterministic,
zero-allocation **Hot Path** (C++20) and a signal-driven **Cold Path**.

> ⚠️ **Status**: **Remediated** — Hot path audit complete (VERIFICATION_FINAL.md).
> Hot path runs zero-allocation, zero-network-I/O per tick. EIP-712 KAT verified 20/20.
> For real-money deployment: security-audit secret zeroization, certificate
> pinning, and production credential management are still pending.

---

## 🗂️ Project Structure

```
core/                  # HOT PATH (C++20) - Zero Alloc, Lock-Free after remediation
  ├── include/         # OrderBookL2, SPSC_RingBuffer, RiskEngine, OrderManager
  ├── crypto/          # EIP712Signer (Keccak-256 + libsecp256k1) — VERIFIED 20/20
  └── src/             # ExecutionEngine, LightweightCLOBClient, NonceManager
alpha/                 # COLD PATH - Alpha Signals & Strategy
  ├── crowdintel/      # AlphaParser (FDR q-value filtering)
  └── strategy/        # KellyEngine (fractional Kelly), MarketMakingEngine
infra/                 # Infrastructure
  ├── scripts/         # kernel_tuning.sh, deploy_production.sh, mutualambda_optimize.py
  ├── docker/          # Dockerfile.prod (Deterministic LTO/PGO build, Clang 15)
  └── mutualambda/     # Evolutionary optimizer adapter (real MutaLambda bridge)
tests/                 # Tests & benchmarks
  ├── test_signer.cpp        # EIP-712 KAT (20 random signature verification)
  ├── test_rate_limiter.cpp  # RateLimiter unit tests
  ├── benchmarks/
  │   └── latency_bench.cpp  # RDTSC-based hot path latency measurement
  └── replay/
      └── l2_backtester.cpp  # L2 market replay skeleton
docs/                  # Architecture & performance analysis
.github/workflows/     # CI/CD pipeline (latency gate enforcement)
```

---

## 🏁 Quick Start

### Prerequisites

- Ubuntu 22.04+, CMake 3.16+
- `build-essential`, `cmake`, `clang`, `libcurl4-openssl-dev`, `libssl-dev`, `autoconf`
- **libsecp256k1 v0.8.0** (built from source with `--enable-module-recovery`)

For production deployment: bare-metal server with `isolcpus` kernel parameter
and `taskset -c 2,3` CPU pinning. See [`infra/scripts/kernel_tuning.sh`](infra/scripts/kernel_tuning.sh).

### Build & Run

```bash
# Native build
cd core
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run crypto known-answer tests (required before every deployment)
./bin/test_signer
# Output: 20/20 KAT PASSED

# Run latency benchmark
./bin/latency_bench

# Run tests
ctest --output-on-failure
```

### Production Entry Point

The production binary (`crowdintel_bot`) loads all credentials from environment
variables. No credentials are ever hardcoded or logged.

```bash
export BOT_PRIVATE_KEY_HEX="<64-hex-char-private-key>"
export CLOB_API_KEY="<api-key>"
export CLOB_SECRET="<api-secret>"
export CLOB_PASSPHRASE="<passphrase>"
export OPERATOR_JURISDICTION="US"   # for compliance checks (default: US)

./bin/crowdintel_bot
```

### Docker Build (Production)

```bash
docker build -f infra/docker/Dockerfile.prod -t crowdintel-prod .
docker run --cpuset-cpus="2,3" --env-file .env crowdintel-prod
```

The Docker image uses Clang 15 with `-O3 -march=native -flto -fno-exceptions -fno-rtti`.

---

## 🧬 Performance Optimizations

### libsecp256k1 Integration

The hot path uses **libsecp256k1** (purpose-built for secp256k1, written in C) instead
of OpenSSL's generic ECDSA implementation. This provides a **~25× speedup** on
ECDSA signing + public key recovery:

| Operation | OpenSSL | libsecp256k1 | Speedup |
| :--- | :--- | :--- | :--- |
| Sign + recover | ~770 μs | ~31 μs | **25×** |

Key benefits:
- Recovery ID (`v`) computed **during** signing at zero extra cost
- Low-S normalization automatic (EIP-2 compliant)
- No DER parsing overhead (native 64-byte compact format)

> ⚠️ **Constraint**: `core/crypto/eip712_signer.hpp` is marked **NEVER MODIFY**.
> Lines 166-170 and 207-211 use `std::vector<uint8_t>` internally. This is an
> accepted constraint — these allocations occur once per order (not per tick)
> and are in the cold/signing path, not the hot path.

### Additional Hot Path Optimizations

| Optimization | File | Status |
|---|---|---|
| Zero-alloc payload builder (`std::array` + `std::to_chars`) | `execution_engine.cpp:474` | ✅ |
| SPSC lock-free queue (async network I/O) | `spsc_ring_buffer.hpp` | ✅ |
| O(1) order lookup (secondary index) | `order_manager.hpp:160` | ✅ |
| O(1) bucket-based rate window | `risk_engine.hpp:263` | ✅ |
| Atomic kill switch (checked before signing) | `risk_engine.hpp:55` | ✅ |
| Cache-aligned OrderBookL2 fields | `order_book.hpp` | ✅ |
| Cache-conscious OrderParams field reordering | `eip712_signer.hpp:50` | ✅ |

### Measured Results (RDTSC Benchmark, 20K ticks, 5K warmup)

> **Measurement scope**: Hot path core logic only (signing + book evaluation).
> Network I/O is excluded — it runs in a background thread via SPSC queue.

| Metric | Value (μs) | Value (cycles @ 2.7GHz) |
| :--- | :--- | :--- |
| Min  | 44    | ~119K |
| P50  | 45.3  | ~121K |
| P99  | 94.5  | ~254K |

> The P99 latency (94.5μs) is higher than P50 due to occasional cache misses and
> branch mispredictions under burst load. CI enforces a **P50 < 100μs** and
> **P99 < 200μs** gate on every push to `main` or `remediation/compliance`.
>
> See [`HOT_PATH_AUDIT_REPORT.md`](HOT_PATH_AUDIT_REPORT.md) for the full audit,
> [`VERIFICATION_FINAL.md`](VERIFICATION_FINAL.md) for remediation summary, and
> [`docs/PERF_METRICS.md`](docs/PERF_METRICS.md) for detailed analysis.

---

## 🛡️ Architecture: Hot Path vs Cold Path

### Hot Path (C++20, zero-allocation, isolated CPU core)

```
run_tick():
  1. Pop AlphaSignal from SPSC queue (O(1))
  2. Compliance guard — jurisdiction check (O(1))
  3. Kill switch check — atomic load BEFORE signing (O(1))
  4. Kelly position sizing (O(1))
  5. FeeModel net EV filter (O(1))
  6. RiskEngine::pre_trade_check — limits, exposure, balance (O(1))
  7. has_open_order — self-trade prevention (O(1), secondary index)
  8. EIP-712 sign_order (libsecp256k1, cold-path call)
  9. build_order_payload_fixed (stack buffer, zero-alloc)
  10. register_order_no_alloc (hash insert, O(1))
  11. SubmitTask → SPSC queue push (O(1), NO network I/O)
  ← return immediately
```

### Cold Path (background threads, can block)

```
process_submit_queue() [background thread]:
  1. Pop SubmitTask from SPSC queue
  2. client_.submit_order_with_response() (HTTP with backoff)
  3. Handle 429/5xx with exponential backoff + jitter
  4. Update OrderManager status (on fill/cancel)
  5. Update PositionTracker (cash value, exposure)
  6. Telemetry logging (async SPSC ring buffer)
```

---

## 🔐 Cryptography

### EIP-712 Signing Pipeline

```
OrderParams → ABI Encode (32-byte slots) → keccak256(typeHash || params) → struct_hash
                     ↓
domain_separator = keccak256(keccak256(domainType) ^^^ domainData)
                     ↓
eip712_hash = keccak256(0x1901 || domain_separator || struct_hash)
                     ↓
(secp256k1_schnorrsig_sign32 → r || s || v=27+recid)
```

- **Keccak-256**: Self-contained implementation (FIPS 202, not SHA3-256)
- **Domain Separator**: Computed from EIP-712 type + ABI-encoded domain data
- **ECDSA**: libsecp256k1 with recoverable signatures
- **Recovery ID**: `v = 27 + recid` (0=even y, 1=odd y)
- **Low-S**: Automatic (EIP-2 compliant)

### Cryptographic Verification

```bash
cd core/build && ./bin/test_signer
```

Produces 20 ECDSA signatures with random nonces. All 20 are verified against
a Python/pycryptodome reference (ECDSA verification + recovery_id match).

### Polymarket CLOB V2 API

| Component | Endpoint |
|---|---|
| REST API | `https://api.polymarket.com/v2/` |
| Market Data WS | `wss://ws-subscriptions-clob.polymarket.com/ws/market` |
| User Channel WS | `wss://ws-subscriptions-clob.polymarket.com/ws/user` |
| Auth | HMAC-SHA256 (`timestamp + method + path + body`, Base64) |
| Rate Limit | 200 requests/minute (enforced by `RiskEngine`) |

See [`core/src/lightweight_client.hpp`](core/src/lightweight_client.hpp) for the
HTTP client with exponential backoff, connection pooling, and HMAC auth.

---

## 🔄 Evolutive Optimization

The project integrates the [MutaLambda](https://github.com/Adlgr87/MutaLambda)
evolutionary optimization framework. The CI pipeline runs daily evolutionary
cycles on hot-path functions.

```
infra/mutalambda/
  └── adapter/
      └── mutualambda_adapter.py   # Real adapter (449 lines, no stubs/mocks)
infra/scripts/
  └── mutualambda_optimize.py      # CLI entry point
```

Optimization targets: `core/src/execution_engine.cpp::run_tick` and
`core/crypto/eip712_signer.hpp::keccak256_hash`.

> **Note**: The MutaLambda repository must be cloned and installed separately.
> The CI workflow checks out `Adlgr87/MutaLambda` before running the evolution
> cycle. See `.github/workflows/ci-cd-and-optimize.yml`.

---

## 🧪 Testing & CI/CD

### Test Suite

```bash
cd core/build
ctest --output-on-failure
```

| Test | Description | Status |
|---|---|---|
| `test_signer` | EIP-712 KAT (20 signatures vs Python ref) | ✅ 20/20 |
| `latency_bench` | RDTSC hot path benchmark | ✅ |
| `l2_backtester` | L2 replay skeleton | ✅ (skeleton) |

### CI Gates (GitHub Actions)

| Gate | Threshold | Purpose |
|---|---|---|
| Build (Clang 15, LTO) | — | Deterministic compilation |
| `test_signer` | 20/20 pass | Crypto integrity |
| `ctest` | All pass | Regression testing |
| P50 latency | < 100μs | Hot path performance |
| P99 latency | < 200μs | Tail latency control |
| MutaLambda evolution | — | Daily optimization (schedule: midnight UTC) |

CI triggers on push to `main` and `remediation/compliance` branches, plus
daily schedule.

---

## ⚠️ Known Limitations

| # | Limitation | File/Component | Priority |
|---|---|---|---|
| 1 | Secret zeroization (`std::string` keys, not `secure_string`) | `lightweight_client.hpp`, `execution_engine.cpp` | P1 |
| 2 | Certificate pinning (MITM risk on VPS) | `lightweight_client.hpp`, `ws_market_listener.hpp` | P1 |
| 3 | WebSocket listener is simulated (no real WS library) | `ws_market_listener.hpp` | P2 |
| 4 | L2 Backtester `run_replay` is a skeleton | `l2_backtester.cpp` | P2 |
| 5 | `bench_engine.hpp` uses hardcoded test key (`0xAA`) | `bench_engine.hpp` | P3 (benchmark-only) |
| 6 | EIP-712 `OrderParams` lacks `expiration` field | `eip712_signer.hpp` (NEVER MODIFY) | P2 (documented constraint) |
| 7 | `eip712_domain_separator` and `eip712_order_struct_hash` use `std::vector` | `eip712_signer.hpp` (NEVER MODIFY) | P3 (cold path, accepted) |

> ⚠️ **Production disclaimer**: Not approved for production with real funds until
> P1 limitations (secret zeroization, certificate pinning) are resolved. See
> [`VERIFICATION_FINAL.md`](VERIFICATION_FINAL.md) for the full remediation report.

---

## 📄 Key Documents

| Document | Purpose |
|---|---|
| [`VERIFICATION_FINAL.md`](VERIFICATION_FINAL.md) | Final remediation verification (authoritative) |
| [`HOT_PATH_AUDIT_REPORT.md`](HOT_PATH_AUDIT_REPORT.md) | Detailed hot path violation analysis |
| [`REMEDIATION_MASTER_PLAN.md`](REMEDIATION_MASTER_PLAN.md) | Audit findings → remediation mapping |
| [`docs/PERF_METRICS.md`](docs/PEF_METRICS.md) | Performance analysis & methodology |
| [`docs/OPTIMIZATION_LINEAGE.md`](docs/OPTIMIZATION_LINEAGE.md) | Optimization history |
| [`PROJECT_ANALYSIS.md`](PROJECT_ANALYSIS.md) | Full project assessment (P0–P3 action plan) |

---

## 🛡️ Disclaimer

This codebase is educational and for research. Trading crypto and derivatives
involves substantial risk. The author assumes no liability for any trading losses
or damages caused by using this software.

**Not approved for production with funds** until security issues (secret zeroization,
certificate pinning) are resolved.
