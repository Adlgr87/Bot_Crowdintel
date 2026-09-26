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
  └── mutualambda/     # MutaLambda optimizer adapter
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
export BOT_PRIVATE_KEY_HEX=$(python3 -c "print('AA'*32)")
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
| P50  | 45.3  | ~121K |
| P99  | 94.5  | ~254K |

> **Note**: The P99 latency (94.5μs) is significantly higher than P50 due to
> occasional cache misses and branch mispredictions under load. The "hot path
> core logic only" benchmark measures signing + book evaluation, excluding
> network I/O. See `HOT_PATH_AUDIT_REPORT.md` for the full audit and `docs/PERF_METRICS.md`
> for detailed analysis.

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
# 6/6 tests passed
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