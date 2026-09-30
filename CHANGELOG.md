# Changelog — Bot_Crowdintel

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
