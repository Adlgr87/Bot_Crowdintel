# Architecture Map — Bot_Crowdintel Hot Path

## Hot-Path Flow (per tick)

```
WebSocket L2 (Polymarket)          WebSocket User Channel (private)
     │                                      │
     ▼                                      ▼
 WsMarketListener                  WsUserListener
     │ parses L2 delta stream             │ parses fills/orders
     ▼                                      ▼
 OrderBookL2 (memmove seqlock)    PositionTracker + AccountEvents
     │
     ▼  (SPSC ring, cap=4096)
 AlphaSignal (external or alpha_parser)
     │
     ▼
 ExecutionEngine::run_tick()  ← RDTSC instrumented
     │
     ├─ 1. ComplianceGuard (jurisdiction + restricted tokens, fail-closed)
     ├─ 2. Book stale check (atomic timestamp, ~90s threshold)
     ├─ 3. Rate limiter (bucket counters, O(1))
     ├─ 4. KellyEngine → fractional sizing
     ├─ 5. RiskManager authorize (P2: exposure caps, stop-loss, kill latch)
     ├─ 6. VolatilityGate (P3: slippage gate, pre-sign rejection)
     ├─ 7. EIP-712 V2 sign (keccak256 domain + structHash, RFC6979)
     ├─ 8. Order construction (ceil_to_quantum for BUY, fee-inclusive notional)
     └─ 9. HTTPS submit (async via pre-signed pool or curl POST)
```

## Layered Architecture (P1–P4)

| Layer | Files | Thread | Responsibility |
|-------|-------|--------|----------------|
| P1 Eyes | `account_events.hpp`, `position_tracker.hpp` | Cold (WSS user) | Private channel accounting, reconciliation, kill-switch on drift |
| P2 Brakes | `risk_manager.hpp`, `kill_switch.hpp` | Hot | Exposure caps, stop-loss VWAP, day-loss kill, hedge coordination |
| P3 Adverse Sel. | `volatility_gate.hpp` | Hot | Slippage gate, dynamic pre-sign ladder TTL, >5%/100ms shock cooldown |
| P4 Brain | `bayesian_engine.hpp`, `evidence.hpp`, `evidence_ingress.hpp` | Cold + Hot | Beta-Binomial posterior, NDJSON evidence ingestion, ~41 ns update |

## New Module Integration Points

The new modules (PHASE-1–6) insert into the existing flow as follows:

| New Module | Inserts At | Hot/Cold | Latency Budget |
|------------|-----------|----------|----------------|
| `BinanceWSClient` | Cold thread (new) | Cold | N/A (reconn + parse < 50μs/msg) |
| `OFICalculator` | Between BinanceWS → SPSC → P4 drain | Cold produce → Hot consume | < 2μs hot-path |
| `CfCNetwork` | Hot drain (step 4, before Kelly) | Hot | < 3μs p50, < 5μs p99 |
| `WindowShield` | Hot (new layer between stale-check and sizing) | Hot | < 200ns p50 |
| `SpikeDetector` | Hot (after SPSC drain, before order) | Hot | < 1μs p50 |
| `KellySizer` | Hot (replaces KellyEngine::kelly_buy/sell) | Hot | < 500ns |
| `TWAPTracker` | Hot (incremental, per tick) | Hot | < 1μs p50 |
| `LadderSkew` | Hot (before order construction) | Hot | < 3μs p50 |

## Data Structures

### SPSC Ring Buffer
- Template: `SPSC_RingBuffer<T, Capacity>` (power-of-2, trivially copyable)
- Used for: AlphaSignal → ExecutionEngine drain, Evidence → Brain drain
- New ring: MarketState → ExecutionEngine drain (PHASE-1 → P4)

### EngineLayers (execution_engine.hpp)
- Contains pointers to P1–P4 sub-objects
- All layer structs must be trivially destructible (no heap in hot path)
- `run_tick()` is O(1), zero-alloc, no std::cout, no network I/O

## Latency Baseline

| Component | p50 | Notes |
|-----------|-----|-------|
| `run_tick()` bare hot-path | ~754ns | Measured in `latency_bench.cpp` |
| Full layered tick (P1–P4 active) | ~754ns | Budget: < 50μs p99 |
| EIP-712 sign | ~100ns | secp256k1 with NEON/SSE |
| SPSC push/pop | ~50ns | cache-line aligned |

## New Module Budget Allocation

| Module | Budget | Rationale |
|--------|--------|-----------|
| CfC inference | < 5μs p50, < 5μs p99 | 32 hidden units, SSE/AVX2 |
| OFI calculation | < 2μs p50, < 4μs p99 | O(1) incremental, no heap |
| WindowShield state | < 200ns p50 | Timer check + state table lookup |
| SpikeDetector | < 1μs p50 | Ring buffer compare + flag |
| Kelly sizing | < 500ns p50 | Simple arithmetic |
| TWAP tracker | < 1μs p50 | Accumulators only |
| Ladder skew | < 3μs p50 | 16-order pool construction |
| **TOTAL new overhead** | < 12μs p50 | Leaves 38μs headroom to 50μs |

**Fallback**: If CfC exceeds 5μs → degrade to cached linear model.
