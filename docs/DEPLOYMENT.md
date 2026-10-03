# Deployment and rollback runbook

## Current release gate

**NO-GO for unattended live capital.** The offline/network builds, the replay/paper simulation, cryptographic KATs, parser tests, concurrency tests, sanitizers, and replay tests are release gates. They do not replace:

1. an authenticated private order/fill channel (implemented in Phase 3: `/ws/user`);
2. startup and post-disconnect reconciliation of open orders and fills (Phase 5), plus account balance/allowance proof before arming (Phase 6: `crowdintel-preflight` and the same check inside the bot). *Continuous* reconciliation and inventory accounting are still missing;
3. independent live comparison with the current official SDK for the exact account/market/signature type;
4. measured DNS/TCP/TLS/HMAC/venue latency on target hardware;
5. controlled failure injection against staging or a disposable canary account.

Until the remaining items exist, any live canary must be human-supervised, tiny, FAK-only, externally reconciled after every order, and stopped on the first ambiguity. This is a validation procedure, not an endorsement for production trading.

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
3. Copy `infra/config/production.env.example` to `/etc/crowdintel/config`; owner `root:crowdintel`, mode `0640`. It contains no secrets and it is a plain `KEY=value` file that systemd's `EnvironmentFile=` already parses — there is no TOML and no second parser in the process. `BOT_MODE=live` is mandatory; every `BOT_`/`CLOB_`/`GAMMA_`/`WS_` name the build does not read aborts startup, so a leftover from an older deployment cannot be ignored. Validate the file with `crowdintel-metadata` (read-only, loads no keys) before restarting the service.
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
| Token | `crowdintel-metadata` resolves an outcome token equal to the intended outcome; market is active/order-enabled. `BOT_TOKEN_ID` is a replay-only fixture. |
| Domain | standard vs negative-risk exchange address agrees with metadata. |
| Signature type | account is exactly type 0/1/2 with maker/signer/API-owner mapping verified. Type 3 is NO-GO. |
| Tick/min size | current metadata matches configured initial tick/minimum; dynamic tick event tested. |
| Fees | The taker fee curve printed by `crowdintel-metadata` matches the current venue document; `BOT_TAKER_FEE_RATE` is a replay-only fixture. |
| Balances | collateral and outcome balances independently queried. |
| Allowances | exchange/spender allowance is sufficient but least-privilege. |
| Inventory | open orders + settled/unsettled fills reconciled; `BOT_INITIAL_POSITION_SHARES` documented. |
| Risk caps | max order/exposure/daily loss fit disposable canary capital. |
| Credentials | API owner, signer and maker relations verified with an authenticated read. |
| Clock | host offset is within operational bound; alerting active. |
| Mode | `BOT_MODE=live` and `BOT_ENABLE_LIVE_TRADING=1`; `crowdintel-metadata` accepts the same file in `paper`, and `crowdintel-preflight` accepts it in `live` without a signing key present. |
| User channel | The unit opens the authenticated user WSS channel (`/ws/user`) and sends `PING` every 10 s. Outbound firewall rules must allow `wss://ws-subscriptions-clob.polymarket.com:443` in addition to the market channel; a session that stops receiving data is dropped and reconnected with backoff, and any divergence in the private feed pauses trading until reconciliation. |
| Order heartbeat | Outbound HTTPS to `CLOB_HOST` must stay open while orders live: `POST /v1/heartbeats` every 5 s keeps them alive, and after 10 s without a valid heartbeat the venue cancels **all** open orders of those credentials. The heartbeat is the same host/credentials as order submission, so no extra rule is needed, but a network policy that silently blackholes `clob.polymarket.com` must be treated as an order-cancelling event. On the first 10 s gap the bot pauses trading and marks non-terminal orders `UNKNOWN` (`TRADING PAUSED reason=heartbeat_lost`); alert on `heartbeat state=CRITICAL` in the session summary. |
| Order journal | `/var/lib/crowdintel/orders.journal` (`BOT_LEDGER_PATH`), owned by `crowdintel`, mode `0640`. The unit grants it through `StateDirectory=crowdintel`. The format magic is `CILG0004`: a journal from an older format (`CILG0003`/`CILG0002`) or with a different payload size is refused as corrupt, so an upgrade must migrate or quarantine the file instead of letting it be reinterpreted. Startup refuses to arm while any recovered order is not terminal: in live mode the Phase 5 reconciliation queries the venue first, and only a human quarantines (`mv` the file aside and record why) when the order is provably closed. A corrupted journal also blocks startup until a human inspects it. |
| Config validation | Before restarting with a new `/etc/crowdintel/config`: `crowdintel_bot --check-config` with the bot's full environment, or `crowdintel-metadata --check-config` without secrets. Both exit non-zero with the exact startup message on any unknown `BOT_`/`CLOB_`/`GAMMA_`/`WS_` name, so a typo cannot silently become "default". |
| Account preflight | `infra/scripts/run_preflight.sh [--json] [--refresh-allowances]` runs `crowdintel-preflight` (Phase 6) from an environment of exactly the config file plus the L2 credential files, because the tool refuses to start with signing-key material present. It is read-only and needs no private key: it proves the L2 credentials, the venue clock, the closed-only flag, the collateral balance and the allowances that the order will name. Run it with the bot's environment (minus the signing key) before arming, and again after any allowance/approval change; `--refresh-allowances` is the only call that changes venue state and is always an explicit operator action. Exit 0 = READY, 1 = BLOCKED. |
| Reconciliation | Outbound HTTPS `GET /auth/ban-status/closed-only`, `GET /data/orders`, `GET /data/trades`, `GET /data/order/{id}` on `CLOB_HOST` with the same L2 headers as order submission. Live startup runs it *before* the signer exists and exits on any non-READY gate; a user-channel drop/reconnect, a heartbeat critical or an unexplained divergence pauses trading and re-runs it (immediate after a reconnect, then every 30 s while paused), so the outbound policy must not blackhole these reads. `READINESS = BLOCKED reason=open_orders` means the venue proved a resting order: this build trades FAK, so it stays unarmed and expects a human (the missing heartbeat cancels venue orders within 10 s). |
| Kill switch | creating `/run/crowdintel/kill` disables live enqueue/egress, triggers process shutdown, and discards unsent work. |
| Alpha ingress | bound to loopback/trusted path; bearer rejection and body cap tested. |
| TLS | CA validation works; if pinning is enabled, primary and rotation/backup pin plan tested. |

## 4. Shadow gate

Start with live market data but no order capability: `BOT_MODE=paper` runs the
real market WSS, the real metadata resolver and the real alpha ingress, while
every fill is simulated by the local client and no order can be sent. It needs
no venue credentials and no journal (they are unavailable in that mode by
construction), so the shadow gate no longer needs a credential-isolated host.
Validate:

- WSS snapshot/deltas, textual `PING`/`PONG`, dynamic tick changes, disconnect invalidation;
- book age and signal age rejection;
- alpha authentication, malformed JSON, duplicate IDs, wrong market, and overload;
- predicted wire amounts against an independently generated official-SDK order;
- p50/p95/p99/p99.9 decision, queue, DNS, connect, TLS, request, first-byte, and semantic-ack timings separately.

The current WSS resolver can block in `getaddrinfo`; test resolver outage explicitly. TLS public-key pinning is optional because a stale single pin is also an availability risk. Neither limitation may be hidden by the CPU benchmark.

## 5. Controlled canary (only after explicit owner approval)

0. Verify the heartbeat reaches the venue (`heartbeat acks` grows in the log while no order is open) before any order is armed; without it the first 10 s gap cancels everything sent.
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
4. disarm with `BOT_ENABLE_LIVE_TRADING=0` (and `BOT_MODE` stays `live` until the file is re-validated);
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
