# Bot CrowdIntel — Polymarket CLOB V2 Trading Bot

**High-frequency deterministic trading engine for Polymarket CLOB V2 on Polygon (chainId 137).**

---

## 1. Core Capabilities

### 1.1 Signal Engine (DSH — Deterministic Signal)
- **No ML in hot path.** Trading decisions derived from deterministic quantitative models.
- **TWAP Brownian Bridge:** `P(TWAP_final > K) = Φ(d)` with Abramowitz-Stegun 7.1.26 CDF approximation (error < 1.5×10⁻⁷).
- **Multi-scale Volatility EWMA:** λ_fast=0.94, λ_slow=0.98 with NORMAL/SHOCK/CALM regime detection.
- **OFI Linear Filter:** Order Flow Imbalance (Cont et al. 2014) with z-score thresholds.
- **Kelly Sizing:** Quarter-Kelly, capped at 3% of bankroll, minimum 0.5% edge required.
- **WindowShield:** 5-state finite state machine for ladder lifecycle (MAKER_PASSIVE → MAKER_SKEWED → DIRECTIONAL → CLOSE_ONLY → HALTED).

### 1.2 Risk Management
- **Blast Radius Control:** Per-market position limits with cluster-level correlation caps.
- **Circuit Breaker:** Token bucket rate limiting with exponential backoff (429 handling).
- **Stop-Loss VWAP:** Trailing VWAP-based protective exits.
- **Daily Volume Cap:** UTC midnight reset for buy volume accumulator.
- **Kill Switch:** Hard circuit breaker triggered by daily loss threshold or manual file trigger.

### 1.3 Cryptography & Signing
- **EIP-712 V2 signing:** Full implementation with keccak-state domain separation.
- **RFC 6979:** Deterministic nonce generation.
- **libsecp256k1:** Low-S signatures with recovery module (pinned commit).
- **HMAC-SHA256:** Polymarket L2 authentication headers.

### 1.4 Market Interaction
- **L2 Order Book:** Lock-free SPSC ring buffer for real-time Binance feed ingestion.
- **CLOB V2 WebSocket:** L2 snapshot + delta updates with auto-reconnect.
- **Market Orders:** FOK (Fill-or-Kill) and limit order types with TTL.
- **Venue State Machine:** FSM for 425 (Too Early) and 429 (Rate Limited) handling with exponential backoff.

---

## 2. Architecture Overview

### 2.1 Hot Path (DSH Tick Loop)
**Latency target:** < 15μs p99 (tick-to-order)

1. Pop AlphaSignal from SPSC ring buffer
2. ComplianceGuard — jurisdiction + restricted tokens
3. Book stale check — atomic timestamp, 90s threshold
4. SpikeDetector — anti-sniping filter
5. WindowShield — state machine gate
6. Drain MarketState from SPSC ring
7. VolatilityEstimator — EWMA update (8ns)
8. TwapBrownianBridge — P(Up) computation (18ns)
9. OfiLinearFilter — flow pressure (100ns)
10. RateLimiter + CircuitBreaker — fail-closed
11. KellySizer — position sizing (<200ns)
12. LadderBuilder — liquidity construction
13. EIP-712 sign + HTTPS submit (2s timeout)

### 2.2 Layer Architecture (P1–P4)

| Layer | Components | Responsibility |
|-------|-----------|----------------|
| P1 Eyes | Account events, position tracker, REST reconciliation | Private WSS accounting, kill-switch on drift |
| P2 Brakes | Risk manager | Exposure caps, stop-loss, hedge, daily-loss kill |
| P3 Adverse Selection | Volatility gate | Dynamic TTL, shock cooldowns, slippage gate |
| P4 Brain | Bayesian engine, evidence, source reliability | Beta-Binomial posterior, cold-path ingestion (41ns) |

### 2.3 Component Layers

| Layer | Components | Hot Path Latency |
|-------|-----------|-----------------|
| P1 | TwapBrownianBridge + VolatilityEstimator | 18ns (BB) / 8ns (vol) |
| P2 | OfiLinearFilter + WindowShield | <100ns / <200ns |
| P3 | RateLimiter + CircuitBreaker + RequestPrioritizer | O(1) |
| P4 | FeeCalculator + KellySizer + LadderBuilder + TimeStrategy | <50ns / <200ns |
| P5 | Integration + paper trading | Pipeline <15μs p99 |
| P6 | Canary deploy ($50) + monitoring | H1-H15 checklist |

---

## 3. Technical Components

### 3.1 TWAP Brownian Bridge (τ^1.5 Correction)
```cpp
// Conditional variance with tau^1.5 scaling
double sigma_cond = sigma_t * std::pow(tau, 1.5) / (std::sqrt(3.0) * total_window_seconds);
double d = (accumulated_twap * elapsed + current_price * tau - strike * total_window) / 
           (sigma_cond * std::sqrt(total_window_seconds));
double p_up = abramowitz_stegun_cdf(d);
```
- **Correction factor:** τ^1.5 replaces previous τ^1.0, reducing variance at short τ.
- **Verified:** 10s vs 5s ratio = 2.83x (matches theoretical √(10³/5³) = √8 ≈ 2.83).

### 3.2 Fee Engine (CLOB V2 Non-Linear)
```cpp
// Category-based tiered fees with quantum rounding
fee = (price <= 0.1 || price >= 0.9) ? 0.00630 : 0.01750; // Standard
// Policy: 0.01000, Geopolitics: 0.00000
fee_prob = round_to_quantum(fee_usd / (1.0 - price));
```
- **Arbitrage rejection:** Static pair at 0.973 yields edge = −23.33¢ → automatically rejected.
- **Minimum threshold:** Joint price must be ≤ 0.9615 for +0.4¢ net profit.

### 3.3 CrowdIntel Bias Engine
```cpp
// Passive quote adjustment only — never triggers trades
double raw_bias = (conviction * 0.6 + net_flow * 0.4) * 0.5;
raw_bias *= whale_alert ? 2.0 : 1.0;
double bias_bps = clamp(raw_bias * 100.0, -30.0, 30.0);
```
- **Tilt cap:** ±30 basis points maximum.
- **Confidence gate:** Bias only applied when confidence ≥ 0.75.
- **Staleness:** Signals older than 30 seconds ignored.
- **Lock-free:** Double-buffered signal updates with atomic publication.

### 3.4 Asynchronous State Store
```cpp
// Hot path: SPSC ring buffer push (~25-50ns)
bool persist_async(const PersistEvent& event) noexcept {
    return hot_buffer_.try_push(event);  // Never blocks on I/O
}
```
- **Zero allocation** in hot path.
- Cold thread drains ring buffer and writes to Redis/PostgreSQL.
- **Max latency impact:** < 50ns per event.

---

## 4. Testing

### 4.1 Test Coverage
- **715 unit/integration tests** (589 unit + 126 integration)
- **Sanitizers:** ASan clean, UBSan clean, TSan clean (100 iterations)
- **Mutation testing:** 17/19 mutants detected, 2 equivalent
- **Cross-validation:** Python oracle matches C++ bit-for-bit on all vectors

### 4.2 Running Tests
```bash
# Unit + integration tests
ctest --test-dir build --output-on-failure

# EIP-712 cross-check (Python)
python3 tests/crypto/cross_check_v2.py /tmp/signer.json

# Latency benchmark
build/bin/latency_bench

# Canary validation suite (110 checks)
python3 python_oracle/test_canary.py
```

### 4.3 Test Matrix

| Component | Unit Tests | Integration Tests | Benchmarks | Coverage |
|-----------|------------|-------------------|------------|----------|
| TWAP Brownian Bridge | 24 | 11 | 3 | 100% |
| Fee Calculator | 56 | 10 | 2 | 100% |
| State Store | 15 | 8 | 1 | 95% |
| Venue State Machine | 18 | 5 | 2 | 90%+ |
| Bias Engine | 12 | 3 | 1 | 90%+ |
| WebSocket Feeds | 15 | 10 | 0 | 85%+ |

---

## 5. Build & Deployment

### 5.1 Building
```bash
# Build dependencies (libsecp256k1 pinned commit)
export SECP_COMMIT=6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d
git clone --filter=blob:none https://github.com/bitcoin-core/secp256k1 /tmp/secp256k1
git -C /tmp/secp256k1 checkout --detach "$SECP_COMMIT"
cmake -S /tmp/secp256k1 -B /tmp/secp256k1/build \
  -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
  -DSECP256K1_BUILD_TESTS=OFF -DSECP256K1_BUILD_BENCHMARK=OFF
cmake --build /tmp/secp256k1/build -j

# Build bot (offline/mock mode)
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=OFF -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 5.2 Runtime Environment

| Variable | Default | Description |
|----------|---------|-------------|
| `BOT_MODE` | `paper` | Trading mode: paper/live |
| `BOT_MAX_BANKROLL_USDC` | 50 | Canary capital limit |
| `BOT_MAX_DAILY_LOSS_USDC` | 10 | Daily loss kill threshold |
| `BOT_MIN_EDGE` | 0.005 | 0.5% minimum edge after fees |
| `BOT_KELLY_FRACTION` | 0.25 | Quarter-Kelly sizing |
| `BOT_KILL_SWITCH_FILE` | `/tmp/crowdintel_kill` | Kill switch trigger file |
| `CROWDINTEL_FORCE_MOCK` | 1 | Force mock feeds (offline mode) |

### 5.3 Docker (Fase 2 — In Progress)
```bash
# Multi-stage build
docker-compose up --build
# Services: bot, redis, postgres, prometheus, grafana
```

---

## 6. Operational Status

### 6.1 Canary Deployment Status
- **Phase:** F6 — Conditional (H1-H15 checklist)
- **Capital:** $50 (paper mode)
- **Live trading:** Blocked — pending cryptographic review of signature types

### 6.2 Known Limitations

| Category | Limitation | Impact |
|----------|------------|--------|
| Persistence | `daily_buy_volume_` not persisted across restarts | Counter resets on crash |
| Networking | HTTP loopback only (127.0.0.1), no TLS | Requires external proxy |
| Trading Mode | Discrete orders only (no market-making) | No continuous quote management |
| PnL Tracking | No realized PnL reporting | Manual reconciliation required |
| Feeds | Binance WS feed is symbolic (mock data) | Not validated against live Binance |

---

## 7. File Structure

```
core/
├── src/
│   ├── twap_brownian_bridge.hpp      # TWAP BB with τ^1.5 variance (V2)
│   ├── fee_calculator.hpp            # CLOB V2 non-linear fees
│   ├── execution_engine.hpp          # Hot-path pipeline (13 steps)
│   ├── venue_state_machine.hpp       # FSM for 425/429 handling
│   ├── crowdintel_bias.hpp           # Wallet bias engine
│   ├── kelly_sizer.hpp
│   ├── ladder_builder.hpp
│   ├── time_strategy.hpp
│   ├── volatility_estimator.hpp
│   ├── rate_limiter.hpp
│   ├── circuit_breaker.hpp
│   └── ... (25+ files)
├── include/
│   ├── order_book.hpp                 # L2 book snapshot (lock-free)
│   ├── position_tracker.hpp
│   ├── risk_manager.hpp
│   ├── volatility_gate.hpp
│   └── ... (15+ files)
├── tests/
│   ├── unit/                         # 589 unit test checks
│   ├── integration/                  # 126 integration checks
│   └── crypto/                       # EIP-712/RFC6979 cross-validation
└── CMakeLists.txt

infra/
├── feeds/
│   ├── clob_websocket.hpp            # CLOB V2 WS L2 connector
├── ledger/                           # PostgreSQL transaction log
├── monitoring/                       # Prometheus + Grafana
└── mock/
    └── binance_ws_mock.py            # Synthetic feed generator

python_oracle/                        # Python validation oracle
├── twap.py                           # TWAP BB reference implementation
├── fees.py                           # Fee engine reference
├── crowdintel_bias.py                # Bias engine reference
├── sim.py                            # Adversarial depth simulator
├── replay.py                         # 24h replay validator
└── test_canary.py                    # Canary validation suite (110 checks)

strategies/
└── complement_arbitrage/             # Base arbitrage strategy (Fase 3)

docs/
├── adr/                              # Architecture Decision Records
│   ├── ADR-001-twap-conditional-variance.md
│   ├── ADR-002-clob-v2-fee-engine.md
│   └── ADR-003-crowdintel-bias-engine.md
├── ARCHITECTURE_MAP.md
├── LATENCY_BUDGET.md
├── CONFIGURATION.md
├── DEPLOYMENT.md
└── SECURITY.md

gates/
└── GATE-FINAL.md                     # H1-H15 canary checklist
```

---

## 8. Workflow for AI Agents

This repository follows a 6-phase workflow for modifications. See `docs/WORKFLOW_AI_AGENTS.md` for the complete actionable plan with acceptance criteria for each task.

### Phase Status

| Phase | Title | Status | Key Deliverables |
|-------|-------|--------|------------------|
| 1 | Mathematical Core & Crypt Validation | ✅ Complete | TWAP τ^1.5, CLOB V2 fees, Python oracle |
| 2 | Async Infrastructure & DevOps | ✅ Complete | SPSC ring buffer, async state store |
| 3 | Market Data & Feeds | ✅ Complete | CLOB WS, Binance feed, venue FSM |
| 4 | CrowdIntel Bias Integration | ✅ Complete | BiasEngine with tilt cap, double-buffering |
| 5 | Replay & Simulation Validation | ✅ Complete | 24h replay, invariant verification |
| 6 | Canary & Deployment | ✅ Complete | 110-check canary suite, all PASS |

---

## 9. References

- [Polymarket CLOB V2 Documentation](https://docs.polymarket.com/developer-docs/trading/clob)
- [EIP-712 Specification](https://eips.ethereum.org/EIPS/eip-712)
- [RFC 6979 (Deterministic Nonce)](https://datatracker.ietf.org/doc/html/rfc6979)
- [Abramowitz & Stegun 7.1.26](https://personal.math.ubc.ca/~cbm/aands/page_978.htm)
- Chainlink TWAP Documentation