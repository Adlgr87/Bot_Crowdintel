# Canary runbook — the two steps that need a human

Everything else is built and tested. These two need credentials and capital, so
they cannot be automated from a sandbox. Both are designed to be copy-paste.

---

## Step 8 — Confirm the region (15 min, no cost beyond a spot instance)

**Why it matters:** the CLOB lives in `eu-west-2` (London), but **GB is
close-only on the API**, so hosting there gets orders rejected. The docs name
`eu-west-1` (Dublin) as the closest *non-restricted* region. That claim must be
verified from an Irish IP before any lease is signed — the shipped `docs`
and the Help Center disagree about Ireland.

### Fastest path: AWS Cloud Shell (no local install)

Cloud Shell already has the AWS CLI and your credentials. Paste these two
commands; they provision a spot instance in Dublin, probe it, and destroy it.

```bash
# 1. Launch a throwaway spot instance in Dublin
aws ec2 run-instances --region eu-west-1 \
  --image-id resolve:ssm:/aws/service/canonical/ubuntu/server/22.04/stable/current/amd64/hvm/ebs-gp2/ami-id \
  --instance-type t3.micro --instance-market-options '{"MarketType":"spot"}' \
  --query 'Instances[0].InstanceId' --output text
```

```bash
# 2. Get its public IP, SSH in is not needed: use SSM
#    (replace i-XXXX with the id from step 1)
aws ec2 wait instance-running --region eu-west-1 --instance-ids i-XXXX
aws ssm send-command --region eu-west-1 --instance-ids i-XXXX \
  --document-name AWS-RunShellScript \
  --parameters 'commands=["curl -s https://polymarket.com/api/geoblock"]' \
  --query 'Command.CommandId' --output text
# then read it:
aws ssm get-command-invocation --region eu-west-1 \
  --command-id <id-from-previous> --instance-id i-XXXX \
  --query 'StandardOutputContent' --output text
```

```bash
# 3. Destroy it — do not skip this
aws ec2 terminate-instances --region eu-west-1 --instance-ids i-XXXX
```

### What the answer means

| Output | Meaning |
|---|---|
| `{"blocked":false,"country":"IE",...}` | **Dublin works.** Proceed to step 9. |
| `{"blocked":true,...}` | Dublin is restricted. Fallback order: **Zurich `eu-central-2`** (17 ms, CH unlisted) then **Stockholm `eu-north-1`** (30 ms, SE unlisted). |

### Or locally, if you prefer installing the CLI

```bash
pip install --user awscli && ./scripts/probe_region.sh --aws eu-west-1
```

The script prints the geoblock verdict, CDN→origin latency for the CLOB, WS,
Gamma, Data API and Binance, the Binance HTTP 451 check, and kill-switch
writability. **Exit 2 means geo-blocked: do not deploy.**

---

## Step 9 — Measure the conditional mark-out (3–5 days)

**This is the only number still missing, and it decides the capital.** The
measured `+0.392c` net per fill uses *unconditional* one-minute volatility.
Adverse selection is conditional on being filled — and a fill happens precisely
when the price moved against the maker. If the real figure is 2c rather than
0.09c, **every net in `docs/EXPERIMENT_MOTOR_C.md` flips negative**.

### Before the canary: pick the market well

The sleeve earns carry + half-spread. Rank candidates by:

```bash
python3 scripts/experiment_maker_edge.py --markets 22
```

Pick a row where `spread/2` is comfortably above `adverse 1m` and the pool is
non-zero. The best measured example was `democrats-win-texas-senate`:
1.00c half-spread against 0.081c adverse.

Then read that market's own parameters (they override your env):

```bash
curl -s "https://gamma-api.polymarket.com/markets?limit=1&slug=<SLUG>" | python3 -m json.tool | \
  grep -E "orderPriceMinTickSize|rewardsMaxSpread|rewardsMinSize|feeSchedule"
```

### Environment (live maker, minimal size)

```bash
export BOT_MODE=live
export BOT_ENABLE_LIVE_TRADING=1                    # second key, required
export BOT_MK_LIVE_ACK=I_ACCEPT_NO_INVENTORY_SKEW   # names the remaining gaps

export BOT_MARKET_SLUG=<chosen-slug>
export BOT_ENTRY_ORDER_TYPE=GTC                     # resting maker, fee 0

# Capital: START AT THE MINIMUM THAT QUALIFIES. Not more.
export BOT_BANKROLL_USD=500
export BOT_MAX_DAILY_LOSS_USD=10                    # must be <= 5% of bankroll
export BOT_MAX_ORDER_USD=100
export BOT_MAX_EXPOSURE_USD=200
export BOT_MAX_PORTFOLIO_EXPOSURE_USD=300
# MUST exceed 2x a single quote or the volume cap blocks every BUY:
# quote = rewardsMinSize shares x price, so ~200 x 0.45 = 90 -> use 300
export BOT_MAX_DAILY_BUY_VOLUME_USD=300

export BOT_MK_ADVERSE_BPS=25                        # the decisive cushion
export BOT_MK_QUOTE_SIZE=200000000                  # 200 shares, the venue minimum
export BOT_MK_REWARDS_MIN_SIZE=200000000
export BOT_MK_MAX_INVENTORY=400000000               # 400 shares
export BOT_MK_MAX_AGE_MS=30000                      # refresh a quote at 30s

export BOT_STATE_FILE=/var/lib/crowdintel/daily_state.bin
export BOT_METRICS_PORT=8080
export BOT_METRICS_BIND=127.0.0.1                   # keep metrics off the internet
```

Credentials: `CLOB_API_KEY`, `CLOB_SECRET`, `CLOB_PASSPHRASE`, and a
`BOT_PRIVATE_KEY_HEX_FILE` holding the 64 hex chars (mode 0600, no symlink).

### What to watch — and what "good" looks like

```bash
curl -s localhost:8080/metrics | grep -E "crowdintel_maker_total|crowdintel_decisions"
```

| Series | Healthy | Bad sign |
|---|---|---|
| `…{event="quoted"}` | climbing steadily | flat 0 → see the next row |
| `…{event="volume_cap"}` | 0 | **> 0 means a BUY was refused by the cap: raise `BOT_MAX_DAILY_BUY_VOLUME_USD`** |
| `…{event="band_too_narrow"}` | 0 | the venue band cannot cover `BOT_MK_ADVERSE_BPS` |
| `…{event="risk_refused"}` | occasional | constant → caps or kill latched |
| `…{event="inventory_full"}` | occasional | constant → one-sided, no inventory |
| `…{event="venue_refused"}` | 0 | tick mismatch or wire problem |

**Abort immediately if** any of these appear: `risk_kill_switch` in `/status`,
the kill-switch file exists, `venue_refused` exceeds a handful, or `/ready`
reports not ready.

### Computing the mark-out (while the tracker is not yet built)

The decisive number is the price move **after** each fill, signed against the
position. Until `MarkoutTracker` lands, take it from the journal:

1. Every fill is journalled as `ACCOUNT_FILL` with price and size.
2. For each fill, note the book mid at fill time and again N minutes later
   (60s minimum; the experiment used one minute).
3. `adverse = (mid_later − fill_price) × sign` where `sign = +1` for a BUY.
   Positive means the price moved **against** you — the cost you are measuring.
4. Report the **mean of that quantity over fills**, not over ticks. A market can
   look calm unconditionally and be brutal conditional on being filled; that gap
   is the whole point of the canary.

### Decision rule, fixed before the run so the result cannot be rationalised

| Mean adverse per fill | Action |
|---|---|
| **< 0.5c** | the sleeve works as measured — fund it at the planned fraction |
| **0.5c – 1.5c** | marginal: `BOT_MK_ADVERSE_BPS` must rise and the band must still fit; re-run |
| **> 1.5c** | **stop.** The half-spread does not cover the fills; the sleeve does not work |

Write the result into `docs/EXPERIMENT_MOTOR_C.md` whichever way it goes. A
negative result there is worth more than another week of engineering.

---

## What remains true regardless of the outcome

The engine, the risk layers and the maker path are built and tested
(`ctest` 39/40, the single failure pre-existing and documented). What no amount
of code can substitute for is this: **a fill is the only honest measure of what
a maker costs.**
