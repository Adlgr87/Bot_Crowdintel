# Motor A gate — TWAP-60s convergence edge experiment

**Status: NEGATIVE.** The crypto sleeve is not funded as specified.

Run: `python3 scripts/experiment_twap_edge.py --asset btc --duration <5m|15m> --lead <s>`

## What was tested

Polymarket's crypto up/down markets settle on a 60-second Chainlink TWAP.
Verified live from the Gamma market object, not from a blog post:

```
cryptoMarketConfig: {"id":"btc-15m-twap-60","twapEnabled":true,"twapLookbackSeconds":60}
resolutionSource:   https://data.chain.link/streams/btc-usd-twap-60s-streams
feeSchedule:        {"exponent":1,"rate":0.07,"takerOnly":true,"rebateRate":0.2}
```

Chainlink publishes that report 1-2 s after DON consensus, so the report is the
**label**, not a signal. The hypothesis was: reconstruct the TWAP-60s locally
(Binance 1m VWAP as the proxy series, conditional mean projected to the close)
and know the direction before the market finishes repricing.

## Results

Decision taken `lead` seconds before the close. "Model right on disagreement"
is the only column that matters: when the model agrees with the market there is
no trade to make.

### BTC 15m (n = 17 settled rounds)

| lead | model acc | market acc | market Brier | disagreements | model right |
|---|---|---|---|---|---|
| 120 s | 82.4% | 82.4% | 0.1183 | 4 | **2/4 (50%)** |
| 60 s | 88.2% | 82.4% | 0.0796 | 3 | 2/3 (67%) |
| 30 s | 88.2% | 88.2% | 0.0555 | 2 | **1/2 (50%)** |
| 10 s | 88.2% | **100%** | **0.0172** | 2 | **0/2 (0%)** |

### BTC 5m (n = 26 settled rounds)

| lead | model acc | market acc | market Brier | disagreements | model right |
|---|---|---|---|---|---|
| 60 s | 76.9% | 80.8% | 0.1396 | 3 | **1/3 (33%)** |
| 10 s | 76.9% | 76.9% | 0.1319 | 2 | **1/2 (50%)** |

### Realised PnL of the naive rule (follow the model when edge > fee)

Every configuration loses money. BTC 5m at T-10s, the largest sample:

| margin | trades | won | PnL (u/contract) |
|---|---|---|---|
| 0.00 | 12 | 9 | **-1.001** (-0.083/trade) |
| 0.01 | 10 | 7 | **-1.011** (-0.101/trade) |
| 0.02 | 9 | 6 | **-1.030** (-0.115/trade) |

Nine wins out of twelve trades and still negative: the wins land at high prices
(small payoff) and the losses at low prices. That is the adverse-selection
signature of buying into a market that has already moved.

## Reading

1. **The market improves faster than the replica.** From T-120s to T-10s the
   market's Brier falls 0.118 -> 0.017 while the model's accuracy plateaus at
   88%. By the last ten seconds the market is essentially perfect and there is
   nothing left to take.
2. **Where the model disagrees it is a coin flip or worse** (33-50% across five
   measurements). That is the definition of noise, not edge.
3. **Chainlink's own 1-2 s DON latency does not create a window**, because the
   window that exists in the final seconds is already priced by the venue.
4. Fee at p=0.50 is 1.75c on a 50c contract (3.5% of capital), so a rule must be
   right ~51.75% *net*. The model does not clear that bar even gross.

## Caveats (stated so the result is not over-read)

* Small sample: 17 and 26 settled rounds. Directionally consistent across both
  samples and four horizons, but not a powered study.
* The Binance 1m VWAP is a **proxy** for Chainlink's own aggregate. Part of the
  12-point accuracy gap could be proxy error rather than a market advantage.
  Resolving that requires Data Streams credentials and a real TWAP replica from
  the underlying `btc-usd` stream, which is the only version of this experiment
  worth re-running.
* `lead` is measured from the close, not from the DON's publication instant.
  Sub-second arbitrage inside the last second is untested here and cannot be
  tested without the colo.

## Decision

**Do not fund Motor A on this evidence.** The $3,500 planned for the crypto
sleeve stays unallocated until either (a) a real Chainlink-stream TWAP replica
closes the proxy gap, or (b) a sub-second study against the colo shows a
different result. Move that allocation to Motor B (crowdintel insider
mirroring), whose gate is still unmeasured and whose fee burden is 0-4%
instead of 7%.
