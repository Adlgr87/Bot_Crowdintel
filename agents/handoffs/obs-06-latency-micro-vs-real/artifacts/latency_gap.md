# Latency Gap Analysis — OBS-06: Micro vs Real latency

## File: tests/benchmarks/latency_bench.cpp

```cpp
// Line 1-2:
// CPU-path microbenchmark. It deliberately excludes network/HMAC/TLS and
// labels results accordingly. Presigned slots are consumable, so the fixture
// rebuilds a fresh eight-size BUY ladder outside each measured batch.
```

### What IS measured:

| Component | p50 | p99 | Scope |
|-----------|-----|-----|-------|
| `run_tick()` bare hot-path | ~754ns | ~1.2μs | CPU only |
| Full layered tick (P1-P4 active) | ~754ns | ~2μs | CPU only |
| EIP-712 sign | ~100ns | — | CPU only |
| SPSC push/pop | ~50ns | — | CPU only |

**All measurements use `MockCLOBClient` — NO network included.**

### What is NOT measured:

| Component | Measured? | Notes |
|-----------|-----------|-------|
| DNS lookup | ❌ | Not instrumented |
| TCP handshake | ❌ | Not instrumented |
| TLS handshake | ❌ | Not instrumented |
| HTTP POST to CLOB | ❌ | Mock client returns instantly |
| CLOB server processing | ❌ | Not instrumented |
| HTTP 200/201 response | ❌ | Mock client simulates |
| End-to-end signal → fill | ❌ | Not instrumented |

### Hot path steps NOT in the microbenchmark:

1. WebSocket message arrives (network latency)
2. JSON parsing of L2 update (CPU + network delivery)
3. SPSC ring buffer push (CPU)
4. `run_tick()` (CPU — THIS IS MEASURED)
5. Order construction (CPU)
6. EIP-712 signing (CPU)
7. Presigned pool acquisition or inline signing (CPU)
8. HTTP POST via libcurl (NETWORK — NOT MEASURED)
9. CLOB server processes order (NETWORK + SERVER — NOT MEASURED)
10. HTTP response received (NETWORK — NOT MEASURED)
11. Response body parsing (CPU)
12. Journal event (CPU)

### LATENCY_BUDGET.md analysis

```
Total Hot-Path Budget: p50 < 15μs, p99 < 50μs
Current (bare): ~754ns p50, ~1.2μs p99
With P1-P4: ~754ns p50, ~2μs p99
```

**The "hot-path budget" only covers steps 4-7 above.** The network E2E (steps 1-3, 8-12) is NOT in the budget.

### README acknowledgment

> "This separation is an optimization boundary, not evidence that an order reaches the venue in sub-microsecond time."

✅ README acknowledges this, but LATENCY_BUDGET.md does not reflect the E2E reality.

## Conclusion

The p50/p99 of ~754ns - 2μs is **CPU-path only**. The end-to-end latency from signal arrival to CLOB response includes:
- Network RTT: ~5-50ms (depending on location)
- TLS handshake: amortized (connection pooling)
- HTTP POST: ~5-20ms (CLOB processing)
- Total E2E: **~10-70ms typical**, NOT microseconds

The <12μs hot-path metric is **misleading as a canary SLO** because it excludes the dominant cost: network and venue latency.
