# ⚡ Bot CrowdIntel: Ultra-Low Latency Polymarket Trader (CLOB V2)

A high-frequency trading bot for **Polymarket CLOB V2** (live since 2026-04-28),
built around a deterministic, zero-allocation **hot path** (C++20) and
signal-driven **cold paths** (market feed, alpha ingest, pre-signing).

> **Status**: functional open-source prototype. The crypto pipeline is verified
> byte-for-byte against an independent Python reference of the official V2
> scheme, and the hot path measures **~90 ns P50** tick latency with the
> pre-signed order pool (**~24 µs** inline ECDSA fallback). For real-money
> deployment, review the open items in [`docs/STATUS.md`](docs/STATUS.md).

---

## 🗂️ Project Structure

```
core/                       # HOT PATH (C++20) — zero alloc, lock-free
  ├── include/              #   OrderBookL2 (seqlock), SPSC_RingBuffer
  ├── crypto/               #   EIP712Signer (Keccak-256 + libsecp256k1),
  │                         #   SHA-256/HMAC midstates, FastRandom salts
  └── src/                  #   ExecutionEngine, PresignedOrderPool,
                            #   LightweightCLOBClient (CLOB V2 REST),
                            #   WsMarketListener (hand-rolled RFC 6455 + TLS)
alpha/                      # COLD PATH — alpha signals & sizing
  ├── crowdintel/           #   AlphaParser (FDR q-value filtering)
  └── strategy/             #   KellyEngine (exact binary Kelly), MarketMaking
tests/
  ├── unit/                 #   SPSC, book, Kelly, amounts, wire body, HMAC, WSS
  ├── crypto/               #   Python cross-check (pycryptodome + coincurve)
  ├── benchmarks/           #   RDTSC latency benchmark (calibrated)
  └── replay/               #   L2 backtester (CSV replay)
infra/                      # Docker (deterministic build), kernel tuning, deploy
docs/                       # Architecture, performance, status
```

---

## 🏁 Quick Start

### Prerequisites
- Ubuntu 22.04+, CMake 3.16+, C++20 compiler (GCC 11+/Clang 14+)
- libcurl-dev, libssl-dev (live network layer)
- libsecp256k1 v0.8.0 built with the recovery module (see below)

```bash
# libsecp256k1 (CMake build — no autotools needed)
git clone --depth 1 --branch v0.8.0 https://github.com/bitcoin-core/secp256k1.git /tmp/secp256k1
cmake -S /tmp/secp256k1 -B /tmp/secp256k1/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DSECP256K1_ASM=x86_64 \
  -DSECP256K1_BUILD_BENCHMARK=OFF -DSECP256K1_BUILD_TESTS=OFF
cmake --build /tmp/secp256k1/build -j$(nproc)

# Build the bot
cd core && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSECP256K1_LIBRARY=/tmp/secp256k1/build/lib/libsecp256k1.a \
  -DSECP256K1_INCLUDE_DIR=/tmp/secp256k1/include
cmake --build build -j$(nproc)
```

### Run the test suite
```bash
cd core/build
ctest --output-on-failure          # crypto KATs, unit tests, bench, backtester

# Independent Python cross-verification of the EIP-712 V2 signer
./bin/test_signer --json > /tmp/signer.json
python3 ../tests/crypto/cross_check_v2.py /tmp/signer.json
```

### Run (offline demo — no network, no credentials)
```bash
BOT_MODE=mock \
BOT_PRIVATE_KEY_HEX=$(python3 -c "print('11'*32)") \
BOT_TICKS=8 \
./build/bin/crowdintel_bot
```

### Run (live)
```bash
export BOT_PRIVATE_KEY_HEX=...        # 64 hex chars — wallet key
export CLOB_API_KEY=... CLOB_SECRET=... CLOB_PASSPHRASE=...
export BOT_TOKEN_ID=...               # ERC-1155 position id (decimal string)
export BOT_NEG_RISK=0                 # 1 for neg-risk markets (V2 contract)
# optional: BOT_SIGNATURE_TYPE (0 EOA/1 POLY_PROXY/2 SAFE/3 POLY_1271),
#           BOT_MAKER_ADDRESS (funder; default = signer), BOT_TICK_SIZE,
#           BOT_PIN_CPU, BOT_KELLY_FRACTION, BOT_MIN_EDGE, BOT_TICKS
./build/bin/crowdintel_bot
```

---

## 🧬 What makes it fast

| Stage | Mechanism | Measured (this repo, 2.1 GHz shared vCPU) |
| :--- | :--- | :--- |
| Full tick, pre-signed pool hit | signal → filters → book → Kelly → **scan 16 pre-signed slots** → submit | **~90 ns P50**, ~109 ns P99 |
| Full tick, inline ECDSA | + Keccak ×3 + libsecp256k1 recoverable sign | ~24 µs P50 |
| Pre-signed pool scan only | fixed-slot linear compare | ~54 ns P50 |
| L2 auth HMAC per order | in-house SHA-256 with precomputed key midstates (no library calls) | ~0.3 µs |

Key techniques:
- **Pre-signed order pool** — the cold thread pre-builds and pre-signs orders
  over the plausible taker price grid (±8 ticks, both sides) and refreshes them
  before the CLOB timestamp goes stale. The hot path pops an already-signed
  order: tick-to-wire drops from "hash + ECDSA + JSON" to a ~50 ns scan.
- **Zero allocation, zero syscalls** in the hot path (fixed buffers, hand-rolled
  number formatting, vDSO clocks).
- **Persistent TLS connection** — one libcurl easy handle with keep-alive;
  handshake happens once, not per order.
- **HMAC key midstates** — the L2 auth signature per order is two SHA-256
  block compressions on precomputed states.
- **Seqlock order book + SPSC queue** — wait-free single-producer feeds into a
  pinned, spin/park consumer.
- **Deterministic salts** via RDRAND (CLOB V2 replaced nonces with
  `timestamp(ms)` + random `salt`).

Full numbers and methodology: [`docs/PERF_METRICS.md`](docs/PERF_METRICS.md).

---

## 🔐 Cryptography (verified)

- EIP-712 domain: `Polymarket CTF Exchange`, version **"2"**, chainId 137,
  verifyingContract `0xE111180000d2663C0091e4f400237545B87B996B`
  (standard) / `0xe2222d279d744050d28e00520010520000310F59` (neg-risk).
- Signed struct (V2): `Order(salt, maker, signer, tokenId, makerAmount,
  takerAmount, side, signatureType, timestamp, metadata, builder)`.
- Keccak-256 (pre-FIPS padding 0x01), self-contained; KATs pass.
- libsecp256k1 recoverable signing — RFC 6979 nonces, low-S, recid→v at zero cost.
- **Golden-vector verified**: the C++ signer's domain separator, struct hash,
  digest, and signature match an independent Python implementation
  (`tests/crypto/cross_check_v2.py`, pycryptodome + coincurve) byte-for-byte.
- **Rendimiento Keccak optimizado vía MutaLambda** — el núcleo `keccak_f1600`
  fue optimizado evolutivamente con el motor genético de
  [Adlgr87/MutaLambda](https://github.com/Adlgr87/MutaLambda) (ver PR #2).
  La mutación ganadora reescribe el paso ρ+π como una cadena de rotación
  in-situ con guardia `shift==0`, evitando el barrel-shifter en lanes de
  rotación cero, preservando la equivalencia funcional (KAT verificado bajo
  `g++` y `clang++`):

  | Métrica | Antes (XKCP canónico) | Después (MutaLambda) | Mejora |
  |---|---|---|---|
  | keccak_f1600 ns/op (clang++ 22.1.8, 1e6 permutaciones, -O3 -march=native) | 547.8 ns | 416.2 ns | **1.3156×** (+31.6 %) |
  | keccak256("") | `c5d24601…85a470` | `c5d24601…85a470` | ✅ idéntico |
  | keccak256("abc") | `4e03657a…c0d6c45` | `4e03657a…c0d6c45` | ✅ idéntico |

  - **Motor / parámetros**: LLM `agnes-2.5-flash` (endpoint OpenAI-compatible)
    vía MutaLambda `--backend openai`; NSGA-II 3 islas × 14 generaciones × 50
    individuos; timing mediana 10 samples / 2 warmups, proceso aislado; KAT
    cross-compile `g++ 13.3.0` y `clang++ 22.1.8` (bit-idéntico).
  - Artefactos: `benchmarks/MUTALAMBDA_KEECCAK_BENCHMARKS.md`,
    `benchmarks/results/results_cpp_keccak.json` y
    `benchmarks/targets/keccak256_optimized.hpp` en el repo de MutaLambda.
- Secrets: decoded once into fixed buffers, wiped with non-elidable volatile
  stores; HMAC keys held only as precomputed midstates.

---

## ⚠️ Known Limitations

See [`docs/STATUS.md`](docs/STATUS.md) for the live list. Highlights:
- Real-order acceptance on the production CLOB requires funded credentials —
  the signing/wire layer is verified offline (golden vectors + cross-check),
  but a live end-to-end order has **not** been placed by this repository's CI.
- Tick-size is a static config (`BOT_TICK_SIZE`); V2 exposes per-market tick
  via `getClobMarketInfo` — wire it for multi-market use.
- Certificate pinning is supported but off by default (`BOT_TLS_PIN`).

---

## 🛡️ Disclaimer

Educational/research software. Trading prediction markets involves substantial
risk. Not approved for production funds until the open items in
`docs/STATUS.md` are resolved by the operator.
