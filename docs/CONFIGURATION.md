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
| `BOT_MODE` | `paper` | `replay` (the `l2_backtester` binary), `paper` (live market data, no egress, no simulated fills) or `live`. `mock` is a deprecated alias for `paper`. |
| `BOT_MARKET_SLUG` | required live | Alpha routing identity and the metadata lookup key. |
| `BOT_TOKEN_ID` | resolved from metadata | Decimal uint256 outcome token ID. Live **and paper** require it explicitly, or a slug/condition id to resolve it from venue metadata; there is no test-vector default in either. Only `BOT_MODE=replay` - which has no venue to ask - falls back to the documented test vector. `crowdintel-config fingerprint` prints `token_id_source` as `BOT_TOKEN_ID`, `venue-metadata` or `replay-test-vector`, and the bot's startup banner prints the same provenance next to the resolved id. |
| `BOT_CONDITION_ID` | empty | Optional; resolved from the slug when absent. |
| `BOT_NEG_RISK` | `0` | Selects the negative-risk exchange domain when `1`. Treated as an expectation: live mode reads the value from the venue and refuses to start on disagreement. |
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
| `BOT_KILL_SWITCH_FILE` | `/tmp/crowdintel.kill` | Polled every 100 ms in **every** mode (live, paper and the offline mock build). An existing file - of any type - disables enqueue/egress and triggers shutdown; unsent entries are discarded. A path that cannot be checked at all (`EACCES`, `ENOTDIR`, `ENAMETOOLONG`, ...) is treated as engaged and logs `KILL SWITCH check failed closed (<errno text>)`. Production uses `/run/crowdintel/kill`. |
| `BOT_TICKS` | `0` | Stop after N hot-loop ticks; `0` means unlimited. Useful for mock tests. |
| `CROWDINTEL_FORCE_MOCK` | unset | Test/CI only. Forces `BOT_MODE` to paper (no venue transport) whatever the environment says, and `MarketConfig::load` rejects it outright when combined with `BOT_MODE=live`; preflight reports `mock_absent` FAIL if it is set for a live run. It must never be present on a production host. |
| `BOT_SESSION_TIMEOUT_MS` | `0` | Bounded session length in ms; `0` runs until signalled. Validated to `0..86400000` at load and mixed into `config_fingerprint`. Used by CI smoke runs and rehearsals; production leaves it at `0`. |

Start from [`infra/config/production.env.example`](../infra/config/production.env.example), the authoritative template: `crowdintel-config validate <file>` accepts it and rejects any variable this binary does not read. With the default `BOT_STRICT_ENV=1` the process itself also refuses to start on an unknown `BOT_*`/`CLOB_*`/`POLY_*`/`WS_*`/`POLYGON_*`/`GAMMA_*`/`CROWDINTEL_*` variable, so a setting the binary would ignore can never look active. Configuration changes still require peer review and a startup-log check of the effective non-secret identity/risk values and of `config_fingerprint`.


## Live-operations variables (Phases 1-7)

`infra/config/production.env.example` is the authoritative, validated template
(`crowdintel-config validate` passes on it). Summary:

| Group | Variables |
|---|---|
| Metadata | `GAMMA_HOST`, `BOT_CONDITION_ID`, `BOT_METADATA_MAX_AGE_MS`, `BOT_METADATA_REFRESH_MS`, `BOT_MAX_TAKER_FEE_RATE`, `BOT_ALLOW_PROTOCOL_V2`, `POLYGON_RPC_URL`, `POLYGON_RPC_BACKUP_URL` |
| Ledger | `BOT_LEDGER_DIR`, `BOT_LEDGER_FSYNC`, `BOT_LEDGER_CHECKPOINT_EVERY` |
| User channel | `BOT_USER_WS_ENABLED`, `BOT_USER_WS_HOST`, `BOT_USER_WS_KEEPALIVE_MS`, `BOT_USER_WS_IDLE_MS`, `BOT_USER_WS_PONG_MS`, `BOT_USER_WS_RECONNECT_MIN_MS`, `BOT_USER_WS_RECONNECT_MAX_MS` |
| Heartbeat | `BOT_HEARTBEAT_ENABLED`, `BOT_HEARTBEAT_INTERVAL_MS`, `BOT_HEARTBEAT_WARN_MS`, `BOT_HEARTBEAT_BLOCK_MS`, `BOT_HEARTBEAT_ASSUME_CANCELLED_MS`, `BOT_HEARTBEAT_MAX_FAILURES` |
| Reconciliation | `BOT_RECON_MAX_PAGES` (startup reconciliation itself is
  unconditional in live mode: there is no switch to disable it) |
| Preflight | `BOT_PREFLIGHT_TOKEN_FILE`, `BOT_PREFLIGHT_MAX_AGE_S`, `BOT_MAX_CLOCK_SKEW_S`, `BOT_MIN_COLLATERAL`, `BOT_TARGET_ALLOWANCE`, `BOT_PREFLIGHT_CHECK_L1`, `BOT_PREFLIGHT_CHECK_USER_WS`, `BOT_PREFLIGHT_CHECK_HEARTBEAT` |
| Hygiene | `BOT_STRICT_ENV` |

Rules enforced at load time (they are errors, not warnings):

* live mode requires `POLYGON_RPC_URL` (https), an enabled user channel,
  startup reconciliation, `BOT_INITIAL_POSITION_SHARES=0` (inventory comes from
  reconciliation) and a resolved token id;
* heartbeat thresholds must satisfy `interval < warn < block ≤ 10000 ms` and
  `block ≤ assume-cancelled ≤ 10000 ms`, because the venue cancels resting
  orders from 10 s after the last *acknowledged* beat onwards
  (`heartbeat::K_VENUE_TIMEOUT_MS`). The documented 5 s evaluation cadence means
  the kill itself can land as late as ~15 s
  (`heartbeat::K_VENUE_WORST_CASE_MS = 10000 + 5000`), and that worst case is a
  reason to start treating orders as possibly cancelled *at* 10 s and to
  reconcile - never a licence to wait for it: `assume-cancelled` only disables
  trading and triggers the authoritative reconciliation, it never releases
  exposure. Both bounds are enforced twice, at `MarketConfig::load_live_ops`
  (`core/src/market_config.hpp`) and at `heartbeat::Config::validate`
  (`core/src/order_heartbeat.hpp`), which additionally requires
  `interval + request timeout` to stay `K_MIN_MARGIN_MS` below the venue timeout;
* user-channel timings must satisfy `keepalive < idle < pong`;
* `BOT_METADATA_MAX_AGE_MS >= 2 * BOT_METADATA_REFRESH_MS`, because the age
  budget is enforced at runtime (below) and a refresh period longer than half
  the budget would block egress between two scheduled refreshes by construction;
* `BOT_STRICT_ENV=1` (default) rejects any `BOT_*`, `CLOB_*`, `POLY_*`, `WS_*`,
  `POLYGON_*`, `GAMMA_*` or `CROWDINTEL_*` variable this binary does not read,
  so a setting the process would ignore cannot look active;
* the effective non-secret configuration is hashed into
  `config_fingerprint`, printed at startup and bound into the preflight pass
  token: `BOT_ENABLE_LIVE_TRADING=1` is honoured only for a token whose
  fingerprint matches. The fingerprint covers configuration, not credentials -
  the wallet is derived from a secret and is excluded by construction - so both
  binaries also print `wallet=`/`maker=`/`api_address=` and the canary checklist
  (H2/H3) requires the operator to confirm it is the same wallet: preflight
  verified balances, allowances and positions for that address only.

### Optional preflight probes

Three `crowdintel-preflight` checks are opt-in because each one touches the venue
with the production credentials. When disabled they report `SKIP` with the reason,
never `PASS`, so a READY verdict always shows what was left unproven.

| Variable | Default | What it proves | When to enable |
|---|---:|---|---|
| `BOT_PREFLIGHT_CHECK_L1` | `0` | That the private key really owns the configured API credentials: it derives them with the L1 `ClobAuth` signature and compares | Before the canary (H2). Without it, a key/credential mismatch is only discovered when the venue rejects a signed request |
| `BOT_PREFLIGHT_CHECK_USER_WS` | `0` | That the user channel handshakes and accepts the subscription - the channel every fill arrives on | Before the canary (H2) |
| `BOT_PREFLIGHT_CHECK_HEARTBEAT` | `0` | Which heartbeat path and chain-start body the venue accepts, and that a beat is acknowledged | Before the canary, **with the bot stopped**: a successful probe leaves the venue's cancel-on-disconnect contract active for these credentials, so any order placed afterwards is cancelled ~10-15 s later unless the bot keeps beating. The check SKIPs by itself when reconciliation reports open orders |

The heartbeat line reports the path and body form that actually answered
(`ClobApiClient::heartbeat_path()`, `heartbeat_uses_null_start()`), not the one
tried first. That matters because the venue contract is still unresolved: the
official SDKs send `POST /v1/heartbeats` with `{"heartbeat_id":null}`, the OpenAPI
reference documents `POST /heartbeats`, and the narrative docs ask for
`{"heartbeat_id":""}`. This line is the artifact that settles it (checklist H2,
open item J).

### Metadata age guard (runtime)

Tick size, minimum order size, fee rate and the negative-risk flag come from the
venue and can change under the bot, so the published metadata generation carries
the wall-clock instant at which it was observed. The supervisor thread checks it
every 250 ms against `BOT_METADATA_MAX_AGE_MS` (default 90000 ms, refreshed every
`BOT_METADATA_REFRESH_MS`, default 30000 ms):

* while the snapshot is within budget, nothing changes;
* when it exceeds the budget - or when no generation is published, or the wall
  clock moved backwards - the supervisor logs
  `TRADING DISABLED: venue metadata is stale (age ... ms, budget ... ms)`, sets
  `trading_enabled=false` and invalidates the runtime view, so the hot path also
  stops seeing tradable metadata;
* egress is re-authorised only by a successful refresh, and only when no other
  blocking condition is present (readiness, orders in `SUBMITTING`/`UNKNOWN`,
  heartbeat health, stale user channel).

Defaults: `BOT_METADATA_MAX_AGE_MS=90000`, `BOT_METADATA_REFRESH_MS=30000`.
