# Threat Model Draft — OBS-09: TLS/Proxy/External

## Threat Model (preliminary draft for ADR)

### Assets

| Asset | Location | Sensitivity |
|-------|----------|-------------|
| Private key (hex) | env var `BOT_PRIVATE_KEY_HEX` → MarketConfig | CRITICAL |
| API secret (b64) | env var `BOT_POLYMARKET_API_SECRET` → MarketConfig | CRITICAL |
| API passphrase | env var `BOT_POLYMARKET_API_PASSPHRASE` | CRITICAL |
| Alpha bearer token | MarketConfig.alpha_bearer_token | HIGH |
| Session keys | runtime memory | CRITICAL |
| Market data (L2 feed) | in-memory OrderBookL2 | MEDIUM |
| Order signatures | transit (HTTPS) | HIGH |

### Trust boundaries

1. **Internet → CLOB API** (HTTPS): `https://clob.polymarket.com/order`
   - TLS 1.3 via libcurl ✅
   - HMAC-SHA256 auth ✅
   - Secret stored in env, zeroed on destruct ✅

2. **Internet → WebSocket** (WSS): `wss://ws-subscriptions-clob.polymarket.com/ws/market`
   - WSS (WebSocket Secure) ✅
   - No auth needed for market feed ✅

3. **Internet → Alpha HTTP Receiver** (loopback): `127.0.0.1:8088`
   - **NO TLS** ❌
   - Bearer token in plaintext across loopback ❌

4. **Alpha producer → Loopback**: Internal network
   - If same host: memory-local, but process boundary means TCP/loopback
   - If different host: traffic crosses network in plaintext ❌

### Threat vectors

| # | Threat | Vector | Impact | Severity | Status |
|---|--------|--------|--------|----------|--------|
| T1 | MITM on alpha receiver (loopback) | ARP spoof / local attacker | Bearer token theft → order injection | CRITICAL | ❌ Unmitigated |
| T2 | MITM on CLOB API | TLS cert spoofing | Order manipulation | HIGH | ✅ TLS verified |
| T3 | Proxy external to alpha receiver | Proxy inspects traffic | Token + market data exposure | HIGH | ❌ No mTLS |
| T4 | Credential leakage via logs | stderr/fprintf on errors | API key exposure | HIGH | ⚠️ secure_zero on destruct, but error logs |
| T5 | Clock backward jump | NTP / adjtime | Rate limit bypass | HIGH | ⚠️ Documented in SEC audit, pending fix |
| T6 | Replay attack on orders | Intercept + resend signed order | Duplicate execution | MEDIUM | ✅ EIP-712 timestamp + salt prevents within TTL |
| T7 | Fill event loss | WebSocket disconnect during fill | Position mismatch | MEDIUM | ✅ Reconciliation catches drift |
| T8 | Signature type 3 (ERC-7739) | Depositor wallet signature | Fail-closed | LOW | ✅ Fails closed, but not implemented |

### Loopback analysis

**Archivo:** `core/src/market_config.hpp` líneas 95-97:
```cpp
char alpha_bind[64] = "127.0.0.1";  // Loopback
uint16_t alpha_port = 8088;
char alpha_bearer_token[128]{};
```

**Attack scenario:**
1. Attacker has local access to the host (or same LAN)
2. Alpha HTTP receiver listens on 127.0.0.1:8088 without TLS
3. Attacker intercepts the Bearer token via:
   - tcpdump on loopback (on Linux: `tcpdump -i lo`)
   - strace on the alpha receiver process
   - Memory dump of the process
4. Attacker sends forged alpha signals → executes trades

**Mitigation needed:**
- TLS on alpha receiver (optional cert)
- OR Unix domain socket with file permissions
- OR mTLS if routed through external proxy

### Recommendations

1. **ADR-0003**: Document threat model
2. **TLS option**: If `BOT_ALPHA_TLS` env is set, use HTTPS loopback
3. **mTLS**: If proxy is external, require mTLS authentication
4. **Secret rotation**: Document in runbook (environment variable rotation)
5. **Log sanitization**: Ensure error paths don't log secrets
