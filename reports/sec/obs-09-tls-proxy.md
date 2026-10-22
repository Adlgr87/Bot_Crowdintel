# AUD-SEC Report — OBS-09: TLS/Proxy externo y loopback

**Date:** 2026-01-22 | **Auditor:** AUD-SEC + AUD-NET | **Commit:** c157c23

## Resumen

- ✅ CLOB API (HTTPS): TLS verificado via libcurl
- ✅ WebSocket (WSS): wss:// configurado por default
- ❌ Alpha HTTP receiver: 127.0.0.1:8088 sin TLS
- ❌ Threat model no documentado
- ❌ No mTLS para proxy externo
- ⚠️ Log sanitization: error paths usan fprintf(stderr) (FIX T1.7 aplicado en algunos paths)

## Evidencia

### CLOB API TLS ✅

```cpp
// lightweight_client.hpp línea 90:
const CURLcode code = curl_easy_perform(curl_);
// libcurl verifica peer por defecto (CURLOPT_SSL_VERIFYPEER = true)
```

### WebSocket WSS ✅

```cpp
// market_config.hpp líneas 44, 46:
char ws_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/market";
char ws_user_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/user";
```

### Alpha HTTP receiver TLS ❌

```cpp
// market_config.hpp líneas 95-97:
char alpha_bind[64] = "127.0.0.1";  // Loopback, NO TLS
uint16_t alpha_port = 8088;
char alpha_bearer_token[128]{};  // Plaintext en loopback
```

### Threat model ❌

```
$ find . -name "THREAT_MODEL*" -o -name "threat_model*"
# Resultado: 0 matches — No threat model documentado
```

### SecurityAudit — cobertura existente

`SECURITY_AUDIT.md` (374 líneas) — audit NEMESIS phase 1, 4 findings:
- Rate limiter overflow (Medium)
- WS reconnection backoff (Medium)
- Retry-After bounds (Low)
- Log callback exposure (Low, no details)

**NO threat modeling, NO network analysis beyond what's above.**

## Threat vectors

| # | Threat | Vector | Impact | Severity | Mitigated? |
|---|--------|--------|--------|----------|------------|
| T1 | MITM on alpha receiver | Loopback ARP spoof | Bearer token theft | CRITICAL | ❌ |
| T2 | MITM on CLOB API | TLS cert spoofing | Order manipulation | HIGH | ✅ |
| T3 | Proxy external | External proxy, plaintext | Token exposure | HIGH | ❌ |
| T4 | Cred leakage via logs | stderr/fprintf | API key exposure | HIGH | ⚠️ Partial |
| T5 | Clock backward | NTP attack | Rate limit bypass | HIGH | ⚠️ Documented |
| T6 | Replay attacks | Intercept + resend | Duplicate orders | MEDIUM | ✅ (EIP-712 timestamp) |

## Conclusión

| Criterio | Cumplido | Evidencia |
|----------|---------|-----------|
| Threat model documentado | ❌ | No existe docs/THREAT_MODEL.md |
| TLS end-to-end CLOB API | ✅ | libcurl SSL verif |
| TLS/mTLS alpha receiver loopback | ❌ | 127.0.0.1 sin TLS |
| Proxy con mTLS | ❌ | No documentado |
| Rotación de secretos | ❌ | No rotation mechanism |
| Logs sin secretos | ⚠️ | secure_zero en dtor, fprintf en algunos paths |

## Recomendación

1. ADR-0003: Threat model document
2. TLS opcional para alpha receiver (`BOT_ALPHA_TLS`)
3. mTLS para proxy externo
4. Secret rotation en runbook
5. Log sanitization (ensure no secret logging in error paths)
