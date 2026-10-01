# Configuration reference

Configuration is read once at startup. Numeric parse errors, unsupported ticks, out-of-range values, oversized strings, insecure live URL schemes, and unknown modes are rejected; they never silently fall back to a trading default.

## Secrets

Every secret accepts either its normal environment variable or a `_FILE` variant, never both. File input is preferred: it must be an owner-readable (`0400`/`0600`), regular, non-symlink file with no group/other permissions. No terminator, LF, and CRLF are supported; empty/unreadable/oversized input is rejected, temporary buffers are zeroed, and the environment entry containing a successfully loaded path/value is removed.

| Secret | File form | Purpose |
|---|---|---|
| `BOT_PRIVATE_KEY_HEX` | `BOT_PRIVATE_KEY_HEX_FILE` | 32-byte signer key as exactly 64 hex characters |
| `CLOB_API_KEY` | `CLOB_API_KEY_FILE` | L2 credential identifier/owner |
| `CLOB_SECRET` | `CLOB_SECRET_FILE` | L2 HMAC secret |
| `CLOB_PASSPHRASE` | `CLOB_PASSPHRASE_FILE` | L2 passphrase |
| `BOT_ALPHA_BEARER_TOKEN` | `BOT_ALPHA_BEARER_TOKEN_FILE` | Alpha HTTP bearer; minimum 16 characters |

The deployment unit maps root-only files through systemd `LoadCredential=`. Do not put secrets in `/etc/crowdintel/config`, command-line arguments, repository files, container layers, or CI logs.

## Arming and identity

| Variable | Default | Notes |
|---|---:|---|
| `BOT_ENABLE_LIVE_TRADING` | `0` | Must equal `1` for live startup. Set last during preflight. |
| `BOT_MODE` | live transport | Set `mock` for deterministic local execution. |
| `BOT_MARKET_SLUG` | required live | Alpha routing identity. |
| `BOT_TOKEN_ID` | test vector in mock | Decimal uint256 outcome token ID. |
| `BOT_NEG_RISK` | `0` | Selects the negative-risk exchange domain when `1`. Must match market metadata. |
| `BOT_SIGNATURE_TYPE` | `0` | `0` EOA, `1/2` proxy/safe identities, `3` rejected until ERC-7739 exists. |
| `BOT_MAKER_ADDRESS` | signer for type 0 | Required for type 1/2; 20-byte hex address. |
| `BOT_API_ADDRESS` | signer | Address that owns the supplied L2 credentials. |
| `BOT_ORDER_TYPE` | `FAK` | One of `GTC`, `GTD`, `FOK`, `FAK`. |
| `BOT_GTD_TTL_SECONDS` | `0` | Required for GTD; accepted range is 120 seconds through one year. |
| `BOT_TICK_SIZE` | `0.01` | Initial tick. Dynamic WSS changes supersede it; unsupported ticks invalidate trading. |

## Risk and strategy

| Variable | Default | Meaning |
|---|---:|---|
| `BOT_BANKROLL_USD` | `1000` | Kelly denominator/reference capital. |
| `BOT_KELLY_FRACTION` | `0.10` | Fractional Kelly multiplier `(0,1]`. |
| `BOT_MIN_EDGE` | `0.02` | Minimum model edge. |
| `BOT_MIN_CONFIDENCE` | `0.85` | Minimum alpha confidence. |
| `BOT_MAX_Q_VALUE` | `0.05` | Maximum alpha q-value. |
| `BOT_TAKER_FEE_RATE` | `0.07` | Fee-rate coefficient. Fetch current market/category value before arming. |
| `BOT_MAX_ORDER_USD` | `100` | Per-order worst-cost cap. |
| `BOT_MAX_EXPOSURE_USD` | `250` | Aggregate reserved exposure cap. |
| `BOT_MAX_DAILY_LOSS_USD` | `50` | Circuit-breaker threshold. |
| `BOT_MIN_SIZE_SHARES` | `5` | Venue minimum/order floor in human shares. |
| `BOT_INITIAL_POSITION_SHARES` | `0` | Operator-reconciled starting inventory; human shares. Never infer this casually. |
| `BOT_PRESIGN_TTL_MS` | `3000` | Maximum age of generated ladder entries; for GTD it must be shorter than the configured GTD lifetime. |
| `BOT_SIGNAL_TTL_MS` | `2000` | Alpha freshness window. |
| `BOT_MAX_BOOK_AGE_MS` | `3000` | Market-book freshness window. |

A conservative default is not a verified venue value. Tick, fee schedule, minimum size, negative-risk status, token ID, balances, allowances, open orders, and inventory must come from current market/account metadata during preflight.

## Eyes — accounting and reconciliation (P1)

| Variable | Default | Meaning |
|---|---:|---|
| `BOT_RESERVATION_TTL_MS` | `10000` | Unconfirmed local reservations are released after this age (unfilled maker orders, dead venues). |
| `BOT_RECONCILE_INTERVAL_SEC` | `30` | Periodic REST positions reconciliation; `0` disables. |
| `BOT_RECONCILE_MAX_DRIFT_SHARES` | `0.01` | Max tolerated \|REST−local\| inventory drift; larger drift restates state and latches the kill switch. |

## Brakes — risk manager (P2)

| Variable | Default | Meaning |
|---|---:|---|
| `BOT_STOP_LOSS_PCT` | `0.15` | Stop-loss: liquidation-mark drop vs VWAP entry that closes the position. `0` disables. |
| `BOT_HEDGE_TRIGGER_PCT` | `0` | Earlier trigger that buys the complement token (`BOT_HEDGE_TOKEN_ID`). Must fire strictly before the stop-loss; `0` disables. |
| `BOT_MAX_PORTFOLIO_EXPOSURE_USD` | `250` | Portfolio-wide exposure cap enforced pre-signature. |

## Adverse selection — volatility gate (P3)

| Variable | Default | Meaning |
|---|---:|---|
| `BOT_POOL_MAX_DEV_BPS` | `200` | Max deviation of an order's price vs the CURRENT mid; a stale pool slot or toxic spread is refused. `0` disables. |
| `BOT_POOL_VOL_TTL_MS` | `500` | Pre-signed ladder TTL while the regime is volatile. |
| `BOT_VOL_MAX_SPREAD_BPS` | `1500` | Spread width (bps of mid) that flags a volatile regime. |
| `BOT_VOL_MAX_TICKS_PER_SEC` | `200` | Mid-change rate (Hz) that flags a volatile regime. `0` disables. |
| `BOT_VOL_MID_GAP_BPS` | `500` | Mid jump arming the 100 ms shock window; cooldown suppresses passive flow for 250 ms. |
| `BOT_VOL_SIZE_MULTIPLIER` | `0.5` | Passive size scale while the regime is volatile (halved again when extreme). |

## Brain — Bayesian edge engine (P4)

| Variable | Default | Meaning |
|---|---:|---|
| `BOT_BAYES_ENABLE` | `1` | Bayes brain active (prior seeding, evidence drain, posterior trigger). `0` cold-paths everything; the hot tick then only bounds-empties the evidence queue. |
| `BOT_BAYES_SOURCES` | empty | `id:weight` pairs, e.g. `"1:0.9,2:0.5"`. Weight = source reliability floor for evidence gating; at least one valid pair enables Beta-Binomial slots per source. |
| `BOT_BAYES_RECAL_FILE` | empty | Hot-reloaded reliability table (same `id:weight` format), re-read by the evidence replayer thread. |
| `BOT_EVIDENCE_FILE` | empty | NDJSON evidence replay/tail input. Lines: `{"source":1,"kind":"count","n":32,"k":22,"hash":N}` (binomial counts, `k<=n`) or `{"source":3,"kind":"lr","lr":693147,"hash":N}` (log-LR ×1e6). `hash` ≠ 0 required (dedup). |
| `BOT_BAYES_PRIOR_STRENGTH` | `24.0` | N₀ pseudo-count of the market mid as prior: α=N₀·mid, β=N₀·(1−mid). |
| `BOT_BAYES_SIGNAL_THRESHOLD` | `0.03` | Emit a synthetic signal when \|posterior − executable side\| exceeds this and reliability clears the floor. |
| `BOT_BAYES_MIN_RELIABILITY` | `0.35` | Evidence below this source weight is journaled `BAYES_LOW_RELIABILITY` and never updates the posterior. |

Paper trading (`BOT_MODE=mock`) synthesizes venue fills for every accepted mock order into the account queue, so tracker, exposure, stop-loss and brain observe the same flow they will see live. The mock feeds a tight, continuously refreshed book (~100 bps execution slip, mid pinned) alternating BUY/SELL hints.

## Transport and runtime

| Variable | Default | Notes |
|---|---:|---|
| `CLOB_HOST` | `https://clob.polymarket.com` | HTTPS origin; non-HTTPS is rejected. |
| `WS_HOST` | official market WSS | Market-channel endpoint; non-WSS is rejected. |
| `BOT_TLS_PIN` | empty | Optional libcurl public-key pin (`sha256//...`). Requires a rotation process. |
| `BOT_ALPHA_BIND` | `127.0.0.1` | Bind alpha ingress to loopback unless a trusted proxy/network design says otherwise. |
| `BOT_ALPHA_PORT` | `8088` | Alpha HTTP port. |
| `BOT_PIN_CPU` | `-1` | Hot-loop CPU; best effort. |
| `BOT_COLD_CPU` | `-1` | Network/replenisher CPU; best effort. |
| `BOT_KILL_SWITCH_FILE` | `/tmp/crowdintel.kill` | Existing file disables enqueue/egress and triggers shutdown; unsent entries are discarded. Production uses `/run/crowdintel/kill`. |
| `BOT_TICKS` | `0` | Stop after N hot-loop ticks; `0` means unlimited. Useful for mock tests. |

Start from [`infra/config/production.env.example`](../infra/config/production.env.example). Unknown variables are ignored by the process, so configuration changes require peer review and a startup-log check for the effective non-secret identity/risk values.
