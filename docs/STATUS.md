# Project status

_Last updated: 2026-10-03. The authoritative observation-by-observation ledger is [REMEDIATION_STATUS.md](REMEDIATION_STATUS.md)._

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
