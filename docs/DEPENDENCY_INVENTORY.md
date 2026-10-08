# Dependency Inventory

## Build System

| Item | Version | Notes |
|------|---------|-------|
| CMake | ≥3.16 | Required (tested on 3.20+) |
| C++ Standard | C++20 | `CMAKE_CXX_STANDARD=20`, no extensions |
| Compiler flags | `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -fno-plt -fvisibility=hidden` | Release: `-O3` |
| CPU targets | portable, x86-64-v2, x86-64-v3, native | `-march=` flag, portable = baseline |

## External Libraries

| Library | Version | Source | Used By |
|---------|---------|--------|---------|
| libsecp256k1 | commit `6e2c8bc4` (pinned) | bitcoin-core/secp256k1 | EIP-712 signing, recovery |
| OpenSSL | any (dev) | system | HTTPS/WSS TLS (network=ON only) |
| libcurl | 7.x (dev) | system | HTTP/WSS transport (network=ON only) |

**NOTA**: No ML frameworks (ONNX, TF, PyTorch) are available. The CfC network
must be hand-implemented in C++20. SIMD via SSE4.2/AVX2 intrinsics.

## Internal Components

| Component | Header | Depends |
|-----------|--------|---------|
| `SPSC_RingBuffer` | `spsc_ring_buffer.hpp` | C++20 atomics |
| `OrderBookL2` | `order_book.hpp` | seqlock, micro-allocations |
| `PositionTracker` | `position_tracker.hpp` | atomic, fixed arrays |
| `RiskManager` | `risk_manager.hpp` | atomic, PositionTracker |
| `VolatilityGate` | `volatility_gate.hpp` | atomic, config |
| `BayesianEngine` | `bayesian_engine.hpp` | Beta-Binomial conjugate |
| `EvidenceIngress` | `evidence_ingress.hpp` | BetaBinomial, source reliability |
| `WsMarketListener` | `ws_market_listener.hpp` | libcurl, OrderBookL2 |
| `WsUserListener` | `ws_user_listener.hpp` | libcurl, PositionTracker |
| `LightweightCLOBClient` | `lightweight_client.hpp` | libcurl, HMAC-SHA256 |
| `ExecutionEngine` | `execution_engine.hpp` | ALL above |
| `PresignedPool` | `presigned_pool.hpp` | EIP-712 signer, fixed array |
| `EIP712Signer` | `eip712_signer.hpp` | secp256k1, keccak256 |

## SIMD Support

| Feature | Present? | Where |
|---------|----------|-------|
| SSE4.2 | Yes (portable builds via intrinsics) | `keccak256.hpp`, `sha256_engine.hpp` |
| AVX2 | Conditional (`-march=x86-64-v3+`) | Not currently used in hot path |
| AES-NI | Available | Not used (HMAC uses SHA-256) |

## Environment Variables (BotConfig)

Critical ones for new modules:
- `BOT_MAX_BANKROLL_USDC` — position sizing ceiling
- `BOT_MAX_POSITION_SIZE` — per-market share cap
- `BOT_WINDOW_SHIELD` — enable/disable window state machine
- `BOT_SPIKE_DETECTOR` — enable/disable Binance spike detection
- `BOT_CFC_MODEL_PATH` — path to model binary (PHASE-2)
- `BOT_CANARY_LIVE_TRADING` — must be 1 for live mode (fail-closed otherwise)
