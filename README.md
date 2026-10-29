# Bot CrowdIntel — Polymarket CLOB V2 trading engine

High-frequency deterministic execution engine for Polymarket CLOB V2 on Polygon
(chainId 137), plus the three alpha strategies that were **measured** before any
of them was funded.

The defining property of this repository is that every strategic claim has a
number behind it, and the numbers are reproducible from scripts in `scripts/`.
Two of the three strategies came back **negative**. They are documented, not
deleted, because the negative results are what stopped $7,500 of planned capital
from being deployed without evidence.

---

## 1. What was measured, and what it decided

| Sleeve | Gate | Result | Verdict |
|---|---|---|---|
| **A · Crypto TWAP convergence** | `scripts/experiment_twap_edge.py` | Replica 88% accurate at T-10s vs market **100%**; market Brier **0.017**. 12 trades, 9 wins, **−1.001 u/c** | **Do not fund** |
| **B · Insider mirroring** | `scripts/experiment_copy_edge.py` | Wallet skill persists (**+11 pts**, n=19), but the copy loses **13–16¢ in 10 minutes**; model right on disagreement 33–50% | **Inconclusive** — only with crowdintel's own timestamps |
| **C · Maker carry** | `scripts/experiment_maker_edge.py` | **+0.392¢ median net per fill**, positive in **19 of 22** markets | **Build this one** |

Full write-ups: `docs/EXPERIMENT_MOTOR_A.md`, `docs/EXPERIMENT_MOTOR_B.md`,
`docs/EXPERIMENT_MOTOR_C.md`.

**Why C wins and A loses, with the same mechanism:** the adverse-selection term.
These markets resolve over weeks, so one-minute volatility is 0.05–0.6¢ against
a 0.5–2.0¢ half-spread. A five-minute crypto round is the opposite: the venue's
own probability is essentially settled in the final seconds, and the taker fee
is 7% on top.

---

## 2. Venue facts established from primary sources

Not from blog posts — from the venue's own documentation, the live API, and DNS.

| Fact | Source |
|---|---|
| CLOB V2 primary servers: **AWS `eu-west-2` (London)** | Official docs; corroborated by `mm.clob.polymarket.com` CNAME → `clob-mm-gateway-…elb.eu-west-2.amazonaws.com` |
| **GB is close-only on the API**, so hosting in London gets orders rejected | `docs.polymarket.com/api-reference/geoblock`, three-tier table |
| **IE is "the API itself is not restricted"** → `eu-west-1` (Dublin) | Same table; docs name it "Closest Non-Georestricted Region" |
| Crypto up/down markets settle on **Chainlink TWAP-60s** | Live market object: `cryptoMarketConfig.twapLookbackSeconds: 60`, `resolutionSource: …/btc-usd-twap-60s-streams` |
| Fee = `rate × p × (1−p)`, per category | Live `feeSchedule`: `{exponent:1, rate:0.07}` |
| **Makers pay nothing** | `feeSchedule.takerOnly: true`, verified per market |
| Rewards band is **fractional cents** (`rewardsMaxSpread: 3.5` = 350 bps) | Live `clobRewards` + `rewardsMaxSpread` |
| Binance WebSocket is **AWS Tokyo**, no EU endpoint | Resolved IPs mapped against AWS `ip-ranges.json` |

The practical consequence: **Dublin, one region, no dual-colo.** Running the
strategy in Tokyo and shipping the order to London costs the same 214 ms as
receiving the Tokyo feed in Dublin. That is physics, not engineering.

---

## 3. Safety — the parts that stop it losing money

Five layers, in order of activation. Each one closes a way to lose the bankroll
that was reachable from a perfectly valid environment file.

| Layer | Behaviour |
|---|---|
| **Run mode** | `BOT_MODE` unset or unrecognised **aborts**. An offline build refuses `live`. `paper` is accepted but reported as behaving as mock until a paper engine exists. |
| **Startup invariants** | A live session is refused when `daily_loss` is 0 (disables the kill), `stop_loss_pct` is 0 (disables the stop), any cap exceeds its bankroll fraction, or the buy-volume cap cannot fund two max-order orders. |
| **Durable daily state** | `daily_buy_volume` and the kill latch survive restarts. A **corrupt state file refuses startup in live** — not knowing today's exposure is worse than not trading. |
| **Monetary aliases** | `*_USDC` and `*_USD` both accepted; a **conflicting pair is a hard error**, never a silent precedence choice. A startup banner prints the effective limits. |
| **Reconciliation drift** | WS-vs-REST inventory drift latches the kill and stops trading. |

Two defects found while building this, both fixed:

* `BOT_MODE` was **fail-open**: in the production network build an unset
  `BOT_MODE` meant `mock = false`, i.e. real orders. Now every unknown path aborts.
* `BOT_MAX_DAILY_BUY_VOLUME_USD` was validated but **never read** — the gross BUY
  cap was pinned at $50 and could not be raised without recompiling.
* `utc_day_number()` returned `era * 36524 + doe` instead of the civil-from-days
  value, so it was neither the day count nor collision-free. A collision would
  make the daily loss cap miss its rollover.

---

## 4. Architecture

```
   Binance WS (Tokyo)          Polygon RPC              CLOB WS (London)
   12–214 ms                    on-chain                28–35 ms
        │                           │                        │
        ▼                           ▼                        ▼
   ┌──────────────────── SPSC rings, no allocation on the hot path ─────────┐
   │                                                                        │
   │  ExecutionEngine (core/src/execution_engine.hpp, 1000 lines)           │
   │    compliance → book freshness → volatility gate → Kelly sizing        │
   │    → fee-aware edge gate → risk authorization → wire body              │
   │                                                                        │
   │  Target: tick-to-decision p50 ≈ 754 ns, p99 ≈ 1.2 µs (measured)        │
   └────────────────────────────────────────────────────────────────────────┘
        │                    │                      │
        ▼                    ▼                      ▼
   OrderGateway       PositionTracker         Metrics  →  /metrics (Prometheus)
   (async egress)     (fills, inventory)      DailyState → disk (CRC, atomic)
```

**Hot path is single-threaded and allocation free.** Every other thread is cold:
market/user WebSocket listeners, the order gateway worker, the journal renderer,
the reconciler, the daily-state persister and the health server.

### Execution and risk, wired

`execution_engine.hpp` · `market_config.hpp` · `risk_manager.hpp` ·
`lightweight_client.hpp` (libcurl, persistent handle, TCP_NODELAY) ·
`order_gateway.hpp` · `presigned_pool.hpp` · `ws_market_listener.hpp` ·
`ws_user_listener.hpp` · `compliance_guard.hpp` · `position_tracker.hpp` ·
`volatility_gate.hpp` · `bayesian_engine.hpp` · `metrics.hpp` ·
`daily_state.hpp` · `market_metadata.hpp`

### Motor C maker path (built, awaiting the quote loop)

`maker_quote.hpp` (where to quote) · `quote_registry.hpp` (when to pull) ·
`resting_mock.hpp` / `resting_mock_client.hpp` (a transport where orders rest) ·
plus `entry_order_type` routing and `DELETE /order` in `lightweight_client.hpp`.

Entries may rest (`GTC`), exits are always `FAK` — a protective exit that merely
rests is an exit that did not happen. Resting entries bypass the pre-signed pool
because a pre-signed ladder assumes immediate consumption on a TTL, while a
resting order's lifetime is governed by cancel/replace.

### Motor A crypto path (built, gated negative)

`chainlink_stream.hpp` — strict Data Streams report parser plus a
**time-weighted** 60-second TWAP window. Arithmetic mean would be wrong: the
DON's cadence is uneven.

---

## 5. Fee model

One implementation, in `market_config.hpp`:

```cpp
taker_fee_per_share(rate, price) = rate * price * (1 - price)
```

| Category | rate |
|---|---|
| Geopolitics / world events | **0.00** |
| Politics / Finance / Tech / Mentions | 0.04 |
| Sports / Economics / Weather / Culture | 0.05 |
| Crypto | 0.07 |
| Unknown | 0.07 (fail-closed: assume the expensive one) |

`BOT_MARKET_CATEGORY` maps to the rate; `BOT_TAKER_FEE_RATE` overrides it.
Both consumers — the decision gate and the Kelly sizing — read the same field,
so they cannot disagree.

`fee_calculator.hpp` and `fee_calculator_v2.hpp` still exist but carry a
**DEAD CODE banner**: they implement a flat-tier table that does not match the
venue, and no production translation unit includes them. This matters: that
disconnected duplicate is what a reader reaches for first.

---

## 6. Build and test

```bash
# libsecp256k1 with the recovery module is a hard dependency
export SECP256K1_ROOT=/path/to/secp256k1
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release -DCROWDINTEL_NETWORK=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

**Current state: 35 of 36 CTest targets pass.** The single failure is
pre-existing and unrelated: `layer_units`'s "flow resumes after cooldown".
Suppression has two sources — a time-based shock cooldown that expires, and a
published regime pause that only recomputes on fresh book samples. That is a
conservative failure (it stops trading, it never loses money), and changing it
is a product decision, so it is left failing rather than silently "fixed".

Behaviour that was verified rather than assumed:

* `BOT_MODE` unset → `FATAL`, exit 1
* `BOT_ENTRY_ORDER_TYPE=LIMIT` → `FATAL`
* `BOT_MARKET_CATEGORY=geopolitics` → `fee=0.00000 source=category`
* restart → `[state] restored buy_volume=49.99 USD for UTC day 20755`
* `kill -9` then restart with a latched kill → `kill latch restored from disk`
* `BOT_ENTRY_ORDER_TYPE=GTC` + real slug → `[rewards] from venue: pool=$3000/day band=350bps min_size=200`
* SIGTERM → clean exit (this hung before the health server switched to `poll()`)

### Test suites added

| Suite | Assertions | Covers |
|---|---|---|
| `test_config_hardening` | 59 | run mode, monetary aliases, fee categories, live invariants |
| `test_daily_state` | 37 | CRC-32 vectors, atomic write, corruption, symlink refusal |
| `test_metrics` | 35 | decision histogram, edge/slippage means, Brier, NaN |
| `test_chainlink` | 46 | report parser, time-weighted TWAP, replay |
| `test_fees` | 25 | the production fee model (rewritten from the dead one) |
| `test_maker_quote` | 41 | placement, reward band, inventory skew, tick rounding |
| `test_quote_registry` | 37 | fills, supersede, price/age/size replace rules |
| `test_cancel_id` | 14 | order-id validation incl. JSON injection |
| `test_market_metadata` | 15 | fractional-cent band → bps |
| `test_resting_mock` | 30 | fills only on a cross |
| `test_resting_client` | 26 | OPEN vs FILL, cancel, malformed bodies |

---

## 7. Operational surface

```bash
# Region preflight — run this BEFORE signing any lease
./scripts/probe_region.sh                 # probe this host
./scripts/probe_region.sh --aws eu-west-1 # provision spot, probe, tear down

# Research gates (each reproduces one row of the table in §1)
python3 scripts/experiment_twap_edge.py     --asset btc --duration 15m
python3 scripts/experiment_copy_edge.py     --wallets 40
python3 scripts/experiment_maker_edge.py    --markets 22

# Dead-code policy (fails on a NEW orphan)
./scripts/check_dead_headers.sh
```

`probe_region.sh` exits **2** when the host is geo-blocked. That is the answer
that matters: a blocked region cannot trade no matter how fast it is.

Endpoints on `BOT_METRICS_PORT` (127.0.0.1, `0` disables):
`/health` `/ready` `/metrics` (Prometheus) `/status`. The series that decides
whether a strategy has alpha at all is `crowdintel_decisions_by_result` — the
labelled histogram of every verdict.

---

## 8. Configuration

Full reference: `docs/CONFIGURATION.md`. The ones that change behaviour:

| Variable | Default | Meaning |
|---|---|---|
| `BOT_MODE` | **required** | `mock` / `paper` / `live` |
| `BOT_ENABLE_LIVE_TRADING` | 0 | second key; live is disarmed without it |
| `BOT_MARKET_CATEGORY` | – | `geopolitics`/`politics`/`sports`/`crypto` |
| `BOT_ENTRY_ORDER_TYPE` | `FAK` | `GTC` for resting maker entries |
| `BOT_MK_ADVERSE_BPS` | 25 | **the maker sleeve's decisive number** |
| `BOT_MK_REWARDS_BAND_BPS` | from venue | overridden by `market_metadata` |
| `BOT_BANKROLL_USD` *(or `BOT_MAX_BANKROLL_USDC`)* | 1000 | both spellings, conflict = fatal |
| `BOT_MAX_DAILY_LOSS_USD` | 50 | refused at startup if 0 in live |
| `BOT_STATE_FILE` | `/var/lib/crowdintel/daily_state.bin` | durable risk state |
| `BOT_METRICS_PORT` | 8080 | 0 disables |

---

## 9. Known limitations — read before trusting anything

1. **No live order or fill has been claimed from this environment.** Every
   statement above is offline verification, public-API measurement, or a mock.
2. **No paper engine exists.** `BOT_MODE=paper` runs the mock transport:
   `MockCLOBClient` synthesizes a fill on every accepted submit, so it cannot
   observe a rest. `resting_mock_client.hpp` is the transport that can, and it is
   not yet driven by a quote loop.
3. **The Motor C conditional mark-out is unmeasured.** The +0.392¢/fill figure
   uses *unconditional* one-minute volatility. Adverse selection is conditional
   on being filled, which is strictly worse. If the real number is 2¢ rather than
   0.09¢, every net in that table flips negative. **This is the number that can
   still kill the only funded sleeve**, and it needs real fills.
4. **Motor A's result has a proxy gap.** The TWAP replica uses Binance VWAP, not
   Chainlink's own aggregate. Part of the accuracy difference could be proxy
   error. Closing it needs Data Streams credentials.
5. **Samples are small:** 17 and 26 settled rounds for Motor A, 19 wallets for
   Motor B, 22 markets for Motor C. Directionally consistent, not powered studies.
6. **`layer_units` fails** (see §6).

---

## 10. Repository map

```
core/src/          engine, config, fee model, metrics, feeds, maker path
core/include/      risk, book, SPSC rings, daily state, maker_quote, quote_registry
core/crypto/       EIP-712 + libsecp256k1 signing, keccak, ChaCha20 salts
infra/             Docker, systemd (SCHED_FIFO + CPU isolation), Prometheus, health
scripts/           region preflight, three research gates, dead-code guard
tests/             36 CTest targets: unit, integration, acceptance, replay
docs/              architecture, ADRs, three experiment write-ups, dead-code ledger
experimental/      (planned) frozen research code behind -DCROWDINTEL_BUILD_EXPERIMENTAL
```

Governing documents: `docs/STATUS.md` (what is verified vs not),
`docs/DEAD_CODE.md` (a decision for all 32 orphan headers),
`docs/EXPERIMENT_MOTOR_{A,B,C}.md` (the gates), `docs/ADR-*.md` (design records).

---

## 11. What is genuinely next

In dependency order, not in wish order:

1. **Wire the Motor C quote loop** — the only thing between this repository and
   its first real measurement. `plan_quotes` → `entry_order_type=GTC` →
   `quote_registry` → `cancel_order` already compose; nothing drives them.
2. **Measure the conditional mark-out** with `resting_mock_client` driving a mock
   book, then with a size-minimal live canary. This is the number that decides
   whether the sleeve is funded.
3. **`probe_region.sh --aws eu-west-1`** before any lease is signed.
4. Only then: capital, at a fraction of the originally planned allocation.

The planned $10,000 allocation is **not** justified by the evidence. Motor C's
positive result is real but its decisive risk term is unmeasured.
