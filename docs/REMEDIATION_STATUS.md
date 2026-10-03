# Remediation ledger

Status vocabulary:

- **Implemented/tested offline:** code exists and deterministic tests/builds pass.
- **Fail-closed:** unsafe mode is deliberately rejected.
- **Partial:** meaningful controls exist, but a production dependency remains.
- **Live/hardware verification required:** no offline claim is made.

| # | Observation | Resolution | Status / residual work |
|---:|---|---|---|
| 1 | Live path had no alpha HTTP receiver | Added bounded `POST /signal`, bearer authentication, strict HTTP/JSON framing, duplicate-field rejection, a 250 ms absolute request deadline, required fields, market normalization, TTL-compatible timestamping, and live topology integration. | Implemented/tested offline. Native TLS is intentionally delegated to loopback/trusted reverse proxy. |
| 2 | CLOB V2 signing appeared invented/incompatible | Implemented EIP-712 V2 domain/struct hashing, Keccak-256, recoverable low-S secp256k1, standard/negative-risk exchange selection, KATs, recovery tests, and independent Python/coincurve cross-check. | Offline cross-check passes for both standard and negative-risk domains; exact live SDK comparison remains required for each account/market mode. |
| 3 | Floating-point precision and BUY/SELL amount rules were wrong | Replaced wire arithmetic with integer fixed point, exact tick quanta, 0.01-share truncation, distinct BUY/SELL formulas, FAK/FOK collateral quantization, decimal-string JSON, and unknown-tick rejection. | Implemented/tested offline; live SDK golden matrix remains required. |
| 4 | Kelly sizing ignored fees, inventory, exposure, and losses | Added fee-adjusted edge/sizing, fractional Kelly, max order/exposure/daily-loss caps, BUY worst-cost reservation, SELL confirmed inventory reservation, and rejection breakers. | Partial: fills/PnL cannot be authoritative until reconciliation exists. |
| 5 | WebSocket/book could trade stale or disconnected data | Current WSS endpoint, verified TLS/upgrade headers, textual `PING`/`PONG`, canonical bounded frames, strict JSON/field parsing, reconnect invalidation, authoritative empty snapshots, age/crossed-book checks, snapshot/delta handling, and fail-closed dynamic tick events. | Implemented/tested with fixtures; live schema drift/soak required. |
| 6 | Queues used unsafe raw storage/lifetimes | Replaced byte storage with typed object storage and explicit SPSC release/acquire protocol; added FIFO and threaded tests. | Implemented/tested offline and under TSan. SPSC ownership remains a design invariant. |
| 7 | Private key/credentials were insecurely configured | Added unambiguous `_FILE` inputs opened as non-symlink regular files with private permissions, empty/overflow/read-failure rejection, CRLF-safe trimming, control-character/base64 checks, zeroization/env removal, fatal live `mlockall`, systemd `LoadCredential`, non-root service, no core dump, and empty capabilities. | Implemented/tested for process-level handling; signer key must still remain in locked process RAM. HSM/isolated signer is future hardening. |
| 8 | Claimed microsecond latency was not end-to-end | Replaced duplicated strategy benchmark with production engine/pool paths and documented boundaries. Added loose CI gross-regression budgets and segmented production measurement plan. | CPU path verified only; DNS/TCP/TLS/HMAC/venue claims require target measurements. |
| 9 | Order timestamp/expiry semantics were contradictory | Centralized millisecond order timestamp; GTD wire expiry uses seconds plus venue buffer; non-GTD expiry is zero; stale pre-sign entries expire monotonically. | Implemented/tested offline; compare current SDK before canary. |
| 10 | Double buffer could be overwritten while read | Replaced with a three-buffer publication design and reader accounting; metadata inspection uses the same read registration rather than racing a rebuild. | Implemented/tested offline; stress/TSan gate retained. |
| 11 | Presigned orders were detached from actual side/price/size | Added bid/ask ladders, eight risk buckets per side, explicit tick-generation/market/price/TTL matching, largest safe bucket selection, and one-time CAS consumption. Unknown tick changes invalidate the book, and a pool miss falls back rather than crossing generations. | Implemented/tested offline. |
| 12 | Benchmark omitted HMAC/network while headline implied full submit | Renamed/documented CPU measurements and separated mock enqueue from semantic venue acceptance. | Corrected; target-host e2e instrumentation still required. |
| 13 | Pool/benchmark signature counts were inconsistent | Benchmark now reports measured production/rejections, consumes actual slots, and replenishes by batch. | Implemented/tested offline. |
| 14 | No open-order/fill/inventory reconciliation | Engine no longer credits reservations as fills and SELL requires confirmed inventory. Service does not auto-restart after ambiguity. Phases 3-6 added the authenticated user channel, the order heartbeat, the append-only journal with a mandatory `UNKNOWN` state, the startup/post-disconnect REST reconciliation gate and the account preflight (`crowdintel-preflight`); live startup stops before the signer exists while anything is unproven. | Implemented/tested offline. Residual: *continuous* reconciliation and inventory/PnL accounting, plus the live SDK/account validation in the runbook. |
| 15 | Health check/unit could report healthy while unusable | Docker health is explicitly liveness-only; readiness must use metrics. systemd is fail-closed and no-auto-restart. Deployment checklist verifies feed/account separately. | Partial: native machine-readable readiness endpoint is still desirable. |
| 16 | Configuration and README disagreed with code | Added one environment reference, non-secret production template, architecture, security, deployment, benchmark, and status documents. Phase 7 removed the remaining ambiguity: `BOT_MODE` is mandatory (`replay`/`paper`/`live`, `mock` rejected), replay/paper/live requirements are explicit, the order journal and the arming flag are live-only, venue parameters are rejected outside replay, and every unknown `BOT_`/`CLOB_`/`GAMMA_`/`WS_` name aborts startup. The deployed file remains the systemd `EnvironmentFile` (`KEY=value`, no TOML, no second parser); the deploy script and runbook no longer tell operators to set live-rejected variables. | Implemented/tested offline; keep docs in CI/review scope. |
| 17 | Backtester booked artificial same-tick spread PnL | Changed to next-tick marking, V2 fees, and exclusion of an unmarkable final trade. | Implemented/tested smoke. Still not a fill/impact simulator. |
| 18 | WSS parser handled only a narrow obsolete schema | Supports current `book`, `price_change(s)`, best bid/ask, tick-size changes, object arrays and legacy pair fixtures; filters assets and bounds length. | Implemented/tested fixtures; fuzzing and live schema monitoring remain. |
| 19 | Signature type 3 was treated as ordinary 65-byte ECDSA | Type 3 now fails startup with an ERC-7739-specific error. Signature storage/wire must become variable-length before support is added. | Fail-closed. Use official V2 sidecar or implement/verify wrapper; never fake it. |
| 20 | JSON wire assembly/response parsing was brittle | Added bounded canonical field construction with validated identities, exact decimal strings, an allocation-free recursive syntax validator, top-level semantic lookup, duplicate-field rejection, bounded decoding, and response checks for HTTP 2xx + `success:true` + no error + recognized status/order ID. | Implemented/tested offline; coverage-guided fuzzing remains advisable. |
| 21 | Salt used entropy/time unsafely or exceeded JS exact range | Added OS-seeded ChaCha20 CSPRNG and 53-bit wire-safe salts; no clock/address entropy fallback. | Implemented/tested uniqueness smoke; cryptographic assurance inherits OS RNG/ChaCha implementation review. |
| 22 | L2 authentication/HMAC path was incomplete | Persistent HTTPS client computes timestamp/method/path/body HMAC, base64url signature, and identity headers outside hot loop. Secrets can load from files. | Implemented offline; live authenticated-read and SDK header comparison required. |
| 23 | Enqueue/HTTP 2xx was called order acceptance; retries unsafe | `submit()` means queue admission only (`final=false`). Worker records semantic result. Retries are limited to unequivocally safe pre-send failures; ambiguous requests are not replayed. The kill switch gates both enqueue and each egress attempt, while emergency shutdown discards unsent work instead of draining it. | Implemented. An already in-flight ambiguous request still requires private reconciliation. |
| 24 | Docker image was architecture-fragile and ran as root | Multi-stage immutable-base amd64/x86-64-v3 build, pinned secp source, tests during build, minimal non-root runtime, liveness check, and explicit apt-snapshot caveat. | Image build, in-build tests, non-root identity check, and mock smoke passed on GitHub-hosted CI. Registry publication, target-host CPU acceptance, SBOM, and signature remain release work. |
| 25 | `-march=native` made artifacts nonportable | Added `portable`, `x86-64-v2`, `x86-64-v3`, and explicit local-native option; deployment probes target and never chooses native. LTO is configurable. | Implemented/tested offline. |
| 26 | CI did not prove compiler/sanitizer/network variants | Matrix now includes GCC/Clang, network/offline, native tests, signer reference, benchmark smoke, ASan+UBSan, TSan, and a production-container build/non-root check; Actions and secp are immutable pins. | All six hosted jobs passed on the remediation branch on 2026-09-30. Apt snapshots and artifact provenance remain. |
| 27 | Dead/redundant paths increased audit surface | Main and benchmark use production components; removed the unused market-making strategy, empty parser translation unit, duplicate generated crypto tree/header, fictional optimizer lineage, and obsolete reusable-pool assumptions. | Addressed for known paths; continue compiler/static-analysis review before releases. |

## Explicitly unverified hypotheses

The repository does not claim that:

- an order has been accepted by the live CLOB;
- type 1/2 identity combinations match a specific user's current account;
- fees/ticks/minimums stay constant across markets;
- target-host end-to-end latency meets any SLO;
- DNS, TLS pin rotation, exchange outage, packet loss, or clock-jump behavior is production-proven;
- replay PnL predicts live profitability;
- a public-market `paper` run or a `live` run has ever completed a session against
  the venue: this sandbox cannot reach `gamma-api.polymarket.com` /
  `clob.polymarket.com`, so both stop at the first authenticated request
  (`READINESS = BLOCKED reason=gamma request failed: SSL connect error`). Every
  network path added in phases 3-7 is exercised through scripted transports in
  the unit suites, not against the venue;
- the `max` allowance sentinel is what the CLOB returns for an unlimited
  approval (it is documented for the Data API approvals endpoint only), nor that
  the `allowances` map is keyed by the exchange address for a live account;
- the venue accepts this build's TLS stack, its WSS subscribe frames, or its
  heartbeat body: the request/response shapes are taken from the published
  OpenAPI/AsyncAPI documents, which is not the same as a live acknowledgement;
- `BOT_ENABLE_LIVE_TRADING=1` and the journal cannot be reached by a replay or
  paper run in ways other than the validated configuration paths;
- the paper mode's "no order egress" claim is enforced by anything other than
  the absence of a gateway object in that topology (a live run of the same
  binary with `BOT_MODE=live` does build one).

These require current credentials, the exact market/account, independent SDK output, controlled capital, and/or target hardware. The deployment runbook defines the evidence needed before changing those statuses.
