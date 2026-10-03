# Changelog — Bot_Crowdintel

## Unreleased — loopback integration venue, heartbeat margin verification

### Verified against the official sources (point 5 of the risk review)
- Confirmed the heartbeat contract: cancel threshold **10 s** (py-clob-client `client.py:715` and clob-client `src/client.ts:1144`, identical wording), path `POST /v1/heartbeats` with `heartbeat_id` chaining (both SDKs), response `{heartbeat_id, error?}` (clob-client `types.ts:757`).
- Recorded the two official-source disagreements and made the client tolerate both: the OpenAPI page documents `POST /heartbeats` → `{"status":"ok"}` (path fallback on 404, remembered), and the narrative docs send `{"heartbeat_id":""}` while both SDKs send `{"heartbeat_id":null}` (form fallback on 4xx, remembered). Both are marked `[NO VERIFICADO]` with the verification step.
- Tightened the cadence margin: the scheduler is now deadline-based (`next_wakeup_ms`, pure and unit-tested) so a slow or failed request cannot push the next beat later; worst-case gap between acknowledged beats is `interval + request timeout` = 7.5 s, i.e. **2.5 s below** the venue threshold. `Config::validate()` now rejects `interval + timeout > 8000 ms` and `assume_cancelled > 10000 ms` (the earliest instant the venue may have cancelled). Local block stays at 9 s, before the venue's 10 s.

### Loopback integration venue (point 3)
- `tests/support/local_venue.hpp`: in-process HTTP/1.1 + WebSocket server on 127.0.0.1 with scriptable responses and adversarial faults (no response, close mid-body, delayed answer, invalid body with HTTP 200, partial WebSocket frame then hangup).
- `tests/integration/test_local_venue.cpp` (`local_venue_integration` in CTest, network builds): duplicated and out-of-order user events, disconnect mid-message with reconnect on the same thread, heartbeat rejected/delayed and the OpenAPI path fallback, HTTP 200 with an invalid body, `POST /order` timeout → `UNKNOWN` with exactly one request sent and trading disabled, ticket linked by reconciliation, and a restart with the order still open.
- Found and fixed a **production bug** that no offline test could see: `ws::Session::connect()` sent the subscription frame before marking the session connected, so the user channel would have failed to subscribe on every attempt.
- Hardening that came out of it: `clob::CurlTransport` refuses plain `http://` except for loopback with an explicit opt-in, and `user_ws::Config` only accepts `ws://` for loopback with a test-only flag; production stays `wss://`/HTTPS-only.
- `core/src/ledger_order_observer.hpp`: the egress↔ledger bridge extracted from `main` so the integration test exercises the production observer rather than a copy.
- Locked ledger accessors (`fill_copy`, `position_copy`, `order_count`, `fill_count`, `metadata_copy`, `last_run_copy`) for cross-thread readers; TSan flagged the test's direct map reads and they are gone.

### Decisions recorded (points 1, 2, 4)
- Signature type 3 stays fail-closed; documented as post-canary work.
- The market-listener migration onto `ws_session` is deferred to after the canary, with the plan written down.
- `BOT_ALLOW_PROTOCOL_V2=0`, `BOT_RECON_MAX_PAGES=4` and "no automatic approval" are unchanged.
- `docs/CANARY_CHECKLIST.md` gained the mandatory operational rules (exclusive credentials, one process per ledger, one token/fingerprint, manual minimum allowances, EOA for the canary) and the deferred-work table.

## Unreleased — live-safety blockers (Phases 1-8)

### Venue metadata (Phase 1)
- Added dynamic market metadata: condition id / token resolution, Gamma market status, CLOB `/clob-markets`, `/tick-size`, `/neg-risk`, `/fee-rate`, `/book` snapshot, cross-source agreement checks and atomic publication to the hot path (`core/include/venue_metadata.hpp`, `core/src/metadata_pipeline.hpp`).
- Removed the hardcoded trading parameters from the live path: tick size, minimum order size, negative-risk flag, fee schedule, condition id and token id now come from the venue; the environment values are expectations that preflight cross-checks, and `BOT_TOKEN_ID` no longer defaults to a test vector in live mode.
- Added a read-only Polygon JSON-RPC client (`eth_chainId`, `eth_getCode`, `eth_getBalance`, `eth_call`) with derived function selectors and backup-URL failover; contract constants are trusted only after `eth_chainId` answers 137.
- Refused protocol-v2 position ids by default (they settle through the Combos exchange / PositionManager).

### Persistent ledger and order state machine (Phase 2)
- Added an append-only write-ahead journal plus atomic checkpoint, exact idempotency keys, bounded derived state (orders, fills, positions, balances, heartbeat, reconciliation runs, state events) and fail-closed corruption handling — no new third-party dependency (`core/include/event_ledger.hpp`).
- Added the explicit order lifecycle with the mandatory `UNKNOWN` state, a checked transition table, terminal-state protection and mappings from the venue's order/trade/post-response vocabularies (`core/include/order_state.hpp`).
- Added the order recorder: a durable submission ticket keyed by the local EIP-712 digest is written **before** egress, the venue-keyed record replaces it on response, ambiguous outcomes become `UNKNOWN` and are never retried, and FAK partial fills are taken from the `POST /order` response instead of waiting for the stream (`core/src/order_recorder.hpp`, `core/crypto/order_digest.hpp`).
- One ledger mutex serialises the gateway, user-channel and supervisor writers; the hot path reads an atomic inventory publication instead.

### User channel (Phase 3)
- Added `core/src/user_ws_client.hpp` with the protocol split into an offline-testable layer (`user_ws_protocol.hpp`, `user_event.hpp`) and a reusable TLS/RFC 6455 session (`ws_session.hpp`, `ws_url.hpp`).
- Covered placement, update, cancellation, partial/complete fills, error frames, keep-alives, duplicates, out-of-order delivery, foreign orders and events for other markets; every connect/reconnect marks the account view stale until REST reconciliation clears it.

### Order heartbeat (Phase 4)
- Added `core/src/order_heartbeat.hpp`: the `POST /v1/heartbeats` id chain, separate from the WebSocket keep-alive, with a monotonic watchdog (warn 7 s, block 9 s, assume-cancelled 10 s by default, all range-checked against the venue's documented 10 s / 5 s contract) and resynchronisation on HTTP 400.

### Reconciliation and readiness (Phase 5)
- Added `core/src/reconciliation.hpp`: startup and post-disconnect reconciliation over `GET /data/orders`, `GET /data/trades`, `GET /data/order/{id}` and `GET /balance-allowance`, producing `READY` or `BLOCKED` with explicit reasons; unlinked submission tickets are resolved by unique fingerprint match or forced to `UNKNOWN`.

### Preflight (Phase 6)
- Added the `crowdintel-preflight` binary: chain id, CLOB health, clock offset, L2 credentials, optional L1 credential derivation (`core/crypto/clob_auth.hpp`), signer/maker/funder consistency, metadata, contracts, balances, allowances, inventory, user channel, heartbeat, ledger durability and kill switch — any FAIL exits 1 and writes no pass token.
- The bot honours `BOT_ENABLE_LIVE_TRADING=1` only with a fresh pass token whose configuration fingerprint matches the loaded configuration.

### Configuration and modes (Phase 7)
- Confirmed there was never a TOML file in this repository and implemented option B: `crowdintel-config validate|render-env|fingerprint|keys` over a `KEY=VALUE` file, secrets rejected in configuration, unknown prefixed variables rejected at startup, and a SHA-256 fingerprint of the effective configuration.
- Separated `replay` (the `l2_backtester` binary), `paper` (live market data, refusing egress, no simulated fills) and `live`; `mock` remains as a deprecated alias for `paper`, and `MockCLOBClient` is now compiled only into the no-network build.

### Tests and operations (Phase 8)
- Added `tests/unit/test_live_safety.cpp` (ledger WAL/idempotency/recovery/corruption, state machine, metadata parsers and validation, REST client over fixtures, user-channel parsing and application, heartbeat, reconciliation, recorder, ticket linking, multi-threaded ledger stress) and `tests/support/fixture_transport.hpp`.
- Added `docs/CANARY_CHECKLIST.md` and `docs/LIVE_SAFETY_2026-10-03.md`; shipped the new binaries and state directories in the production image; extended CI with configuration/preflight gate smoke tests.

## Unreleased — correctness, safety, and deployability remediation

### Protocol and execution

- Added authenticated bounded alpha HTTP ingress and asynchronous bounded CLOB egress.
- Reworked CLOB V2 amounts/ticks/order types, semantic response handling, safe retry classification, standard/negative-risk domains, and fail-closed type 3 behavior.
- Replaced reusable pre-sign assumptions with a consumable bid/ask risk ladder, three buffers, per-buffer reader/writer registration, and one-time CAS claims.
- Added fee-aware fractional Kelly sizing, exposure/daily-loss breakers, BUY cost reservations, and confirmed-inventory SELL reservations.
- Updated WSS TLS/upgrade validation, heartbeat/schema handling, strict bounded JSON/frame parsing, authoritative empty snapshots, freshness invalidation, and tick-generation binding.

### Concurrency and security

- Replaced unsafe raw SPSC storage with typed non-copyable storage.
- Added atomic coherent top-of-book publication and mutex-protected depth snapshots.
- Replaced weak salt fallback with OS-seeded ChaCha20 and 53-bit wire-safe salts.
- Added strict regular/non-symlink `_FILE` secret inputs, permission/size/control-character checks, zeroization, systemd credentials, non-root/capability-free deployment, and disabled core dumps.
- Restricted ambiguous POST retries, made the kill switch cancel unsent egress, and disabled automatic service restart pending reconciliation.

### Verification and operations

- Added GCC/Clang network/offline CI, ASan+UBSan, TSan, production-container builds, pinned dependencies/Actions, independent Python EIP-712 checks for both exchange domains, and CPU gross-regression budgets.
- Added HTTP, gateway, concurrent pool, parser, risk/math, secret-file, replay, and concurrency tests.
- Changed replay PnL to next-tick marking with V2 fees and no unmarkable final trade.
- Added portable/x86-64-v2/x86-64-v3 CPU contracts, hardened Docker/systemd definitions, conservative reversible host tuning, configuration reference, security model, and deployment/rollback runbook.
- Removed duplicate generated crypto trees, unused market-making/parser translation units, fictional optimization lineage, stale benchmark claims, and contradictory readiness documents.

### Known blockers

- Private user-channel fill/order updates and startup/continuous account reconciliation are not implemented.
- Signature type 3 needs a verified variable-length ERC-7739 wrapper.
- Live SDK/wire/account validation and target-hardware/network measurement remain controlled deployment gates.
