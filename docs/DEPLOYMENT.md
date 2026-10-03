# Deployment and rollback runbook

## Current release gate

**NO-GO for unattended live capital.** The offline/network builds, mock transport, cryptographic KATs, parser tests, concurrency tests, sanitizers, and replay tests are release gates. They do not replace:

1. an authenticated private order/fill channel;
2. startup and continuous reconciliation of open orders, fills, balances, allowances, and inventory;
3. independent live comparison with the current official SDK for the exact account/market/signature type;
4. measured DNS/TCP/TLS/HMAC/venue latency on target hardware;
5. controlled failure injection against staging or a disposable canary account.

Until items 1–2 exist, any live canary must be human-supervised, tiny, FAK-only, externally reconciled after every order, and stopped on the first ambiguity. This is a validation procedure, not an endorsement for production trading.

## 1. Build gate

Use `infra/scripts/deploy_production.sh user@host HOT_CORE COLD_CORE`. It:

- detects whether the remote host satisfies x86-64-v3 and otherwise selects `portable`;
- never uses `-march=native`;
- builds with LTO and the supplied pinned libsecp256k1;
- runs all native tests and the CPU benchmark before copying a binary;
- installs a non-login service account and hardened, fail-closed systemd unit;
- does not create/copy credentials and does not start the service.

Example:

```bash
export SECP256K1_ROOT=/opt/src/secp256k1-v0.8.0
infra/scripts/deploy_production.sh trader@execution-host 2 3
```

The Docker alternative is `infra/docker/Dockerfile.prod`. It uses an immutable Ubuntu manifest, pinned secp256k1 commit, non-root runtime, tests during build, and explicit x86-64-v3. Live containers must receive a sufficient memlock rlimit (for example `--ulimit memlock=67108864:67108864`); live startup now fails if secrets cannot be locked. A truly byte-reproducible supply chain must also use a dated/mirrored Ubuntu package repository; the Dockerfile says this explicitly rather than claiming reproducibility from floating apt metadata.

## 2. Host and secrets

1. Use a dedicated host/account, encrypted disk, restricted SSH, NTP/chrony, and outbound-only firewall rules for required HTTPS/WSS endpoints.
2. Disable core dumps. The generated service unit sets `LimitCORE=0`, removes capabilities, and enables systemd filesystem/kernel/process hardening.
3. Copy `infra/config/production.env.example` to `/etc/crowdintel/config`; owner `root:crowdintel`, mode `0640`. It contains no secrets.
4. Put one value in each root-owned mode-`0400` file:

```text
/etc/crowdintel/credentials/bot_private_key
/etc/crowdintel/credentials/clob_api_key
/etc/crowdintel/credentials/clob_secret
/etc/crowdintel/credentials/clob_passphrase
/etc/crowdintel/credentials/alpha_bearer_token
```

The unit exposes these through `LoadCredential=` and `_FILE` variables. Never use shell history to write keys; use an approved secret manager or protected provisioning channel. Rotate API credentials after suspected disclosure. Keep only a small purpose-specific balance on the execution identity.

## 3. Market/account preflight

Record the evidence and reviewer for every row. Any unknown value is a NO-GO.

| Check | Required evidence |
|---|---|
| Chain/network | Polygon chain ID and endpoint identity agree with current venue docs. |
| Token | `BOT_TOKEN_ID` equals the chosen outcome token; market is active/order-enabled. |
| Domain | standard vs negative-risk exchange address agrees with metadata. |
| Signature type | account is exactly type 0/1/2 with maker/signer/API-owner mapping verified. Type 3 is NO-GO. |
| Tick/min size | current metadata matches configured initial tick/minimum; dynamic tick event tested. |
| Fees | `feesEnabled` and current category coefficient copied into `BOT_TAKER_FEE_RATE`. |
| Balances | collateral and outcome balances independently queried. |
| Allowances | exchange/spender allowance is sufficient but least-privilege. |
| Inventory | open orders + settled/unsettled fills reconciled; `BOT_INITIAL_POSITION_SHARES` documented. |
| Risk caps | max order/exposure/daily loss fit disposable canary capital. |
| Credentials | API owner, signer and maker relations verified with an authenticated read. |
| Clock | host offset is within operational bound; alerting active. |
| Kill switch | creating `/run/crowdintel/kill` disables enqueue/egress, triggers process shutdown and discards unsent work. It is polled in every mode, not only in live, and a path that cannot be checked is treated as engaged. |
| Alpha ingress | bound to loopback/trusted path; bearer rejection and body cap tested. |
| TLS | CA validation works; if pinning is enabled, primary and rotation/backup pin plan tested. |

## 4. Shadow gate

Start with live market data but no order capability. Because this binary currently requires full live arming for its network topology, use a credential-isolated/disposable environment or packet-denied egress while validating:

- WSS snapshot/deltas, textual `PING`/`PONG`, dynamic tick changes, disconnect invalidation;
- book age and signal age rejection;
- alpha authentication, malformed JSON, duplicate IDs, wrong market, and overload;
- predicted wire amounts against an independently generated official-SDK order;
- p50/p95/p99/p99.9 decision, queue, DNS, connect, TLS, request, first-byte, and semantic-ack timings separately.

The current WSS resolver can block in `getaddrinfo`; test resolver outage explicitly. TLS public-key pinning is optional because a stale single pin is also an availability risk. Neither limitation may be hidden by the CPU benchmark.

## 5. Controlled canary (only after explicit owner approval)

1. Use one active, liquid, non-negative-risk market unless negative-risk vectors have independently passed.
2. Type 0 EOA only for the first canary; no deposit-wallet/type-3 workaround.
3. FAK only; one minimum-size BUY whose exact max loss is acceptable.
4. Compare the C++ EIP-712 digest, signature, body, owner, maker/taker amounts, headers, and expiration with the current official SDK **before** sending.
5. Submit once. Do not retry timeout/receive ambiguity.
6. Query order status and fills through an independent authenticated client. Reconcile balances and inventory before any second order.
7. Repeat one SELL only after confirmed inventory. Verify fee and resulting balances.
8. Capture sanitized response/status timings. Never capture secrets, Authorization headers, full credentials, or private key material.

The canary passes only if wire bytes/meaning, semantic status, fill, fee, and balances all agree. “HTTP 200” or gateway-enqueued is not a pass.

## 6. Start procedure

Only after all gates are approved:

```bash
# Set BOT_ENABLE_LIVE_TRADING=1 last.
sudo systemctl daemon-reload
sudo systemctl enable --now crowdintel
sudo systemctl status crowdintel
sudo journalctl -u crowdintel -f
```

The service deliberately has `Restart=no`. A restart can duplicate unknown exposure after an ambiguous request. Reconcile open orders/fills/inventory before every manual restart.

## 7. Emergency stop and rollback

Fast stop:

```bash
sudo install -o crowdintel -g crowdintel -m 0600 /dev/null /run/crowdintel/kill
sudo systemctl stop crowdintel
```

Then, using an independent authenticated client:

1. cancel all open orders;
2. query fills and balances until consistent;
3. record unresolved request IDs/ambiguous submits;
4. disarm `BOT_ENABLE_LIVE_TRADING=0`;
5. rotate credentials if compromise is possible;
6. preserve sanitized logs and monotonic/UTC timestamps;
7. restore the prior root-owned binary, re-run its exact test suite, and do not restart until reconciliation is signed off.

Rollback is not complete when the process exits; it is complete when venue state and local inventory are reconciled.

## 8. Host tuning

`infra/scripts/kernel_tuning.sh` applies only conservative TCP client settings by default. NIC offload changes require `--nic=...`; CPU isolation/C-state changes require `--apply-boot` and are refused inside virtualization. It creates backups and never edits GRUB with a broad regex.

```bash
sudo HOT_CORE=2 infra/scripts/kernel_tuning.sh
# Only after baseline measurements on bare metal:
sudo infra/scripts/kernel_tuning.sh --hot-core=2 --nic=eth0 --apply-boot
```

Compare p99.9 and power/thermal behavior before and after. Revert settings that do not improve the real end-to-end distribution.


## 9. Live-safety operations (Phases 1-7)

### 9.1 Configuration

`/etc/crowdintel/config` is a `KEY=VALUE` file consumed through systemd
`EnvironmentFile=`. Validate it before deploying and never put secrets in it:

```bash
crowdintel-config validate /etc/crowdintel/config     # structure, allowlist, ranges
crowdintel-config render-env /etc/crowdintel/config   # what systemd will export
crowdintel-config keys                                # the recognised key list
crowdintel-config fingerprint                         # fingerprint of the *effective* config
```

Secrets stay in `/etc/crowdintel/credentials/*` (mode `0400`, root-owned) and are
read through the `*_FILE` indirection with `LoadCredential=`.

### 9.2 State directories

| Path | Purpose |
|---|---|
| `/var/lib/crowdintel/state.journal` | append-only event log (orders, fills, balances, heartbeats, reconciliation runs, state events) |
| `/var/lib/crowdintel/state.checkpoint` | atomic snapshot of derived state + idempotency keys |
| `/run/crowdintel/preflight.pass` | preflight pass token (fingerprint + timestamp) |
| `/run/crowdintel/kill` | kill switch; its presence stops egress and the process |

One ledger directory per instance, on durable local storage. Two processes
sharing a journal is unrecoverable by design. A corrupt ledger refuses to open
and the bot refuses to trade; a torn trailing record (crash during append) is
discarded and reported.

### 9.3 Startup sequence

```bash
crowdintel-config validate /etc/crowdintel/config
crowdintel-preflight --json | tee /var/log/crowdintel/preflight.json   # exit 1 blocks
systemctl start crowdintel     # BOT_ENABLE_LIVE_TRADING=1 only after preflight passed
```

The bot then repeats the gating itself: ledger → metadata → chain id → signer →
preflight token freshness/fingerprint → heartbeat contract → user channel
subscription → startup reconciliation. Trading is enabled only after every gate
passes, and the supervisor disables it again on the first breach.

### 9.4 Allowances

`crowdintel-preflight` never approves anything. It reports the required target
(`BOT_TARGET_ALLOWANCE`, or `BOT_MAX_EXPOSURE_USD` + 10 % margin) and fails until
an operator sets exactly that amount for the correct spender:

* pUSD (`0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB`) → the exchange that matches
  the market (`0xE111180000d2663C0091e4f400237545B87B996B`, or
  `0xe2222d279d744050d28e00520010520000310F59` for negative-risk markets);
* CTF outcome tokens (`0x4D97DCd97eC945f40cF65F87097ACe5EA0476045`) →
  `setApprovalForAll` for the same exchange, required only to sell.

`approve(max_uint256)` is never used: an unlimited approval converts any
exchange-contract bug into a total loss of the wallet balance.

### 9.5 Heartbeat ownership

`BOT_HEARTBEAT_ENABLED=1` starts the venue's cancel-on-disconnect contract for
**all** orders owned by those CLOB credentials. Use dedicated credentials for the
process that owns the heartbeat, and prefer an explicit `DELETE /cancel-all` at
shutdown (the binary does this) over relying on the ~10-15 s lapse.
