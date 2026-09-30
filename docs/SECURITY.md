# Security model

## Assets and trust boundaries

Assets are the EOA private key, CLOB L2 credentials, collateral/outcome balances, strategy/alpha, order intent, and integrity of market/account state.

Trust boundaries:

1. alpha producer → bounded authenticated HTTP ingress;
2. public market WSS → untrusted bounded JSON parser → book state;
3. process → DNS resolver/CA system/libcurl → CLOB HTTPS;
4. cold threads → bounded queues → hot decision thread;
5. host secret store/systemd credentials → process memory;
6. process intent → venue semantic response → account state.

Public market data, DNS responses, HTTP bodies, alpha payloads, clocks, and process logs must all be treated as attacker-influenced.

## Principal threats and controls

| Threat | Control | Residual risk |
|---|---|---|
| Key exposure in source/config/process list | `_FILE` secrets, systemd `LoadCredential`, root-only files, env removal, zeroized temporary buffers, no command-line secrets | The signer must retain key material in process memory; a privileged host compromise can read it. |
| Secret leakage through core dumps | `LimitCORE=0`, non-root service, no-new-privileges | Host administrator/debug access remains trusted. |
| Forged/replayed alpha | Bearer comparison, required market/IDs/timestamps, TTL, dedupe, body cap | Bearer compromise permits injection; TLS is expected at a trusted reverse proxy if not loopback. |
| Malformed/oversized WSS data | 1 MiB frame cap, bounded parsing, asset filtering, invalidation on disconnect | Parser correctness still needs fuzzing beyond deterministic tests. |
| MITM venue traffic | HTTPS/WSS scheme enforcement, CA/hostname verification, optional public-key pin | CA/host stack compromise; one stale pin can cause outage. Pin rotation is operational. |
| DNS delay/spoofing | TLS hostname verification, connect timeouts | WSS `getaddrinfo` can still block; asynchronous/cached resolver remains open work. |
| Duplicate order after timeout | retries only for unequivocally pre-send failures; ambiguous outcomes not replayed | Without order reconciliation, ambiguity requires a stop and independent query. |
| Signature replay/reuse | CSPRNG salt, bounded timestamp/expiry, consumable CAS slots | Venue semantics and clock quality remain dependencies. |
| Wrong account/signature mode | explicit maker/signer/API-owner validation; type 3 fail-closed | Type 1/2 still require controlled live SDK comparison. |
| Stale/crossed/corrupt book | age checks, atomic top publication, dynamic tick validation, disconnect invalidation | Public feed can be delayed/manipulated; no independent second feed yet. |
| Inventory oversell | confirmed inventory and SELL reservations | Startup inventory is manual; continuous fill reconciliation is missing. |
| Supply-chain substitution | pinned secp256k1 commit, immutable Actions SHA, pinned Python versions, immutable Docker base manifest | Apt package repository is not yet snapshot-pinned; dependency attestations/SBOM are future work. |
| Container/service breakout | non-root, empty capabilities, strict filesystem, syscall/address-family restrictions, no core dump | Kernel and runtime vulnerabilities remain. |

## Credential handling rules

- Never commit real credentials or `.env` files.
- Never send secrets in issue reports, chat, shell arguments, benchmark output, packet captures, or screenshots.
- Provision through a secret manager or protected root-only channel. `*_FILE` inputs must be owner-readable regular, non-symlink files with no group/other permission bits (normally mode `0400` or `0600`); empty, unreadable, or oversized files fail startup even when the process has privilege to bypass mode checks.
- Keep separate research/canary/production identities.
- Live startup requires `mlockall`; failure to keep signer/credential memory out of swap is fatal.
- Minimize balances and allowances; revoke/rotate after incidents.
- The alpha token is a secret even if it cannot move funds.
- Do not use a flat ECDSA signature as a substitute for deposit-wallet/ERC-7739 type 3.

## TLS pinning policy

CA and hostname validation are mandatory. `BOT_TLS_PIN` can add a libcurl public-key pin, but only if operations maintains:

1. an owner and expiry/rotation calendar;
2. an independently verified replacement pin;
3. an overlap window using libcurl's semicolon-separated pin list (`sha256//old;sha256//new`) before removing the old pin;
4. a tested emergency rotation procedure and monitoring that distinguishes pin failure from venue outage.

Pinning without rotation converts certificate renewal into a trading outage. Leaving the optional pin empty does not disable normal CA/hostname validation.

## Logging and observability

Logs may contain market slug/token ID, reason codes, HTTP status, venue status, counts, and durations. They must not contain private keys, HMAC secrets, passphrases, bearer tokens, Authorization headers, credential files, or full raw authenticated requests.

Track separately:

- alpha accepted/rejected by reason;
- stale/invalid book and reconnect count;
- pool hit/miss/expired/consumed;
- risk rejections and reservations;
- gateway enqueued vs semantic accepted/rejected/ambiguous;
- DNS/connect/TLS/request/first-byte/ack timing;
- open-order/fill/inventory reconciliation lag (when implemented).

## Known security blockers

1. No authenticated private user channel or continuous account reconciliation.
2. Startup inventory relies on an operator-provided value.
3. WSS DNS resolution is blocking and not under the socket connect timeout.
4. Signature type 3 is intentionally unsupported.
5. Alpha HTTP provides bearer authentication but not native TLS; keep it on loopback or behind a mutually authenticated, rate-limited proxy. Its single bounded parser enforces a 250 ms absolute header-plus-body deadline but is not a public-edge DoS service.
6. Deterministic parser tests exist, but coverage-guided fuzzing and long soak/fault injection are not yet release gates.
