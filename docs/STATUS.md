# 📊 Project Status — Bot CrowdIntel

_Last updated: 2026-09-22 (CLOB V2 remediation). For the full defect list that
was fixed, see [REMEDIATION_2026-09-22.md](REMEDIATION_2026-09-22.md)._

## What is verified (automated, in CI)

| Area | Verification | Result |
| :--- | :--- | :--- |
| Keccak-256 | KATs (Ethereum vectors + multi-block) | ✅ PASS |
| SHA-256 / HMAC (in-house, hot path) | FIPS 180-2 + RFC 4231 vectors | ✅ PASS |
| EIP-712 V2 domain / struct hash / digest | golden vector vs Python reference | ✅ PASS |
| ECDSA (libsecp256k1, RFC 6979) | signature byte-identical to coincurve; 20/20 recover to signer | ✅ PASS |
| Wire body (POST /order JSON) | byte-for-byte vs Python reference | ✅ PASS |
| L2 auth headers + HMAC | scheme per docs (POLY_*, base64url) | ✅ implemented + HMAC KAT |
| SPSC ring buffer | unit + 200k-item 2-thread FIFO test | ✅ PASS |
| OrderBookL2 seqlock | concurrency torture (writer+reader) | ✅ PASS |
| Kelly sizing | exact binary Kelly known answers | ✅ PASS |
| WSS market parser | real doc messages (snapshot + deltas + legacy pairs) | ✅ PASS |
| Hot-path latency | calibrated RDTSC, 20k samples | ✅ 90 ns P50 (pool) / 24 µs (inline) |

## What is NOT yet verified

- **A live order against production clob.polymarket.com.** The sandbox/CI
  environment signs correctly (cross-checked) but holds no funded wallet.
  First live deployment should start with a paper-size GTC on a cheap market.
- **WSS connection to the production feed.** The client is hand-rolled
  (RFC 6455 + TLS) and its parser is unit-tested against official message
  shapes, but a long-run soak against `wss://ws-subscriptions-clob.polymarket.com`
  has not been executed from CI. Run `BOT_MODE=live` with `BOT_TICKS=0` on a
  VPS and watch the `ws:` counters in the shutdown summary.

## Open items before real-money deployment

1. **Operator security review**: key management (the bot reads
   `BOT_PRIVATE_KEY_HEX` from the environment — use a vault/secret manager and
   consider a dedicated hot wallet with capped balance).
2. **Per-market tick size**: `BOT_TICK_SIZE` is static; fetch the market's
   minimum tick via `getClobMarketInfo` at startup for multi-market setups.
3. **Balance/allowance checks** before sizing (the engine clamps to visible
   liquidity but does not query the wallet balance).
4. **Cert pinning** supported (`BOT_TLS_PIN`, `sha256//...`) — enable in prod.
5. **User-channel (fills) feed**: order lifecycle tracking is not wired into
   the engine yet; fills are visible via the CLOB data API/WS user channel.
