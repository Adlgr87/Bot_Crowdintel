# 🏛️ Architecture — Bot CrowdIntel (CLOB V2)

## 1. Design principles

- **Deterministic hot path**: no heap allocation, no syscalls, no logging,
  no unbounded loops between "signal seen" and "order on the wire".
- **Single market focus**: one configured `tokenId` ⇒ everything expensive
  (decimal token string, hex addresses, domain separator, typehash, HMAC
  midstates, TLS session) is computed once at startup and cached.
- **Precompute everything the signal hasn't seen yet**: the pre-signed pool
  moves ECDSA *off* the critical path entirely.

## 2. Topology

```
                         ┌────────────────────────────── COLD PATHS ─────────┐
                         │                                                    │
 wss://…/ws/market ──► WsMarketListener ── seqlock ──► OrderBookL2            │
 (RFC 6455 + TLS,        (thread)                                 │            │
  hand-rolled)                                                    │            │
 CrowdIntel webhook ─► AlphaParser ── SPSC ──► [signals]          │            │
 (FDR q, confidence)    (thread)                              │            │
 Presign thread ──────► PresignedOrderPool ── atomic flip ────────┤            │
 (±8 ticks × both sides, exact-Kelly bucket, TTL refresh)         │            │
                         └────────────────────────────────────────┼────────────┘
                                                                  ▼
                     ExecutionEngine (pinned core, spin/park)
                     filters (edge, liquidity, size) → Kelly →
                     pool hit (~90 ns) | inline ECDSA (~24 µs) →
                     wire body (fixed buffers) → HMAC midstates
                                                                  │
                                                                  ▼
                     LightweightCLOBClient — persistent TLS,
                     POST /order with POLY_* L2 headers
                                                                  │
                                                                  ▼
                     clob.polymarket.com  (CLOB V2)
```

## 3. The hot path, step by step (per tick)

1. `SPSC::try_pop(signal)` — ~10 ns, wait-free.
2. Statistical filters re-check (q, confidence, p_win sanity) — a few ns.
3. `OrderBookL2::read_top()` — seqlock-guarded best bid/ask snapshot; up to
   4 retries, else `NO_BOOK`.
4. Direction: signal hint, else the side with the larger edge.
5. Economic filters: `edge = p_win − ask` (BUY) below `min_edge` ⇒ skip.
6. Sizing: exact Kelly `f = (w−p)/(1−p)` × fraction cap × bankroll ÷ price,
   floor to ×1e6, clamp to visible level size, enforce min size.
7. Amounts: `__int128` product, 6-decimal raw units, tick-rounded price.
8. **Pool first**: linear scan (≤16 slots) for `(side, price, size)` with a
   fresh timestamp → memcpy of a ready wire body (~50 ns).
   Miss ⇒ inline: build `OrderV2` (fresh RDRAND salt + wall-clock ms),
   ABI-encode (stack), Keccak ×3, libsecp256k1 recoverable sign (~24 µs on a
   2.1 GHz shared vCPU; ~2–5 µs on tuned bare metal), fill the wire template.
9. `client.submit(body)`: timestamp (s) + `HMAC-SHA256` over
   `ts + "POST" + "/order" + body` via precomputed midstates (~0.3 µs) →
   headers + POSTFIELDS on the persistent libcurl handle → `curl_easy_perform`.

## 4. Thread & concurrency model

| Thread | Role | Sync primitive |
| :--- | :--- | :--- |
| WSS listener | feed → book producer | seqlock (odd/even counter) |
| Presign | pool rebuild + flip | `atomic<uint32_t>` release/acquire |
| Webhook/alpha ingest | signal producer | SPSC head/tail release/acquire |
| Engine (pinned) | consumer, submitter | reads only; `pause`/nanosleep park |

No locks anywhere. No atomics on data (only on counters/indices). All shared
structures are single-writer.

## 5. Build

CMake, C++20. `-O3 -funroll-loops -fno-plt -fvisibility=hidden` +
optional `-march=native` (`CROWDINTEL_MARCH_NATIVE`, default ON) + LTO.
The network layer (curl/OpenSSL) is optional (`CROWDINTEL_NETWORK=OFF`
builds tests/bench/backtester without any network library).

## 6. Deployment posture

- Bare metal with `isolcpus` for the engine core (`infra/scripts/kernel_tuning.sh`
  applies universal sysctls everywhere and GRUB isolation only where writable).
- Deterministic Docker build: `infra/docker/Dockerfile.prod`.
- Secrets only via environment; optional TLS pinning; secret buffers wiped.
