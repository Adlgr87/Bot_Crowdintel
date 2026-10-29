# Motor B gate — is wallet skill copyable?

**Status: INCONCLUSIVE-LEANING-NEGATIVE for naive mirroring.** Wallet selection
is not pure noise, but the copy that a public observer can execute captures
nothing. Motor B is only fundable with a signal materially better than "top
wallet by PnL", i.e. exactly what crowdintel claims to sell — and that claim
now has a measurable test.

Run: `python3 scripts/experiment_copy_edge.py --wallets 40 --trade-samples 20`

## Part 1 — persistence of skill

Source: `/v2/user-pnl?interval=max&fidelity=1d` (daily series of realised market
PnL per wallet). Split at 2026-07-07, chosen to maximise wallets with real data
on both sides. Scale-invariant contingency test, so account size does not skew it.

| Metric | Value |
|---|---|
| wallets with a usable series | 31 |
| wallets active on both sides of the split | 19 |
| base rate, later period | **52.6%** |
| P(profitable later \| profitable before) | **63.6%** (n=11) |
| P(profitable later \| lost before) | **37.5%** (n=8) |
| lift of selecting on history | **+11.0 points** |
| later PnL of the "good" wallets | **+6,047,005 USD** |
| later PnL of all wallets | +610,222 USD |

So the wallets that won before were ~10x more profitable in aggregate afterwards,
and the sign of their early period predicts the sign of their late period about
63% of the time against a 52.6% base. **That is a real but modest signal**, and
n=19 is small. It is not enough on its own: a 63% chance of a wallet being
profitable in a period says nothing about whether you can capture *its* edge by
copying a specific trade.

## Part 2 — what the copier actually gets

Sample: 20 recent BUY trades by those wallets. Drift = the same token's price
`horizon` after the insider's fill, in cents.

| horizon | n | median drift | mean drift |
|---|---|---|---|
| 60 s | 20 | **+0.00c** | −0.71c |
| 180 s | 20 | −0.50c | −2.29c |
| 600 s | 20 | **−16.50c** | −14.46c |

The median at 60 s is exactly zero and the mean is negative. Ten minutes later
the price is **13-16 cents lower**. Mirroring these BUYs means buying into a
position that immediately marks against you, then paying a 0-4% fee on top.

This is the central tension of Motor B, stated precisely:

> **Their PnL is real (Part 1) and the copy is negative (Part 2).** The gap is
> the difference between *owning* an edge — accumulated over days of positioning,
> inventory management, and possibly LP rewards (`realized_lp_pnl` is a separate
> field in the same payload) — and *observing a single fill seconds later*.

## Method notes (a wrong turn worth recording)

The first version scored wallet skill from `/v2/positions` using
`total_pnl > 0`. It reported a **0.4-3.4% base rate and −99% PnL per dollar**,
which is impossible for a leaderboard of profitable traders. Cause: `/positions`
returns *holdings*, and a resolved market is asymmetric — winners get redeemed
and collapse out of the list, losers stay behind as worthless tokens. That is a
survivorship inversion, so that measurement was discarded, not reported.
`/v2/user-pnl` is the correct source because it is a cumulative PnL series.

## Caveats

* n=19 wallets with activity on both sides. Directionally consistent with the
  copy-lag result but not a powered study.
* `prices-history` has 1-minute fidelity, so "60 s" is really "the next minute
  bucket". Sub-minute decay is unmeasured, and is where a better detector could
  still win.
* This measures **naive top-wallet copying**, not crowdintel's curated output.
  The result is a floor on difficulty, not a ceiling on their product.

## Decision

**Do not fund Motor B on public data alone.** The +11-point lift is real but the
copy physics eat it. Two conditions before allocating capital:

1. Crowdintel's own signals must be tested with this same harness, using their
   wallet selection and timestamps. If their hit rate on the *copied* trade — not
   the wallet's period profitability — does not clear the fee, the sleeve is dead
   regardless of how good the forensics are.
2. Any candidate signal must clear the Part 2 drift: the entry it implies has to
   beat `|drift| + fee`, measured on their actual alert latency.

## What this changes in the portfolio

Both measured sleeves are now unfunded:

| Sleeve | Plan | Gate result |
|---|---|---|
| A · crypto convergence | $3,500 (35%) | **NEGATIVE** — `EXPERIMENT_MOTOR_A.md` |
| B · insider mirroring | $4,000 (40%) | **INCONCLUSIVE / negative for naive copying** |
| C · maker carry | $1,500 (15%) | not yet tested — and now the only untested sleeve |

`$7,500 of the planned $10,000 has no measured edge behind it.` The honest next
step is not to allocate; it is to test Motor C, whose economics do not depend on
prediction at all (fee 0% + rebate), and to re-run Motor B against crowdintel's
real feed before it sees a dollar.
