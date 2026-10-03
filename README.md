# CrowdIntel CLOB V2 execution engine

A low-latency C++20 research and execution engine for Polymarket's CLOB V2. It combines authenticated alpha ingress, an L2 WebSocket book, exact fixed-point order construction, EIP-712 signing, a consumable pre-signed ladder, bounded SPSC queues, risk gates, and asynchronous HTTPS submission.

> **Deployment status:** offline/replay paths are tested and paper shares the live data path without order egress. This repository is **not approved for unattended live trading**: private-channel fill/order reconciliation and automatic inventory recovery are still missing, and signature type 3 intentionally fails closed until correct ERC-7739 wrapping is implemented. See [the deployment runbook](docs/DEPLOYMENT.md) and [remediation ledger](docs/REMEDIATION_STATUS.md).

## Safety model

`BOT_MODE` is mandatory and names exactly one execution mode: `replay` (offline, synthetic feed, simulated fills), `paper` (live public market data and metadata, simulated fills, no order egress) or `live` (real orders). The historical `mock` value is rejected. `live` additionally requires `BOT_ENABLE_LIVE_TRADING=1`, L2 credentials, a journal, market identity, and a valid alpha bearer token; `paper`/`replay` reject the arming flag, the journal and the venue parameters that the market document provides. Unknown configuration names, unknown ticks, stale books/signals, exhausted signatures, invalid responses, unsupported signature type 3, and active kill switches fail closed.

An enqueued order is **not** an accepted order. Final semantic acceptance requires HTTP 2xx plus a successful CLOB response. Ambiguous timeouts are not retried blindly.

## Build

Requirements: CMake 3.20+, a C++20 compiler, pthreads, and libsecp256k1 built with the recovery module. Network builds also require OpenSSL and libcurl development packages.

```bash
# Example: build libsecp256k1 at the repository-pinned commit
export SECP_COMMIT=6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d
git clone --filter=blob:none https://github.com/bitcoin-core/secp256k1 /tmp/secp256k1
git -C /tmp/secp256k1 checkout --detach "$SECP_COMMIT"
cmake -S /tmp/secp256k1 -B /tmp/secp256k1/build \
  -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
  -DSECP256K1_BUILD_TESTS=OFF -DSECP256K1_BUILD_BENCHMARK=OFF
cmake --build /tmp/secp256k1/build -j

# Portable offline build
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=OFF -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Set `-DCROWDINTEL_NETWORK=ON` for the real HTTPS/WSS transport. Configuration fails if requested network dependencies are absent; it never silently produces a live-looking stub.

## Verification

```bash
# Native known-answer, unit/concurrency, and replay tests
ctest --test-dir build --output-on-failure

# Independent Python EIP-712/RFC6979 cross-check
python3 -m pip install pycryptodome==3.23.0 coincurve==21.0.0
build/bin/test_signer --json > /tmp/signer.json
python3 tests/crypto/cross_check_v2.py /tmp/signer.json
build/bin/test_signer --json-neg-risk > /tmp/signer-neg.json
python3 tests/crypto/cross_check_v2.py /tmp/signer-neg.json

# CPU-only benchmark (not an end-to-end/network SLO)
build/bin/latency_bench
```

CI covers GCC and Clang, network/offline builds, ASan+UBSan, TSan, the production container/non-root identity, the independent signer reference, replay, and loose gross-regression latency budgets.

## Replay and paper runs

```bash
# Deterministic offline replay: no socket is opened.
BOT_MODE=replay BOT_TICKS=20 \
BOT_PRIVATE_KEY_HEX=23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b \
  build/bin/crowdintel_bot

# Paper: live market data and metadata, simulated fills, no order egress.
BOT_MODE=paper BOT_TICKS=200 BOT_MARKET_SLUG=<slug> BOT_OUTCOME=Yes \
BOT_ALPHA_BEARER_TOKEN=<at least 16 printable chars> \
BOT_PRIVATE_KEY_HEX=<64 hex chars> \
  build/bin/crowdintel_bot
```

The key is the public KAT key used only for local deterministic startup (a
replay/paper key is never transmitted in replay and never produces a wire order
in paper). Never reuse it for funds.

## Documentation

- [Architecture and concurrency invariants](docs/ARCHITECTURE.md)
- [Configuration reference](docs/CONFIGURATION.md)
- [Deployment and rollback runbook](docs/DEPLOYMENT.md)
- [Security and threat model](docs/SECURITY.md)
- [Benchmark methodology](docs/BENCHMARKING.md)
- [Observation-by-observation remediation status](docs/REMEDIATION_STATUS.md)
