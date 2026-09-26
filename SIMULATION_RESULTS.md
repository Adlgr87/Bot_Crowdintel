# 📊 Paper Trading Simulation Results

## Overview

The simulation exercises the CrowdIntel bot's risk pipeline against mock Polymarket CLOB V2
market data. All trades are simulated — no real credentials or network I/O required.

## Test Matrix (9 configurations)

| ID | Ticks | Markets | Seed | Capital | Noise | Fill Rate | P&L | Key Finding |
|---|---|---|---|---|---|---|---|---|
| T1 | 500 | 3 | 42 | $100k | on | 5.0% | -$7.81 | Good fill rate with fewer markets |
| T2 | 2000 | 5 | 42 | $100k | on | 3.6% | $0.00 | P&L scales to zero |
| T3 | 5000 | 8 | 42 | $100k | on | 3.5% | -$4.08 | Many markets = exposure limit |
| T4 | 1000 | 5 | 42 | $100k | off | 0.0% | $0.00 | Static prices exhaust capital |
| T5 | 2000 | 15 | 7 | $100k | on | 4.0% | -$256.18 | Highest loss, too many markets |
| T6 | 500 | 2 | 7 | $100k | on | 8.8% | $0.00 | Best fill rate (fewer markets) |
| T7 | 1000 | 10 | 999 | $100k | on | 6.9% | -$26.81 | Different seed, higher fill |
| T8 | 10000 | 5 | 42 | $100k | on | 0.8% | $0.00 | Large scale = exposure cap |
| T9 | 1000 | 10 | 42 | $1M | on | 2.2% | -$38.35 | More capital = more blocked |

## Key Findings

### 1. Exposure Limit is the Primary Bottleneck
- Risk/exposure blocks account for **70-85%** of all blocked ticks in high-scale runs
- The 20% capital cap is very conservative — consider relaxing to 50% for paper trading

### 2. Fill Rate Inversely Correlates with Scale
- **Few markets (2-3)**: 5-9% fill rate (more opportunities per market)
- **Many markets (10-15)**: 2-4% fill rate (signals spread thinner)
- **Long runs (10k ticks)**: 0.8% fill rate (exposure cap exhausted)

### 3. No-Noise Mode is Degenerate
- Without price movement, static prices cause exposure limit to block ALL signals
- This validates the noise/walk logic — book must move to generate opportunities

### 4. P&L Stability
- P&L ranges from **-$256.18 to $0.00** — the simulation doesn't generate alpha
- **Mean reversion exits only trigger 50% of the time** — most positions don't close
- The slight negative P&L in some runs is due to exit prices being worse than entry

### 5. Stale Book Detection Works
- 49-1087 stale books blocked across runs
- Consistent detection every 200 ticks (feed death simulation)

## Risk Management Validation

| Check | Status | Notes |
|-------|--------|-------|
| Entry band (35-70¢) | ✅ | All fills within range |
| Book staleness (90s) | ✅ | 24-1087 blocked per run |
| Exposure limit (20%) | ✅ | 287-8603 blocked per run |
| EV threshold ($0.02) | ✅ | 45-349 filtered per run |
| Kill switch | ✅ | Available but off by default |

## Recommendations

1. **Relax exposure limit** from 20% to 50% of capital for simulation
2. **Increase exit probability** from 50% to 75% for better P&L
3. **Reduce `min_ev_threshold`** from $0.02 to $0.01 for more opportunities
4. **Add position-level exposure tracking** (not just total portfolio)

## How to Run

```bash
# Build
g++ -std=c++20 -O2 -Icore/include tests/sim/paper_trading_main.cpp -o bin/paper_sim -lpthread

# Run with defaults (1000 ticks, 5 markets, seed=42)
./bin/paper_sim

# Custom parameters
./bin/paper_sim --ticks=5000 --markets=10 --seed=42 --capital=1000000
```
