# Motor C gate — does the maker earn more than it costs?

**Status: POSITIVE on the measured terms, and the only sleeve that is.**
One number that decides it is still unmeasured, and the doc says exactly which.

Run: `python3 scripts/experiment_maker_edge.py --markets 22 --hours 72`

## Why this sleeve is structurally different

Motors A and B both needed a prediction to be right. Motor C does not. Its
income is three mechanical terms, each visible in public data:

| Term | Source | Measured |
|---|---|---|
| **Liquidity rewards** | `clobRewards[].rewardsDailyRate` per market, paid daily to makers quoting inside `rewardsMaxSpread` with at least `rewardsMinSize` shares | $30 – $3,000 per market per day |
| **Spread capture** | quoting at the touch earns the spread on a round trip; half of it is the fair share of a fill | 0.5 – 2.0c per share |
| **Adverse selection** | you are filled precisely when the price is moving against you | 0.02 – 0.6c per minute |

Yield per dollar is `pool / qualifying capital`, and qualifying capital is
exactly the book depth inside the reward band — measured directly from `/book`.

## Results (22 markets with active rewards and a measurable book)

| slug | pool/day | qual. capital | carry %/day | spread/2 | adverse 1m | net/fill |
|---|---|---|---|---|---|---|
| us-announces-end-of-iranian-blockade | 400 | 27,785 | 1.44% | 0.50c | 0.166c | **+0.334c** |
| will-netanyahu-be-next-pm | 3,000 | 233,382 | 1.29% | 0.50c | 0.575c | −0.075c |
| will-gadi-eizenkot-be-next-pm | 1,000 | 118,099 | 0.85% | 0.10c | 0.376c | −0.276c |
| will-there-be-no-change-in-fed-rates | 1,000 | 191,978 | 0.52% | 0.50c | 0.140c | **+0.360c** |
| republicans-win-michigan | 600 | 137,829 | 0.44% | 0.50c | 0.079c | **+0.421c** |
| republicans-win-texas | 900 | 417,841 | 0.22% | 0.50c | 0.085c | **+0.415c** |
| fed-increase-25bp | 1,000 | 771,901 | 0.13% | 0.50c | 0.117c | **+0.383c** |
| democrats-win-texas-senate | 900 | 793,044 | 0.11% | 1.00c | 0.081c | **+0.920c** |
| republican-party-control-house | 1,500 | 2,170,393 | 0.07% | 0.50c | 0.065c | **+0.435c** |
| 2026-balance-of-power-d-senate-d-house | 2,500 | 5,729,505 | 0.04% | 0.50c | 0.048c | **+0.452c** |

| Summary | Value |
|---|---|
| median carry | **0.11% per day** |
| median spread/2 | **0.50c** |
| median adverse (1 min) | **0.092c** |
| **median net per fill** | **+0.392c** |
| markets with net > 0 | **19 / 22 (86%)** |

## Why it works here and not in crypto

The adverse-selection term is the entire story. On these political and
Fed-decision markets, one-minute realised volatility is **0.05–0.6 cents**,
because the underlying event resolves over weeks, not seconds. The half-spread
being captured is **0.5–2.0 cents**. So the cost is an order of magnitude below
the income.

Compare with Motor A: a 5-minute crypto round has a half-spread of maybe 0.5c
against a venue whose Brier drops to 0.017 within the last ten seconds — the
market there is faster than any maker, and the fee is 7% on top. **Same
mechanism, opposite conclusion, and the difference is the horizon of the
underlying event.**

Two design consequences from the docs:

* Maker rebates are funded by taker fees, so **a 0%-fee market pays no rebate** —
  only liquidity rewards. Fee category trades off two income streams.
* Eligibility requires quoting inside `rewardsMaxSpread` with `rewardsMinSize`
  shares, which sets a **minimum capital per market** (e.g. 200 shares × 2 sides
  × 0.445 = $178 for one market).

## The one unmeasured number (and it is decisive)

`adverse 1m` is **unconditional** volatility. Adverse selection is
**conditional on being filled**, and being filled is exactly when the move is
against you. A market maker's real cost is therefore strictly worse than the
0.092c median measured here.

This is not a caveat to wave away: if the conditional adverse move on fills is
2 cents instead of 0.09, every net in the table flips negative. Resolving it
needs either L2 book history or a live canary measuring actual mark-out on real
fills. **It cannot be settled from 1-minute price data.**

The second unmeasured term is the **fill rate**. The carry column assumes about
one fill per side per minute, which is generous; a lower fill rate does not
change carry income but does change how much capital must sit idle.

## Decision

**Motor C is the sleeve to build.** It is the only one of the three where the
measured terms are positive, its income does not require predicting anything,
and its risk is a single tunable number (`adverse_sel_bps`) rather than a
forecast.

Conditions before capital:

1. **Measure mark-out on real fills.** A canary quoting small size for a few
   days gives the conditional adverse move directly. This is the number that can
   still kill the sleeve, and it needs live fills, not history.
2. The engineering prerequisite is real and not yet done: the bot is **100%
   taker today** (`order_type = "FAK"`). Resting GTC entries must exist before a
   single maker experiment can run.

## Portfolio after all three gates

| Sleeve | Plan | Gate | Verdict |
|---|---|---|---|
| A · crypto convergence | $3,500 (35%) | NEGATIVE — market beats the replica at every horizon | do not fund |
| B · insider mirroring | $4,000 (40%) | skill persists (+11 pts) but the copy loses 13-16c in 10 min | fund only with crowdintel's own timestamps clearing the fee |
| **C · maker carry** | $1,500 (15%) | **+0.392c/fill median, 19/22 markets positive** | **build first** |

The plan's original allocation is now inverted. The smallest sleeve is the only
one with evidence behind it, which is exactly what measuring is for.
