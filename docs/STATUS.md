# Project status

_Last updated: 2026-10-03, after the pre-canary audit (F-01..F-27, recorded in
[../CHANGELOG.md](../CHANGELOG.md)). This file is the current status;
[REMEDIATION_STATUS.md](REMEDIATION_STATUS.md) is the historical ledger of the
2026-09-22 remediation pass, and
[LIVE_SAFETY_2026-10-03.md](LIVE_SAFETY_2026-10-03.md) holds the cited evidence for
the live-safety work. The entry point in Spanish is [../README.md](../README.md)._

## Audit result (2026-10-03)

A strict C++20 audit (connectivity, configuration, tests, concurrency, fail-closed,
docs) found and fixed 27 defects: 1 blocking, 2 high, 12 medium, 12 low.

The blocking one: `crowdintel-preflight` never called
`MarketConfig::finalize_identity`, so `signer_identity` compared the derived EOA
against a zeroed `cfg.signer` and could never pass (no token, therefore
`BOT_ENABLE_LIVE_TRADING=1` was unauthorisable and the canary could not start),
while `maker_funder` and `api_owner` passed vacuously on zeros and the
balance/allowance/position checks queried the zero address instead of the operator's
wallet. Identity is now bound in both binaries, both print
`wallet=`/`maker=`/`api_address=`, a CTest runs the real binary
(`preflight_binds_wallet_identity`), and `preflight::Runner::run` has unit coverage
for the first time.

Other notable fixes: `BOT_METADATA_MAX_AGE_MS` was validated but never enforced;
`BOT_CONFIG_FILE` and `BOT_REQUIRE_STARTUP_RECONCILIATION` were decorative knobs;
the kill switch only ran in the live branch; `post_raw`/`post_order` could put a
signed order on the wire bypassing gateway/observer/reservation/gate; paper mode
inherited the replay test token id; `HmacSha256` had uninitialised mid-states
(a real SEGV in Release); `latency_bench` measured nothing (0/10000 productive,
`p50=0`) while the CI gate passed it; 62 of 80 environment keys had no test of their
env-to-field wiring; and L1 (`ClobAuth`) authentication had no known-answer vector —
it now matches the official `py-clob-client` vector byte for byte.

Verification of the final tree: clean from-scratch builds in four configurations
(Release+network, Release offline, ASan+UBSan, TSan) with **zero warnings** under
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`; CTest 7/7, 6/6, 7/7, 7/7; **100
repeated TSan iterations** (50 × `test_live_safety`, 50 × `test_local_venue`) with no
report; Cppcheck 2.17.1 with 0 errors and 0 warnings; 19 deliberate mutations, 17
detected and 2 documented as equivalent. Measured CPU path: decision + presigned pool
+ submit p50 ≈ 0.44 µs, inline signing p50 ≈ 34-38 µs (≈33 µs of it Keccak+ECDSA).

## Verified in this repository

- Offline and network-enabled C++20 builds complete with pinned libsecp256k1.
- Native CTest covers crypto KATs, core/concurrency, strict HTTP/WSS/JSON, secret files (LF/CRLF/no terminator/empty/overflow/unreadable/permissive/symlink/missing), and replay smoke.
- Standard-domain EIP-712 digest/signature independently matches Python PyCryptodome + coincurve.
- GCC and Clang, network/offline, ASan+UBSan, TSan, and the production container are CI gates; all six hosted jobs passed on the remediation branch on 2026-09-30.
- Mock executable submits/rejects deterministically without touching the network.
- The consumable pre-signed path and inline signing fallback are benchmarked with production components.

## Intentionally fail-closed

- Signature type 3 / deposit wallet: correct ERC-7739 wrapper is not implemented.
- Unknown ticks, stale/invalid/crossed books, stale/duplicate alpha, invalid identity, exhausted/mismatched signatures, risk-limit violations, and unsuccessful CLOB response bodies.
- Ambiguous transport outcomes are not blindly retried.
- systemd does not auto-restart after failure because account state may be unknown.

## Not verified

- No live order/fill has been claimed from this environment.
- Type 1/2 account identity combinations need exact official-SDK/live comparison.
- Target-host DNS/TCP/TLS/HMAC/venue latency is unmeasured.
- Current market tick, minimum, fees, balances, allowances, and inventory require metadata/account preflight.
- WSS schema behavior needs a long live soak and fault injection.

## Production blockers

Implemented in code and covered by offline tests as of 2026-10-03 (see
[LIVE_SAFETY_2026-10-03.md](LIVE_SAFETY_2026-10-03.md) and
[CANARY_CHECKLIST.md](CANARY_CHECKLIST.md)):

1. Authenticated private user/order/fill channel — `core/src/user_ws_client.hpp`.
2. Startup and post-disconnect reconciliation of open orders, fills, balances,
   allowances, reservations and inventory — `core/src/reconciliation.hpp` over a
   persistent write-ahead ledger (`core/include/event_ledger.hpp`).
3. Automated readiness — `crowdintel-preflight` plus the supervisor thread's
   `READY|BLOCKED` gate; live egress additionally requires a fresh preflight
   pass token matching the effective configuration fingerprint.
4. Dynamic venue metadata — tick size, minimum order size, negative-risk flag,
   fee schedule, market status and token identity are fetched and cross-checked
   before trading is enabled.

Still open, and the reason this project is **not** approved for unattended
real-money deployment:

- No live order, fill, heartbeat or reconciliation has been observed from a real
  account: every network path is verified against fixtures derived from the
  official SDKs, not against the venue. Canary items H1–H15 in
  [CANARY_CHECKLIST.md](CANARY_CHECKLIST.md) must be executed by an operator.
- Signature type 3 (POLY_1271 / ERC-7739) still fails closed.
- Items marked `[NO VERIFICADO]` in
  [LIVE_SAFETY_2026-10-03.md](LIVE_SAFETY_2026-10-03.md) §4 — notably whether the
  venue `orderID` equals the local EIP-712 digest, and the exact fee-exponent
  semantics.
- The market listener still carries its own WebSocket/TLS plumbing (tracked
  duplication with a written migration path).
- The preflight pass token binds the configuration fingerprint, not the wallet: a
  credential swapped between preflight and start is only caught by the `wallet=`
  comparison in checklist H2/H3 and by the startup reconciliation. Binding the address
  into the token is an open design decision (documented, not applied).
- `MetadataPipeline` has no direct test of its success path (its parsers, validators
  and quantum helpers are covered, and its failure path is covered through the
  preflight tests); `main_hot_path.cpp` wiring is exercised only by the loopback
  integration test, the CI container smoke run and manual runs.
