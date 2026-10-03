# Live-safety work — audit, implementation and evidence (2026-10-03)

This document is the deliverable for the Phase 0 audit plus the record of what
was implemented in Phases 1–8, with the separation between **verified facts**
(cited) and **hypotheses / unverified** items that still require a human with a
funded account and real credentials.

Companion documents: `docs/CANARY_CHECKLIST.md` (the gate), `docs/CONFIGURATION.md`
(every variable), `docs/DEPLOYMENT.md` (operational procedure).

---

## 1. Phase 0 audit — verdict per phase, before any change

| Phase | State before this work | Evidence |
|---|---|---|
| 1 — dynamic market metadata | **Missing.** tick size, minimum order size, taker fee, negative-risk flag and token id were environment/hardcoded values; there was no market metadata read at all and no chain-id verification | `core/src/market_config.hpp` (`tick_size`, `min_size_shares = 5000000`, `taker_fee_rate = 0.07`, `neg_risk` from `BOT_NEG_RISK`, `BOT_TOKEN_ID` defaulting to a test vector); no `/markets`, `/book`, `/tick-size`, `/neg-risk`, `/fee-rate` call anywhere in the tree |
| 2 — persistent ledger + order state machine | **Missing.** Order state existed only in memory inside `ExecutionEngine`; there was no `UNKNOWN` state, no durable record and no idempotency | `core/src/execution_engine.hpp` (`confirmed_inventory_`, `committed_exposure_usd_` in-process only); no ledger file, no `state_events` |
| 3 — user WebSocket | **Missing** and already declared a production blocker | `core/src/user_ws_client.hpp` did not exist; `CHANGELOG.md` "Known blockers"; `docs/REMEDIATION_STATUS.md` observation #14 "Unresolved production blocker" |
| 4 — order heartbeat | **Missing.** The only heartbeat was the market-channel `PING`/`PONG` keep-alive | `core/src/ws_market_listener.hpp` (`send_frame(ssl, fd, 0x1, "PING", 4)` every 8 s) |
| 5 — reconciliation + readiness | **Missing.** No startup or post-disconnect reconciliation, no READY/BLOCKED verdict | `docs/STATUS.md` production blockers 1–3 |
| 6 — preflight binary | **Missing.** Only a manual checklist in `docs/DEPLOYMENT.md` §3; no RPC balance/allowance queries | no `crowdintel-preflight` target in `core/CMakeLists.txt` |
| 7 — configuration and modes | **Discrepancy.** `config.prod.toml` does not exist anywhere in the repository (0 matches for `toml`); configuration is 100 % environment, i.e. "option B" was already the de-facto state. Modes were only `mock`/`live`, `MockCLOBClient` was instantiated even in live, and `BOT_ENABLE_LIVE_TRADING=1` was not tied to any preflight | `core/src/market_config.hpp:205-209`; `core/src/main_hot_path.cpp:164` (`MockCLOBClient mock_client(cfg);` unconditional) |
| 8 — test suite | **Partial.** Unit tests, signer KATs and an L2 replay backtester existed; nothing covered the user channel, heartbeat, ledger, reconciliation, `UNKNOWN`, fault injection or a canary checklist | `tests/unit/test_core.cpp`, `tests/crypto/cross_check_v2.py`, `tests/replay/l2_backtester.cpp` |

What already worked and was deliberately left intact: the market WebSocket
client (TLS, RFC 6455 framing, `book`/`price_change`/`tick_size_change`,
invalidation on reconnect), the EIP-712 V2 signer (with KATs and an independent
Python cross-check), the fixed-point order amount math, the L2-authenticated
`POST /order` client with its response classifier, secret handling through
`*_FILE`, the risk gates, and the CI matrix (GCC/Clang × network on/off, ASan,
UBSan, TSan, non-root production container).

## 2. What was implemented

| Phase | Files |
|---|---|
| Shared primitives | `core/include/json_scan.hpp`, `core/include/asset_type.hpp`, `core/crypto/sha1.hpp` |
| 1 | `core/include/venue_metadata.hpp`, `core/src/metadata_pipeline.hpp`, `core/src/rpc_client.hpp`, `core/src/curl_transport.hpp`, `core/src/clob_rest_client.hpp` |
| 2 | `core/include/order_state.hpp`, `core/include/event_ledger.hpp`, `core/src/order_recorder.hpp`, `core/crypto/order_digest.hpp` |
| 3 | `core/src/user_event.hpp`, `core/src/user_ws_protocol.hpp`, `core/src/user_ws_client.hpp`, `core/src/ws_session.hpp`, `core/src/ws_url.hpp` |
| 4 | `core/src/order_heartbeat.hpp` |
| 5 | `core/src/reconciliation.hpp` |
| 6 | `core/src/preflight.hpp`, `core/src/preflight_main.cpp`, `core/crypto/clob_auth.hpp` |
| 7 | `core/src/config_tool_main.cpp`, `core/src/market_config.hpp` (modes + live-ops block + strict env + fingerprint), `core/src/main_hot_path.cpp` (mode wiring, mock exclusion) |
| 8 | `tests/unit/test_live_safety.cpp`, `tests/support/fixture_transport.hpp`, `docs/CANARY_CHECKLIST.md`, CI steps |
| Integration | `core/src/main_hot_path.cpp` (metadata-first startup, gates, supervisor), `core/src/order_gateway.hpp` (`SubmitObserver`), `core/src/execution_engine.hpp` (reconciled inventory), `core/src/lightweight_client.hpp` (ambiguity + raw response) |

Design decisions and rejected alternatives:

* **No SQLite.** The ledger is an append-only journal plus an atomically
  replaced checkpoint in standard C++. Rationale: no new third-party dependency
  (the build needs only libsecp256k1, plus curl/OpenSSL for the network
  variant), the access pattern is a strictly ordered event log with a bounded
  working set, and durability semantics stay auditable in one file. A database
  remains the right answer if the working set must grow beyond the bounded maps
  (1024 orders / 2048 fills / 8192 dedup keys); that decision is flagged rather
  than taken silently.
* **No TOML parser.** The repository never had a TOML file. Adding one would
  create a second source of truth that the binary could ignore — precisely the
  failure mode the requirement targets. Option B was implemented instead:
  `/etc/crowdintel/config` as a systemd `EnvironmentFile`, credentials through
  `/etc/crowdintel/credentials/*` with the existing `*_FILE` indirection, and
  `crowdintel-config validate|render-env|fingerprint|keys` as the guard. Strict
  mode additionally rejects any prefixed variable the binary does not read, so
  an ignored setting is a hard error.
* **Local order identity = the EIP-712 digest.** A submission ticket is written
  durably *before* egress, keyed `L:<digest>`, and retired (`SUPERSEDED`) when
  the venue id arrives. This makes "crash between send and response"
  recoverable without assuming anything about how the venue derives `orderID`.
* **One mutex in the ledger.** The topology has three writers (gateway, user
  channel, supervisor). A single mutex with `*_locked` variants makes that
  race-free without pushing locking discipline into every caller; the hot path
  never touches it (inventory is published to an atomic by the supervisor).
* **The market listener was not refactored onto the new `ws::Session`.** Its
  plumbing is duplicated, deliberately: it is exercised in production-like runs
  and cannot be re-validated from an environment without venue egress. The
  migration path is written down in `core/src/ws_session.hpp`.

## 3. HECHOS (verified, with citations)

Venue endpoints, wire formats and constants were verified against the official
documentation and the official SDKs (cloned and read locally):

* `Polymarket/py-sdk` @ `b543c9db0c896a3727619ea174db7971f03b5b6a` (2026-10-01)
* `Polymarket/clob-client` @ `7df8257dc95f99edb257b53a7873e273a9b4a9b3`
* `Polymarket/py-clob-client` @ `b076b04d61135657e25dccc1bbd6866a96bd8c6e`
* `https://docs.polymarket.com/...` pages listed inline in the headers

| Fact | Source |
|---|---|
| CLOB base `https://clob.polymarket.com`, Gamma `https://gamma-api.polymarket.com`, market WSS `wss://ws-subscriptions-clob.polymarket.com/ws/market`, user WSS `.../ws/user` | py-sdk `src/polymarket/environments.py:98-101`; docs `getting-started/api` |
| Chain id 137; pUSD collateral `0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB`; CTF `0x4D97DCd97eC945f40cF65F87097ACe5EA0476045`; CTF Exchange `0xE111180000d2663C0091e4f400237545B87B996B`; Neg Risk CTF Exchange `0xe2222d279d744050d28e00520010520000310F59`; Combos exchange (v3) `0xe3333700cA9d93003F00f0F71f8515005F6c00Aa`; PositionManager `0x006F54F7f9A22e0000CC2AB60031000000ae9fEF` | docs `resources/contracts`; py-sdk `environments.py:91-104` |
| `GET /book?token_id=` returns `market, asset_id, timestamp, bids, asks, min_order_size, tick_size, neg_risk, last_trade_price, hash` | clob-client `src/types.ts:316-327`; py-sdk `models/clob/order_book.py` |
| `GET /tick-size?token_id=` → `minimum_tick_size`; `GET /neg-risk?token_id=` → `neg_risk`; `GET /clob-markets/{cid}` → `nr`, `mts`, `t[].t`, `fd.{r,e}`; `GET /markets-by-token/{token}`; `GET /fee-rate?token_id=` → `base_fee` | py-sdk `_internal/actions/orders/market_data.py:52-125,207-252`; py-clob-client `client.py:402-458,1027` |
| Supported tick sizes and amount rounding `0.1→3, 0.01→4, 0.005→5, 0.0025→6, 0.001→5, 0.0001→6` | py-sdk `_internal/actions/orders/context.py:19-25` |
| Fee formula `fee = shares × rate × p × (1 − p)`; makers pay nothing; category rates 0.07/0.05/0.04/0; 5-decimal rounding | docs `trading/fees`; Gamma `feeSchedule.{rate,exponent,takerOnly,rebateRate}` in docs `market-data/market-details` |
| `POST /order` body: `deferExec`, `order{builder,expiration,maker,makerAmount,metadata,salt,side,signature,signatureType,signer,takerAmount,timestamp,tokenId}`, `orderType`, `owner`, optional `postOnly`; batch ≤ 15; response `{success,errorMsg,makingAmount,orderID,status,takingAmount,tradeIDs,transactionsHashes}` | py-sdk `_internal/actions/orders/post.py:19-80`, `models/clob/order_response.py` |
| L2 auth: `HMAC-SHA256(base64url_decode(secret), timestamp + METHOD + path [+ body])`, headers `POLY_ADDRESS/POLY_SIGNATURE/POLY_TIMESTAMP/POLY_API_KEY/POLY_PASSPHRASE`; **the path excludes the query string** | py-sdk `_internal/hmac.py:9-25`, `clients/async_secure.py:4090-4106`; py-clob-client `signing/hmac.py`, `headers/headers.py`; docs `getting-started/api` |
| L1 auth: EIP-712 domain `{ClobAuthDomain,1,137}` (**no** `verifyingContract`), `ClobAuth(address,string timestamp,uint256 nonce,string message)`, message `"This message attests that I control the given wallet"`, headers `POLY_ADDRESS/POLY_SIGNATURE/POLY_TIMESTAMP/POLY_NONCE`, `POST /auth/api-key` / `GET /auth/derive-api-key` | py-sdk `_internal/l1_auth.py:8-52`; docs `getting-started/api` |
| Order book/status vocabulary: order `status ∈ {LIVE,MATCHED,DELAYED,UNMATCHED,CANCELED}`, `type ∈ {PLACEMENT,UPDATE,CANCELLATION}`; trade statuses with `CONFIRMED`/`FAILED` terminal | docs `trading/realtime-order-updates`; py-sdk `models/clob/user_events.py:33-38`, `models/clob/account.py` |
| User-channel subscription frame `{"auth":{apiKey,secret,passphrase},"markets":[cid],"type":"user"}` (omit `markets` for the whole account) and hot `{"operation":"subscribe"|"unsubscribe","markets":[…]}` | docs `trading/realtime-order-updates`; py-sdk `streams/clob/user_protocol.py:35-56` |
| The user stream does **not** replay missed changes: after reconnecting, fetch open orders and recent trades over REST before resuming | docs `trading/realtime-order-updates` ("Recover After Reconnecting"); docs `trading/market-making` |
| Heartbeat cancel threshold is **10 s**: "if heartbeats are started and one isn't sent within 10s, all orders will be cancelled" | py-clob-client `py_clob_client/client.py:715` (docstring of `post_heartbeat`); clob-client `src/client.ts:1144` (docstring of `postHeartbeat`) — both SDKs, identical wording |
| Heartbeat path is `POST /v1/heartbeats`, chaining the returned `heartbeat_id`; `null`/absent starts a chain | py-clob-client `endpoints.py:48` + `client.py:713-727`; clob-client `src/endpoints.ts:81` + `src/client.ts:1147-1154` |
| Heartbeat response shape `{"heartbeat_id": string, "error"?: string}` | clob-client `src/types.ts:757-760` (`HeartbeatResponse`) |
| A second, differently shaped heartbeat endpoint is documented: `POST /heartbeats` → `{"status":"ok"}`, with 401/500 `{error, code, retry_after_seconds}` | docs `api-reference/trade/send-heartbeat` (embedded OpenAPI `clob-openapi.yaml`) |
| Cadence "send a heartbeat every 5 seconds"; "if a valid heartbeat is not received within 10 seconds, all open orders owned by those CLOB API credentials are canceled"; "the cancellation check runs every five seconds, so cancellation may occur up to five seconds after the timeout"; HTTP 400 `{"error_msg":"Invalid Heartbeat ID","heartbeat_id":"<expected>"}` → re-sign with that id | docs `trading/manage-orders`, section "Order Heartbeats" (narrative documentation; the 5 s cadence and the 5 s evaluation interval are **not** corroborated by the SDKs, only the 10 s threshold is) |
| Market/user WSS keep-alive is an application-level text `PING`/`PONG` every 10 s (stale at ~30 s) | docs `market-data/websocket/overview`; py-sdk `streams/clob/heartbeat.py:8-9` |
| `GET /data/orders`, `GET /data/trades` (cursor `next_cursor`, terminator `LTE=`), `GET /data/order/{id}`, `GET /balance-allowance?asset_type&signature_type&token_id` → `{balance, allowances{spender:amount}}`, `DELETE /order`, `/orders`, `/cancel-all`, `/cancel-market-orders` | py-sdk `_internal/actions/account.py:62-137,190-227`, `models/clob/account.py:201-241`, `_internal/actions/orders/cancel.py`; py-clob-client `endpoints.py` |
| Gamma market fields: `conditionId, active, closed, archived, acceptingOrders, enableOrderBook, negRisk, restricted, clobTokenIds, outcomes, orderMinSize, orderPriceMinTickSize, feesEnabled, feeSchedule, secondsDelay`; trade-ready = `active && !closed && acceptingOrders` | py-sdk `models/gamma/market.py:486-560`; docs `market-data/market-details` |
| Protocol-v2 position ids (`(value & (((1<<64)-1)<<40)) == 0`) settle through the Combos exchange/PositionManager, not the CTF exchanges | py-sdk `_internal/protocol.py:12-29`, `_internal/actions/orders/context.py:66-71` |
| pUSD exists and is "the collateral token used for all trading on Polymarket" | docs index `llms.txt`; docs `resources/contracts`; docs `concepts/pusd` |

## 4. HIPÓTESIS / [NO VERIFICADO]

| Item | Why it is not verified | How to verify |
|---|---|---|
| `orderID` returned by `POST /order` equals the local EIP-712 digest | True for CLOB V1 and computable in the legacy TS SDK (`getOrderHash`), but no current official source states it for V2 | The recorder logs `NOTE: venue orderID equals the local EIP-712 digest` the first time it happens; compare one real order (canary item H13) |
| `fd.e` (fee exponent) semantics beyond exponent = 1 | docs describe `feeSchedule.exponent` as "exponent applied to the price component" without a formula for ≠ 1 | Compare `/clob-markets` `fd` with the `fee_rate_bps` of a real fill on a fee-enabled market |
| `GET /fee-rate` `base_fee` units (bps vs. rate) | The legacy SDK returns it as `base_fee` without units; the docs page was not reachable for the unit statement | Cross-check `base_fee` against `fd.r` and a real fill's `fee_rate_bps` |
| Exact venue behaviour when a heartbeat 400 is answered after the cancellation window | Documented as "cancel all open orders"; the ordering of the 400 and the cancellation is not specified | Canary item H8 with a real resting order |
| Whether `GET /data/orders` returns orders from *other* API credentials of the same wallet | docs note session-key/deposit-wallet visibility restrictions | Compare with two credential sets on one wallet |
| Which heartbeat **path** production serves today: `/v1/heartbeats` (both official SDKs) or `/heartbeats` (official OpenAPI page) | The two official sources disagree | The client tries the SDK path first and falls back once on 404, remembering which answered (`ClobApiClient::heartbeat_path()`, logged). Verify with one authenticated `curl` per path, or read the first canary log line |
| Which **chain-start body** production accepts: `{"heartbeat_id":""}` (narrative docs) or `{"heartbeat_id":null}` (both SDKs) | The two official sources disagree | The client sends the documented empty string first and falls back to `null` on a 4xx, remembering the form (`heartbeat_uses_null_start()`). Verify by observing the first canary beat |
| Whether the 5 s evaluation interval (and therefore the ~15 s worst case) is still current | Only the narrative docs state it; the SDKs mention only the 10 s threshold | Time it in the canary: stop the beats with one resting order and measure when the venue cancels (item H8) |
| Latency/SLO of the user channel and of `POST /v1/heartbeats` under load | Not documented | Measure during the canary |
| RPC provider reliability for `POLYGON_RPC_URL` | py-sdk defaults to `https://polygon.drpc.org`; no SLA is published | Run the backup-URL failover path in production and watch `rpc_failover` |

## 4b. Heartbeat margin analysis (requested verification, 2026-10-03)

The venue's contract, as verified above: a **valid** heartbeat must arrive at
least every **10 s** or every open order owned by those credentials is cancelled;
the narrative docs add that the cancellation check runs every 5 s, so the actual
cancellation lands somewhere between 10 s and 15 s after the last valid beat.

Our parameters and why each number is where it is:

| Parameter | Value | Reason |
|---|---:|---|
| `BOT_HEARTBEAT_INTERVAL_MS` | 5000 | the documented cadence |
| request connect/total timeout | 1000 / 2500 ms | a beat must not outlive the interval (`validate()` enforces `total_timeout < interval`) |
| worst case between two **acknowledged** beats | 7500 ms | interval + one request timeout, because the scheduler is *deadline*-based: the next beat is due one interval after the previous **deadline**, not after the previous response, so a slow or failed request cannot push the following beat later |
| margin below the venue threshold | **2500 ms** | `validate()` rejects any configuration where `interval + total_timeout > 8000 ms`, i.e. it requires at least 2 s of slack |
| `BOT_HEARTBEAT_WARN_MS` | 7000 | observability only |
| `BOT_HEARTBEAT_BLOCK_MS` | 9000 | stop placing new orders **before** the venue can cancel (1 s of slack); `validate()` rejects `block > 10000` |
| `BOT_HEARTBEAT_ASSUME_CANCELLED_MS` | 10000 | the earliest instant the venue may already have cancelled; `validate()` rejects anything above 10000, because a later value would keep trading while resting orders may be gone |
| `BOT_HEARTBEAT_MAX_FAILURES` | 2 | two consecutive failures (or one ambiguous failure past the block threshold) → `UNREACHABLE` |

Two clarifications that matter operationally:

* `assume_cancelled` never *frees* exposure. It only marks the account
  `BLOCKED` and forces a reconciliation, which reads the venue's open orders
  authoritatively. Believing orders are gone when they are still resting is the
  unsafe direction (it would allow a duplicate), and no timer in this codebase
  can produce that belief.
* The earlier plan's "block at 10 s" was tightened to 9 s for exactly this
  reason: blocking at the venue threshold leaves no margin for one slow request.

Fixed as a result of this verification (see `core/src/order_heartbeat.hpp`):
deadline-based scheduling with the pure, unit-tested `next_wakeup_ms()`; the
`interval + timeout ≤ 8000 ms` rule; `assume_cancelled ≤ 10000 ms`; automatic
path and body-form fallbacks for the two documented heartbeat shapes.

## 4c. What the loopback venue caught (value of item 3)

`tests/support/local_venue.hpp` + `tests/integration/test_local_venue.cpp` run the
production components on real sockets and threads. The first run found a
**production bug that no offline test could see**:

* `ws::Session::connect()` sent the subscription frame *before* marking the
  session connected, and `send_frame()` refuses to write on a session that is not
  connected — so the user channel would have completed the handshake and then
  failed to subscribe on every attempt (`SessionError::SEND_FAILED`, endless
  reconnects, no fill visibility). Fixed by marking the session connected as soon
  as the handshake validates, before the first write.

It also forced two deliberate hardening decisions: `clob::CurlTransport` now
refuses plain `http://` for any non-loopback host (opt-in loopback only, for the
test venue), and `user_ws::Config::validate()` only accepts `ws://` for loopback
with an explicit test flag — production stays `wss://`-only, and
`LightweightCLOBClient` remains HTTPS-only through `CURLOPT_PROTOCOLS_STR`.

## 5. Build and test (real results, 2026-10-03)

Toolchain used inside the development sandbox (no apt access): cmake 4.4.3 +
ninja from a venv, libsecp256k1 built from the pinned commit
`6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d`, OpenSSL 3.0.20 headers generated
from the matching tag and linked against the system `libssl.so.3`/`libcrypto.so.3`,
curl 7.88.1 headers linked against the system `libcurl.so.4`.

```bash
cmake -S core -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=ON -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/path/to/secp256k1
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Results actually obtained in this sandbox:

| Variant | Build | Tests |
|---|---|---|
| Release, `CROWDINTEL_NETWORK=ON` | ok, 0 warnings | 6/6 pass in 13.50 s (`crypto_kat`, `core_units`, `backtester_smoke`, `live_safety_units` 4.0 s, `local_venue_integration` 9.0 s, `preflight_rejects_empty_config`) |
| Release, `CROWDINTEL_NETWORK=OFF` | ok, 0 warnings | 5/5 pass in 4.48 s (`local_venue_integration` needs the network stack and is not built) |
| Debug, ASan+UBSan | ok, 0 warnings | 6/6 pass in 13.86 s; earlier run found a real stack overflow (ledger instances are >1 MiB) — fixed by heap-allocating every ledger |
| Debug, TSan | ok, 0 warnings | 6/6 pass in 15.21 s with **zero** ThreadSanitizer warnings, including the loopback integration (user-channel thread + gateway thread + ledger) and the ledger stress test; the first TSan run of the integration test flagged the *test* reading ledger maps without `guard()` — fixed by adding locked accessors (`fill_copy`, `position_copy`, `order_count`, `last_run_copy`) |

`local_venue_integration` only exists in network builds (it needs libcurl and the
WebSocket session); the offline variant runs the other five.

Smoke runs (no venue egress available in the sandbox):

```text
BOT_MODE=paper … crowdintel_bot   → paper mode, egress disabled, no crash
BOT_MODE=paper … (network build)  → metadata pipeline fails closed with
                                    "condition_lookup_failed", market WSS retries,
                                    trading never enabled
crowdintel-config validate infra/config/production.env.example → keys=65 errors=0 ok=true
crowdintel-preflight (BOT_MODE=live, dummy key) → CONFIG_ERROR / exit 2
```

## 6. Open risks and what a human must verify

1. **No live venue validation was possible here.** The sandbox has no egress to
   `clob.polymarket.com`, `gamma-api.polymarket.com` or any Polygon RPC. Every
   network path is now exercised twice — offline against fixtures derived from the
   official SDK shapes, and end-to-end against the in-process loopback venue on
   real sockets/threads — but the first contact with the real venue still happens
   in the canary (items H2–H13).
2. **Heartbeat credential ownership.** Enabling the heartbeat cancels *every*
   order owned by those credentials when the beats stop. Dedicated credentials
   per heartbeat-owning process are mandatory; sharing them with another bot or
   with a manual session will cancel that session's orders.
3. **Allowances are never set by this code.** `crowdintel-preflight` reports the
   exact target (`BOT_TARGET_ALLOWANCE`, or `BOT_MAX_EXPOSURE_USD` + 10 %) and
   fails until an operator approves that amount. `approve(max_uint256)` is not
   used anywhere.
4. **Protocol-v2 position ids are refused by default** (`BOT_ALLOW_PROTOCOL_V2=0`)
   because they settle through the Combos exchange and the PositionManager, a
   path this signer/allowance set does not cover.
5. **Signature type 3 (POLY_1271 / ERC-7739) still fails closed**, by decision:
   the canary runs on an EOA (type 0). The wrapping is fully specified by py-sdk
   `_internal/actions/orders/typed_data.py` (app domain separator + contents hash +
   type string + 2-byte length trailer), so implementing it is a bounded task for
   after the canary; it is new cryptographic code with no venue-side KAT and does
   not serve the current objective.
6. **The market listener still carries its own WS/TLS plumbing.** Tracked debt
   with a written migration path (`core/src/ws_session.hpp`); deferred by decision
   to after the canary (`docs/CANARY_CHECKLIST.md` §5).
7. **A shared ledger directory between two instances is fatal by design**
   (two writers on one journal). Deployment must mount one volume per instance.
8. **`GET /data/orders` pagination beyond 4 pages** (default `BOT_RECON_MAX_PAGES`)
   blocks with `venue_order_set_truncated`; a canary with many resting orders
   needs a higher value.
