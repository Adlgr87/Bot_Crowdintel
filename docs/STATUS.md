# Project status

_Last updated: 2026-09-30. The authoritative observation-by-observation ledger is [REMEDIATION_STATUS.md](REMEDIATION_STATUS.md)._

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

1. Authenticated private user/order/fill channel.
2. Startup and continuous reconciliation of open orders, fills, balances, allowances, reservations, PnL, and inventory.
3. Automated readiness that includes public feed and private account-state health.
4. Controlled canary evidence against the current official SDK and exact account/market.

Until those are resolved, the project is suitable for offline research, replay/paper shadow validation, and tightly supervised disposable canaries only—not unattended real-money deployment. Follow [DEPLOYMENT.md](DEPLOYMENT.md).
