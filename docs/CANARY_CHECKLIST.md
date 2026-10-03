# Canary checklist (live-readiness gate)

Nothing in this repository is "100 % functional" because it compiles. A canary
run is authorised only when every **automated** item below is PASS *and* every
**human** item has been executed against a real wallet, real credentials and a
real market by an operator who recorded the result.

Legend: `[A]` automated (machine-checkable, runs in CI or in
`crowdintel-preflight`), `[H]` human (requires a funded account, real
credentials or a live market; **never** executed by an automated pipeline).

---

## 1. Automated gates

| # | Item | Where it is enforced | Status |
|---|---|---|---|
| A1 | Build in all four variants (release+network, release offline, ASan+UBSan, TSan) | `.github/workflows/ci.yml` | PASS locally 2026-10-03 |
| A2 | `crypto_kat`, `core_units`, `backtester_smoke` | `ctest` | PASS |
| A3 | `live_safety_units` (ledger, state machine, metadata, REST, user channel, heartbeat, reconciliation, recorder, tickets, concurrency) | `ctest` | PASS (0 failures) |
| A4 | `preflight_rejects_empty_config` (preflight must not declare readiness without configuration) | `ctest` (`WILL_FAIL TRUE`) | PASS |
| A5 | Configuration template validates and contains no secrets | `crowdintel-config validate infra/config/production.env.example` | PASS (65 keys, 0 errors) |
| A6 | Unknown prefixed environment variables are rejected | `MarketConfig::unknown_prefixed_env()` (`BOT_STRICT_ENV=1`) | PASS |
| A7 | Metadata pipeline fails closed on any cross-source disagreement | `venue::MetadataPipeline` + unit tests | PASS |
| A8 | Ambiguous POST /order → `UNKNOWN`, never retried, trading disabled | `OrderGateway` + `recorder::OrderRecorder` + unit tests | PASS |
| A9 | Duplicate / out-of-order user-channel events are idempotent | `ledger::compute_event_key` + `UserEventApplier` + unit tests | PASS |
| A10 | Corrupt ledger (mid-file damage) refuses to open | `ledger::EventLedger::open` + unit test | PASS |
| A11 | Trailing torn journal record (crash during append) is discarded and reported | `replay_journal` + unit test | PASS |
| A12 | Heartbeat thresholds respect the venue contract (send 5 s, block ≤ 10 s, assume-cancelled ≤ 10 s; the ~15 s worst case is the venue's kill window, not our threshold) | `heartbeat::Config::validate` (`core/src/order_heartbeat.hpp:96-111`) + `tests/unit/test_live_safety.cpp:1367-1390` (12000 rejected, 10000 accepted) | PASS |
| A13 | Chain id verified as 137 by RPC before constants are trusted | `main_hot_path.cpp`, `crowdintel-preflight` | PASS (code path); live RPC call is `[H]` |
| A14 | Mocks excluded from the live binary | `MockCLOBClient` compiled only when `CROWDINTEL_HAVE_NETWORK` is absent | PASS |
| A15 | `BOT_ENABLE_LIVE_TRADING=1` requires a fresh preflight token with a matching configuration fingerprint | `preflight::Runner::token_is_fresh` | PASS |
| A16 | Ledger concurrency under TSan (3 writers + reader + 2 replayers) | `test_ledger_concurrency` | PASS |
| A17 | Integration against the in-process loopback venue: duplicated and out-of-order user events, disconnect mid-message, heartbeat rejected (400) and delayed, HTTP 200 with an invalid body, `POST /order` timeout → `UNKNOWN` with no retry, restart with an order still open | `local_venue_integration` (`tests/integration/test_local_venue.cpp`) | PASS (real sockets, real threads) |
| A18 | Heartbeat cadence leaves margin below the venue timeout (`interval + request_timeout ≤ 8000 ms`) | `heartbeat::Config::validate` + unit tests | PASS |

## 2. Preflight checks (`crowdintel-preflight`)

Any FAIL → exit 1, `READY=false`, and the bot refuses live egress.

| Check | Meaning |
|---|---|
| `mode`, `live_armed`, `mock_absent` | mode is live, armed, and not forced into the mock transport |
| `config_fingerprint` | SHA-256 of the effective non-secret configuration |
| `chain_id` | `eth_chainId` == 137 (primary or backup RPC) |
| `clob_health`, `clock_offset` | `GET /` answers; `GET /time` within `BOT_MAX_CLOCK_SKEW_S` |
| `credentials_l2` | `GET /data/orders` accepted with the L2 headers |
| `credentials_l1` | *(optional)* the private key derives the configured API key |
| `signer_identity`, `maker_funder`, `api_owner` | signer/maker/funder consistent with `BOT_SIGNATURE_TYPE` |
| `market_identity`, `market_status` | condition id and token resolved; active, not closed/archived, accepting orders, book enabled, not restricted |
| `tick_size`, `min_order_size`, `neg_risk`, `fee_schedule` | read from the venue, never defaulted |
| `tick_expectation`, `min_size_expectation`, `neg_risk_expectation` | configuration agrees with the venue |
| `protocol_v2_guard` | token is not a protocol-v2 position id (Combos/v3 exchange) unless explicitly allowed |
| `book_snapshot` | `GET /book` returned a coherent snapshot for the token |
| `exchange_contract`, `collateral_contract`, `ctf_contract` | `eth_getCode` non-empty for the exchange, pUSD and CTF |
| `collateral_balance`, `collateral_allowance`, `balance_cross_check` | pUSD balance ≥ `BOT_MIN_COLLATERAL`; allowance ≥ target; chain and CLOB agree |
| `outcome_balance`, `inventory_cross_check`, `outcome_approval` | outcome-token balance agrees between chain and CLOB; ERC-1155 approval present when there is inventory to sell |
| `gas_balance` | POL available (informational; relayer flows need none) |
| `user_wss` | *(optional)* the user channel accepts the subscription |
| `heartbeat` | *(optional, skipped when open orders exist)* `POST /v1/heartbeats` accepted |
| `ledger` | journal writable, fsync works, no corruption |
| `kill_switch` | the kill-switch file is absent **and can be checked**: an unanswerable check (`EACCES`, `ENOTDIR`, ...) is a FAIL, not a PASS |

## 3. Human gates — document the procedure, never automate it

> These steps touch a real wallet, real credentials and real money. They are
> executed by an operator, with the minimum viable capital (a few pUSD), and the
> output is pasted into the canary record. **No CI job, script or agent in this
> repository may perform them.**

| # | Step | Expected evidence |
|---|---|---|
| H1 | Fund the trading wallet with the canary amount of pUSD and set the approval to exactly `BOT_TARGET_ALLOWANCE` for the exchange spender (never `max_uint256`) | tx hash + `allowance()` read-back |
| H2 | Run `crowdintel-preflight --json` on the production host and store the output. Record the `wallet=`/`maker=`/`api_address=` lines it prints: they are the wallet whose balances, allowances and positions were actually verified. Run it with `BOT_PREFLIGHT_CHECK_L1=1` and `BOT_PREFLIGHT_CHECK_USER_WS=1` so key-to-credential ownership and the fill channel are proven, and with `BOT_PREFLIGHT_CHECK_HEARTBEAT=1` **while the bot is stopped and no order is resting** - a successful probe activates the venue's cancel-on-disconnect contract. Record the heartbeat line verbatim: it names the path and chain-start body the venue actually accepted | `READY=true`, `failures=0`, fingerprint, `wallet=0x...`, `[PASS] credentials_l1`, `[PASS] user_wss`, `[PASS] heartbeat POST <path> accepted with heartbeat_id=<null or "">` |
| H3 | Start the bot with `BOT_MODE=live`, `BOT_ENABLE_LIVE_TRADING=1` and confirm it refuses to start when the preflight token is missing, stale or from another configuration. Then confirm the `wallet=` the bot prints in its startup banner is **identical** to the one H2 recorded: the token binds the configuration fingerprint, not the credential, so a swapped key is only caught by this comparison and by the startup reconciliation | log line `FATAL: … requires a fresh crowdintel-preflight pass`, plus matching `wallet=0x…` in both outputs |
| H4 | Confirm the startup reconciliation prints `READY` and that the ledger contains a `reconciliation_run` event | log + `state.journal` |
| H5 | Confirm the user channel subscribes and that a **real** FAK minimum-size order produces: `POST /order` response, user-channel `order` + `trade` frames, a `fills` record, and an inventory update that matches `GET /balance-allowance` | order id, trade id, ledger events |
| H6 | Kill the process between egress and response (`kill -9`), restart, and confirm the order is resolved (linked or `UNKNOWN` → BLOCKED), never duplicated | restart log + reconciliation report |
| H7 | Cut the network during `POST /order` (firewall drop) and confirm the order goes to `UNKNOWN`, trading is disabled, and no retry is sent | log `TRADING DISABLED: ambiguous_order_outcome` |
| H8 | Cut the network during a heartbeat and confirm: warn → block → assume-cancelled, and that the venue cancels the resting orders | heartbeat counters + venue open-order list |
| H9 | Restart with an order still open on the venue and confirm adoption + BLOCKED (`venue_order_missing_locally`) | reconciliation reasons |
| H10 | Change the market tick size (or pick a market whose tick differs from the configuration) and confirm the book is invalidated, the ladder rebuilds, and no order is sent with a stale grid | log `tick size changed in flight` |
| H11 | Feed an HTTP 200 with an invalid body (proxy) and confirm the response is rejected, not guessed at | log `malformed CLOB response` |
| H12 | Delay/reorder user-channel frames (proxy) and confirm idempotency and monotonic matched sizes | ledger duplicates counter |
| H13 | Verify whether the venue `orderID` equals the local EIP-712 digest (`[NO VERIFICADO]`) | log `NOTE: venue orderID equals the local EIP-712 digest` or its absence |
| H14 | Run 24 h in shadow (paper mode with live market data, egress disabled) and confirm zero unexplained divergences | session summary counters |
| H15 | Only then: canary with the minimum FAK size for 24 h under supervision | canary record |

## 4. Operational rules (mandatory, decided 2026-10-03)

These are deployment rules, not code paths. Violating any of them invalidates the
canary regardless of what the software reports.

1. **Exclusive credentials.** The CLOB API credentials used by the bot must not be
   in use by any other session, bot, notebook or manual tool while the bot runs.
   Two reasons: the heartbeat contract cancels *every* order owned by those
   credentials when the beats stop (so another session's orders would be killed by
   our shutdown, and our orders by theirs), and reconciliation attributes every
   order it sees on those credentials to this process.
2. **One process per ledger.** `BOT_LEDGER_DIR` must be private to a single
   instance. Two writers on one journal make recovery impossible by design, and
   the ledger refuses to open when it detects damage — which would look like an
   unexplained outage rather than a misconfiguration.
3. **One kill switch, one preflight token, one config fingerprint.** The token is
   bound to the fingerprint of the effective configuration: changing any setting
   requires re-running `crowdintel-preflight`.
4. **Allowances are set by hand, to the minimum.** `BOT_TARGET_ALLOWANCE` (or the
   derived `BOT_MAX_EXPOSURE_USD` + 10 %) is the approved amount. No automated
   approval, and never `max_uint256`.
5. **Signature type 0 (EOA) for the canary.** Types 1/2 need the exact
   proxy/safe identity combination verified against the official SDK, and type 3
   remains fail-closed.

## 5. Deferred work (explicitly out of scope for the canary)

| Item | Decision | Why |
|---|---|---|
| Signature type 3 (POLY_1271 / ERC-7739 wrapping) | **Not now.** Post-canary. | The canary runs on an EOA (type 0). The wrapper is fully specified by py-sdk `_internal/actions/orders/typed_data.py` (app domain separator + contents hash + type string + 2-byte length trailer), so it is a bounded task — but it is new cryptographic code with no venue-side KAT, and it does not serve the current objective. |
| Migrate `ws_market_listener.hpp` onto `ws_session.hpp` | **Deferred.** Post-canary. | Technical debt, not a blocker. The market listener is exercised in production-like runs and cannot be re-validated from a sandbox without venue egress; touching it now would trade a known-good hot path for an untestable refactor. The migration path is written in `core/src/ws_session.hpp`. |
| `BOT_ALLOW_PROTOCOL_V2` | **Keep 0.** | Fail-closed: protocol-v2 position ids settle through the Combos exchange and the PositionManager, a path this signer/allowance set does not cover. |
| `BOT_RECON_MAX_PAGES` | **Keep the current value (4).** | Raise it only if the canary ever rests enough orders to exceed four pages, which minimum capital should not produce. |

## 6. Stop conditions (immediate)

Any of these ends the canary: `READY=BLOCKED` after a reconnect, an `UNKNOWN`
order that reconciliation cannot resolve, a ledger write failure, a heartbeat
`INVALIDATED`, an inventory or allowance divergence, a neg-risk change, a
metadata refresh failure, or `TRADING DISABLED: venue metadata is stale` (the
`BOT_METADATA_MAX_AGE_MS` age guard tripping means refreshes are not keeping up). In all cases the process disables egress first and
reports second.
