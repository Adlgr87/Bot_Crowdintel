# Configuration reference

Configuration is read once at startup. Numeric parse errors, unsupported ticks, out-of-range values, oversized strings, insecure live URL schemes, an absent or unknown mode, and **any `BOT_`/`CLOB_`/`GAMMA_`/`WS_` variable this build does not read** are rejected; they never silently fall back to a trading default.

## Modes

`BOT_MODE` is mandatory and has exactly three values. There is no default and
the historical `mock` value is rejected with a migration hint: `mock` conflated
"deterministic offline replay" with "paper trading", which are different
promises about what can reach the venue.

| Capability | `replay` | `paper` | `live` |
|---|---|---|---|
| Market data | synthetic local feed | live public WSS + metadata resolution | live public WSS + metadata resolution |
| Orders | simulated locally | simulated locally | signed and sent |
| `BOT_PRIVATE_KEY_HEX` | required (never leaves the process) | required (signature is never transmitted) | required |
| `CLOB_API_KEY`/`SECRET`/`PASSPHRASE` | optional (validated and wiped if present) | optional (validated and wiped if present) | required |
| `BOT_ALPHA_BEARER_TOKEN` | optional | required (alpha ingress) | required |
| `BOT_LEDGER_*` | rejected | rejected | `BOT_LEDGER_PATH` required |
| `BOT_ENABLE_LIVE_TRADING=1` | rejected | rejected | required |
| Venue fixtures in ENV (`BOT_TICK_SIZE`, `BOT_MIN_SIZE_SHARES`, `BOT_TAKER_FEE_RATE`, `BOT_NEG_RISK`, `BOT_TOKEN_ID`) | allowed | rejected | rejected |
| Selector/hosts | slug defaults to `mock-market` | slug or condition id, https/wss required | slug or condition id, https/wss required |

The order journal is venue evidence, so only a live run may own one: pointing a
replay or paper run at the production journal is rejected instead of writing
simulated orders into the file a later live run reconciles. An offline build
(compiled without curl/OpenSSL) only has the replay transports, so it forces
`replay` and rejects `paper`/`live`.

The tools are narrower than the trader: `crowdintel-metadata` (internally the
inspector role) reads no secret, requires `BOT_MODE=paper|live`, and is the way
to validate the non-secret config file; `crowdintel-preflight` (the tool role)
requires `BOT_MODE=live`, reads the L2 credentials and **refuses to start if
`BOT_PRIVATE_KEY_HEX(_FILE)` is present at all**.

### Validating a deployed file

`crowdintel_bot --check-config` reads and validates the whole configuration
exactly as startup would — including the strict unknown-name check — and exits
`0` (`CONFIG OK mode=… market=… armed=… ledger=…`) or `1` with the same message
startup would print, **without opening a socket**. `crowdintel-metadata
--check-config` (inspector role: no secret) and `crowdintel-preflight
--check-config` (tool role: L2 credentials, never the signing key) do the same
for their narrower roles. Use them before restarting a service with a new
environment file.

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
| `BOT_MODE` | none | `replay`, `paper` or `live`; mandatory, no default, `mock` is rejected. |
| `BOT_MARKET_SLUG` | required live* | Market selector and alpha routing identity. |
| `BOT_CONDITION_ID` | empty | Alternative selector: `0x` + 64 lowercase hex condition id. |
| `BOT_OUTCOME` | empty | Outcome label (`Yes`/`No`/…), matched case-insensitively. Required for multi-outcome markets; single-token markets may omit it. |
| `BOT_SIGNATURE_TYPE` | `0` | `0` EOA, `1/2` proxy/safe identities, `3` rejected until ERC-7739 exists. |
| `BOT_MAKER_ADDRESS` | signer for type 0 | Required for type 1/2; 20-byte hex address. |
| `BOT_API_ADDRESS` | signer | Address that owns the supplied L2 credentials. |
| `BOT_ORDER_TYPE` | `FAK` | One of `GTC`, `GTD`, `FOK`, `FAK`. |
| `BOT_GTD_TTL_SECONDS` | `0` | Required for GTD; accepted range is 120 seconds through one year. |
| `BOT_TICK_SIZE` | `0.01` (replay only) | Rejected in paper/live; the venue tick is resolved at startup. |
| `BOT_TOKEN_ID` | replay only | Rejected in paper/live; the outcome token is resolved from Gamma/CLOB. |
| `BOT_NEG_RISK` | replay only | Rejected in paper/live; the flag comes from Gamma plus the book echo. |

\* either `BOT_MARKET_SLUG` or `BOT_CONDITION_ID` selects the market in live mode.

## Risk and strategy

| Variable | Default | Meaning |
|---|---:|---|
| `BOT_BANKROLL_USD` | `1000` | Kelly denominator/reference capital. |
| `BOT_KELLY_FRACTION` | `0.10` | Fractional Kelly multiplier `(0,1]`. |
| `BOT_MIN_EDGE` | `0.02` | Minimum model edge. |
| `BOT_MIN_CONFIDENCE` | `0.85` | Minimum alpha confidence. |
| `BOT_MAX_Q_VALUE` | `0.05` | Maximum alpha q-value. |
| `BOT_TAKER_FEE_RATE` | `0.07` (replay only) | Rejected in paper/live; the taker fee curve `rate × (p(1-p))^exponent` is resolved from the market document. |
| `BOT_MAX_ORDER_USD` | `100` | Per-order worst-cost cap. |
| `BOT_MAX_EXPOSURE_USD` | `250` | Aggregate reserved exposure cap. |
| `BOT_MAX_DAILY_LOSS_USD` | `50` | Circuit-breaker threshold. |
| `BOT_MIN_SIZE_SHARES` | `5` (replay only) | Rejected in paper/live; the venue minimum is resolved from the market document. |
| `BOT_INITIAL_POSITION_SHARES` | `0` | Operator-reconciled starting inventory; human shares. Never infer this casually. |
| `BOT_PRESIGN_TTL_MS` | `3000` | Maximum age of generated ladder entries; for GTD it must be shorter than the configured GTD lifetime. |
| `BOT_SIGNAL_TTL_MS` | `2000` | Alpha freshness window. |
| `BOT_MAX_BOOK_AGE_MS` | `3000` | Market-book freshness window. |

A conservative default is not a verified venue value. In live mode the startup resolver fetches the market document (`GAMMA_HOST` + `CLOB_HOST`), cross-validates tick size (three sources), minimum order size (three sources), the taker fee curve, the negative-risk flag, the outcome token, the market status flags, and the venue clock, and only then initializes the signer. Any missing, ambiguous, or contradictory value prints `READINESS = BLOCKED reason=...` and exits non-zero. Run `crowdintel-metadata` (read-only, loads no keys) to inspect or script the same resolution. Balances, allowances, open orders, and inventory still come from the preflight tooling of later phases.

## Transport and runtime

| Variable | Default | Notes |
|---|---:|---|
| `CLOB_HOST` | `https://clob.polymarket.com` | HTTPS origin; non-HTTPS is rejected. |
| `GAMMA_HOST` | `https://gamma-api.polymarket.com` | HTTPS origin for market metadata; non-HTTPS is rejected. |
| `BOT_MAX_CLOCK_OFFSET_MS` | `2000` | Maximum accepted venue/local clock offset (100–60000 ms). |
| `WS_HOST` | official market WSS | Market-channel endpoint; non-WSS is rejected. |
| `BOT_TLS_PIN` | empty | Optional libcurl public-key pin (`sha256//...`). Requires a rotation process. |
| `BOT_USER_WS_HOST` | derived | User-data WSS endpoint. Empty means "same authority as `WS_HOST` with path `/ws/user`"; a `WS_HOST` whose path is neither `/ws/market` nor `/ws/user` is refused rather than guessed. |
| `BOT_ALPHA_BIND` | `127.0.0.1` | Bind alpha ingress to loopback unless a trusted proxy/network design says otherwise. |
| `BOT_ALPHA_PORT` | `8088` | Alpha HTTP port. |
| `BOT_PIN_CPU` | `-1` | Hot-loop CPU; best effort. |
| `BOT_COLD_CPU` | `-1` | Network/replenisher CPU; best effort. |
| `BOT_LEDGER_PATH` | required live | Absolute path of the append-only order journal. Live mode refuses to arm without it; replay/paper reject it (the journal is venue evidence). |
| `BOT_LEDGER_FSYNC` | `1` (live only) | `fsync` every journal record. Disabling it trades durability for latency and is not recommended for live. |
| `BOT_LEDGER_MAX_BYTES` | `67108864` (live only) | Size cap (1 MiB–4 GiB). At the cap the journal compacts: terminal orders are pruned, unproven ones are re-materialized. |
| `BOT_KILL_SWITCH_FILE` | `/tmp/crowdintel.kill` | Existing file disables enqueue/egress and triggers shutdown; unsent entries are discarded. Production uses `/run/crowdintel/kill`. |
| `BOT_TICKS` | `0` | Stop after N hot-loop ticks; `0` means unlimited. Useful for replay tests. |

## User channel (Phase 3)

Live mode opens the authenticated user channel (`/ws/user`) described by the venue's AsyncAPI document. The subscribe payload carries the L2 credentials (`CLOB_API_KEY`, `CLOB_SECRET`, `CLOB_PASSPHRASE`) in `auth{apiKey,secret,passphrase}` and filters by the resolved condition id; the payload is wiped from memory right after it is sent. Credentials containing characters that would need JSON escaping abort the session instead of producing a mangled subscribe.

The client sends the text frame `PING` every 10 seconds (the server answers `PONG`) and drops a session that goes 30 seconds without any data. Order and trade events are parsed into bounded structs; every field the AsyncAPI document marks required is required here, duplicate keys are rejected, and unknown enum values (status, side, type) are preserved as `unknown` so they become a divergence instead of a guess.

Event handling rules (fail closed):

* an order event is applied to the journal only when its venue order id is already journaled; otherwise it is counted as **unattributed**;
* `OrderEvent.size_matched` is cumulative and is only ever raised; a regression is a divergence;
* trade increments are deduplicated by `(order id, trade id)` because `MATCHED`, `MINED` and `CONFIRMED` are status updates of one trade; if the dedupe table fills up, increments stop and trading pauses rather than risking a double count;
* `trade.status = FAILED` returns the order to `UNKNOWN`; `RETRYING` is informative only;
* maker fills are applied only to maker orders owned by our API key;
* any unattributed, divergent, or contradictory event pauses trading (`TRADING PAUSED reason=...`) until the Phase 5 reconciliation re-proves the account state.

## Order heartbeat (Phase 4)

Open orders are only kept alive by the authenticated order heartbeat
(`POST /v1/heartbeats`, documented in *Order Heartbeats*): the first body is
exactly `{"heartbeat_id":""}` and every `200` returns the id that the next
request must echo. The venue cancels **every open order of these credentials**
when a valid heartbeat has not been seen for 10 seconds. No new variables are
introduced: the request reuses the same CLOB credentials and the same L2
signature scheme (`timestamp + "POST" + "/v1/heartbeats" + body`) as order
submission, and the signed header block is wiped after each request.

* the service sends every 5 s (the documented cadence) and retries 1 s after a
  transport failure instead of waiting for the next slot;
* `400` with `{"error_msg":"Invalid Heartbeat ID","heartbeat_id":"…"}` adopts
  the expected id and retries; any other non-`200` counts as a missed
  heartbeat;
* after 7 s without a valid acknowledgement the state is `DEGRADED`, and after
  **10 s** it is `CRITICAL`;
* the first `CRITICAL` transition pauses trading
  (`TRADING PAUSED reason=heartbeat_lost`) and marks every non-terminal order
  `UNKNOWN` in the journal, because the venue may already have cancelled them.
  A cancelled-everything sweep is never assumed: the orders stay unproven until
  the Phase 5 reconciliation proves their real state, and a healthy heartbeat
  alone does **not** resume trading;
* the heartbeat only runs in live mode with an active journal; a credential
  whose secret cannot be decoded blocks startup
  (`READINESS = BLOCKED reason=heartbeat_configuration`).

Session output reports `heartbeat sends=… acks=… failures=… resyncs=… timeouts=… state=…`.

## Order journal (Phase 2)

Every order intent is written to `BOT_LEDGER_PATH` and `fsync`ed **before** any network egress, and every state change is written with the evidence that justifies it. The state machine has a mandatory `UNKNOWN` state: an order whose outcome is not proven by venue evidence (acknowledgement, private-channel event, or authenticated REST query) stays `UNKNOWN`, and the engine refuses to trade while any order is not terminal. A timeout is never treated as a rejection.

Recovery rules: a torn tail (interrupted write at the end) is truncated and reported (`torn_tail_bytes`); corruption *between* good records refuses to open; a file that is not a journal is rejected as `ledger corrupt`, never silently started empty. The on-disk format carries the magic `CILG0004`: a journal written by an older format (`CILG0003`, `CILG0002`) or by a different payload size is refused as corrupt instead of being reinterpreted. Startup prints `ledger path=… records=… orders=… unreconciled=… unknown=…`, then the Phase 5 reconciliation result, and finally `READINESS = READY …` or `READINESS = BLOCKED reason=…` (in live mode a blocked gate stops the process before the signer exists).

Compaction keeps the file under `BOT_LEDGER_MAX_BYTES` by pruning terminal orders; the journal is not an accounting record.

Start from [`infra/config/production.env.example`](../infra/config/production.env.example). The file keeps the `KEY=value` form systemd's `EnvironmentFile=` already understands — there is no TOML and no second parser: systemd loads the file, the process reads only the variables in the table above, and every other `BOT_`/`CLOB_`/`GAMMA_`/`WS_` name aborts startup. Validate a deployed file with `crowdintel-metadata` (reads no secret) before restarting the service. Venue parameters (`BOT_TICK_SIZE`, `BOT_MIN_SIZE_SHARES`, `BOT_TAKER_FEE_RATE`, `BOT_NEG_RISK`, `BOT_TOKEN_ID`) are read in replay mode only: in paper/live, setting any of them aborts startup.

## Reconciliation and readiness gate (Phase 5)

The journal records what this process believes; it is never enough to trade. Every time the private-channel evidence is lost — process start, user-channel drop/reconnect, heartbeat critical, or a divergence the channel cannot explain — the account is re-proven against the venue with authenticated REST calls before `trading_enabled` is set again:

1. `GET /auth/ban-status/closed-only` — a closed-only account is never tradable;
2. `GET /data/orders` (paginated by `next_cursor`) — every non-terminal journaled order is matched by venue id, `status`, `size_matched` and owner;
3. `GET /data/trades` — fills are attributed through `maker_orders[].order_id`; an unattributable trade blocks;
4. `GET /data/order/{id}` — a second pass for orders the two lists did not mention; `404 Order not found` is the only evidence that an order does not exist at the venue.

The L2 signature covers `timestamp + "GET" + path`; the query string is **not** signed (the official client signs a fixed `requestPath` and passes `next_cursor` as a parameter, and the documented example for `GET /data/orders` has no query). Writes to the journal are evidence-based and idempotent: `CANCELED` records the reported `size_matched` *before* the cancellation, `MATCHED`/`LIVE` record the cumulative `size_matched` (fills only grow), and an undocumented `status` is never downgraded. Reconciliation never guesses: it does not conclude "rejected" from a timeout, and an order with no venue id cannot be looked up (the CLOB has no client-order-id query) so it stays `UNKNOWN` and blocking.

Readiness is a single ordered gate (`core/src/reconciliation.hpp`): `market_metadata` → `ledger` → `reconciliation` → `unreconciled_orders`/`open_orders` → `closed_only_account` → `user_channel_configuration` → `heartbeat_configuration`. `open_orders` means the venue proved a resting order: the journal gate stays closed (this bot trades FAK), and `READINESS = BLOCKED reason=open_orders` is printed instead of arming.

Operational consequences:

- startup (live): a non-READY gate exits with status 1 before the signer is created, so no order can be signed while the account is unproven;
- recovery: a channel drop, a reconnect, a heartbeat critical or an unexplained divergence pauses trading (`TRADING PAUSED reason=…`) and marks non-terminal orders unproven; the reconciliation then runs immediately after a reconnect and is retried every 30 s while paused (a transient REST failure must not strand the process), and only `READINESS = READY` prints `TRADING RESUMED reason=reconciliation_ready`;
- the REST proof runs on the hot thread while trading is already disabled, bounded by 12 pages × the transport timeout; the market book keeps updating on its own thread;
- replay/offline builds have no credentials to query, so a closed gate stops them exactly as before.

## Account preflight (Phase 6)

`crowdintel-preflight` answers one question before arming: **can this account trade the resolved market right now?** It is read-only — it never signs an order and never reads `BOT_PRIVATE_KEY_HEX` — and it exits `0` on READY, `1` on BLOCKED (reason printed), `2` on a usage error.

What it proves, in order (all over the same L2 headers as order submission):

1. **Venue clock** — `GET /time`, compared against `BOT_MAX_CLOCK_OFFSET_MS` (the same tolerance the metadata resolver enforces, so there is one clock policy in the build);
2. **Credentials** — an authenticated `GET /auth/ban-status/closed-only` must return 200; a 401/403 is reported as `credentials_rejected` (proving *ownership* of the API key is Phase 5's job: every order carries `owner`);
3. **Closed-only** — a `closed_only: true` account cannot open orders, so it blocks;
4. **Collateral** — `GET /balance-allowance?asset_type=COLLATERAL` must show a balance of at least `BOT_MAX_ORDER_USD` (the engine's own cap for one order) and an allowance for the **exchange the order will name** — the address the resolver selected for the negative-risk flag — of at least the same amount (or the venue's `max` sentinel);
5. **Outcome token** — the same query with `asset_type=CONDITIONAL&token_id=…` must show the operator approval for that exchange, because a fill credits inventory and the engine may then submit SELL orders. `BOT_INITIAL_POSITION_SHARES` is checked against the venue balance when non-zero.

The response shape is the one the OpenAPI spec requires (`balance` + `allowances` spender→amount map). A missing map, a duplicated spender (case variants count), an unparsable amount, a value with more than 6 decimals or more spenders than the bounded table all fail the check instead of being ignored. The venue's `max` sentinel is accepted and counted (`unlimited_sentinels`), but it is only documented for the sibling Data API approvals endpoint, so it is printed explicitly rather than assumed.

`--refresh-allowances` performs `GET /balance-allowance/update` first (the operator action to sync the venue cache after approving contracts). It is opt-in: the trading bot never calls it.

`infra/scripts/run_preflight.sh` runs the tool from a filtered environment: the
non-secret configuration file plus the three L2 credential files, and nothing
else. It aborts if the file defines `BOT_PRIVATE_KEY_HEX(_FILE)` (the tool must
never see it) and it refuses CRLF or malformed lines. `--check-config` validates
that environment without network access.

The live bot runs the same preflight itself — at startup, after Phase 5 reconciliation and before the signer exists, and again before it re-arms after any evidence gap. A non-READY preflight prints `READINESS = BLOCKED reason=account_state` and the process stops.

```
BOT_MODE=live BOT_ENABLE_LIVE_TRADING=1 BOT_MARKET_SLUG=<slug> BOT_OUTCOME=Yes \
  build/bin/crowdintel-preflight            # --json for machine-readable output
```

The tool reads the bot's environment (`BOT_MODE=live`, L2 credentials,
`CLOB_HOST`, arming flag…), but does not need `BOT_LEDGER_PATH` or the alpha
bearer token, and it refuses to start if the signing key is present: run it from
a shell (or a `systemd-run` unit) that carries the non-secret config and the L2
credentials only.
