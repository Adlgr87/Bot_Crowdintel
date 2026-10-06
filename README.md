# CrowdIntel CLOB V2 execution engine

[![CI](https://github.com/Adlgr87/Bot_Crowdintel/actions/workflows/ci.yml/badge.svg)](https://github.com/Adlgr87/Bot_Crowdintel/actions/workflows/ci.yml)

A low-latency C++20 research and execution engine for Polymarket's CLOB V2. It combines authenticated alpha ingress, an L2 WebSocket book, exact fixed-point order construction, EIP-712 signing, a consumable pre-signed ladder, bounded SPSC queues, risk gates, and asynchronous HTTPS submission.

> **Deployment status:** offline and mock paths are tested. This repository is **not approved for unattended live trading**: private-channel fill/order reconciliation and automatic inventory recovery are still missing, and signature type 3 intentionally fails closed until correct ERC-7739 wrapping is implemented. See [the deployment runbook](docs/DEPLOYMENT.md) and [remediation ledger](docs/REMEDIATION_STATUS.md).

## Protocol reference

Cross-verified against the official [Polymarket CLOB V2 Python SDK](https://github.com/Polymarket/py-clob-client-v2) (`py_clob_client_v2`):

**Endpoints**

| Service | Host | Path |
|---|---|---|
| CLOB REST API (L2) | `https://clob.polymarket.com` | `POST /order` |
| CLOB server-time probe | `https://clob.polymarket.com` | `GET /time` |
| Market L2 WebSocket | `wss://ws-subscriptions-clob.polymarket.com/ws/market` | — |

Non-HTTPS schemes are rejected at configuration time; HTTP redirects are disabled.

**EIP-712 V2 signing domain (Polygon chainId 137)**

| Variant | Verifying contract |
|---|---|
| Standard CTF Exchange V2 | `0xE111180000d2663C0091e4f400237545B87B996B` |
| Negative-risk CTF Exchange V2 | `0xe2222d279d744050d28e00520010520000310F59` |

Domain separator: `keccak256(DOMAIN_TYPE_HASH ‖ keccak256("Polymarket CTF Exchange") ‖ keccak256("2") ‖ 137 ‖ verifyingContract)` — no watermark, `0x1901` prefix. The 11-field `Order` struct (`salt, maker, signer, tokenId, makerAmount, takerAmount, side, signatureType, timestamp, metadata, builder`) is ABI-encoded in 32-byte slots and hashed with Ethereum Keccak-256 (suffix `0x01`).

**L2 HMAC authentication**

Requests are signed with HMAC-SHA256 using the API secret decoded from base64url. The signing message is `timestamp + method + requestPath + body` where `timestamp` is **Unix seconds** (not milliseconds). The signature is base64url-encoded (with padding) and sent via the `POLY_SIGNATURE` header. Additional headers: `POLY_ADDRESS`, `POLY_TIMESTAMP`, `POLY_API_KEY`, `POLY_PASSPHRASE`. Private keys and credential files are zero-allocated on destruction; `mlockall` keeps signer/credential memory out of swap in live mode.

> Crypto KATs, golden-vector EIP-712 cross-checks, and independent Python/coincurve verification are part of CI (see [Verification](#verification)). Each cryptographic primitive has a known-answer test; the signer output is byte-for-byte cross-verified against an independent Python implementation using `pycryptodome` and `coincurve` for both standard and negative-risk domains.

## Safety model

The executable starts in mock mode unless built with network dependencies and invoked without `BOT_MODE=mock`. Live startup additionally requires `BOT_ENABLE_LIVE_TRADING=1`, credentials, market identity, and a valid alpha bearer token. Unknown ticks, stale books/signals, exhausted signatures, invalid responses, unsupported signature type 3, and active kill switches fail closed.

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

CI covers GCC and Clang, network/offline builds, ASan+UBSan, TSan, the production container/non-root identity, the independent signer reference, replay, and loose gross-regression latency budgets. The independent Python cross-check (`cross_check_v2.py`) verifies domain separator, struct hash, EIP-712 digest, recoverable-signature signer-recovery, and RFC 6979 determinism against the C++ output for both standard and negative-risk exchange domains.

## Mock run

```bash
BOT_MODE=mock BOT_TICKS=20 \
BOT_PRIVATE_KEY_HEX=23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b \
  build/bin/crowdintel_bot
```

This is the public KAT key used only for local deterministic startup. Never reuse it for funds.

## Documentation

- [Architecture and concurrency invariants](docs/ARCHITECTURE.md)
- [Configuration reference](docs/CONFIGURATION.md)
- [Deployment and rollback runbook](docs/DEPLOYMENT.md)
- [Security and threat model](docs/SECURITY.md)
- [Benchmark methodology](docs/BENCHMARKING.md)
- [Observation-by-observation remediation status](docs/REMEDIATION_STATUS.md)
