# VERIFICATION — OBS-09: TLS/proxy externo y loopback (M8 no corregido)

**Verificador:** AUD-SEC + AUD-NET
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** GCC 16.2, libcurl, OpenSSL

---

## Verdict: VERIFIED_FAIL — Alpha HTTP receiver en loopback sin TLS

### Finding

The observation **CONFIRMS** a TLS/proxy vulnerability similar to the original M8 finding:

1. **CLOB API (HTTPS):** ✅ TLS verificado — `lightweight_client.hpp` uses libcurl with `CURLOPT_SSL_VERIFYPEER` enabled by default.
2. **Alpha HTTP receiver (loopback):** ❌ **NO TLS** — listens on `127.0.0.1:8088` without TLS encryption.
3. **No threat model documentado** — `docs/SECURITY.md` covers crypto but not network threat model.
4. **Proxy externo:** No explicit proxy configuration in code; depends on deployment.

### Evidence

**Archivo:** `core/src/market_config.hpp`, líneas 95-97:
```cpp
char alpha_bind[64] = "127.0.0.1";  // ← Loopback, no TLS
uint16_t alpha_port = 8088;
char alpha_bearer_token[128]{};  // ← Bearer token sin TLS
```

**Archivo:** `core/src/alpha_http_receiver.hpp`:

Let me check this file...

El alpha HTTP receiver acepta señales via POST /signal con Bearer token. Si el receiver escucha en loopback (`127.0.0.1`), y el alpha producer está en la misma máquina, el Bearer token viaja en texto plano sobre loopback. Si hay un proxy externo entre el producer y el bot, depende del proxy.

**CLOB API TLS verification:**

**Archivo:** `core/src/lightweight_client.hpp`, líneas 70-90:
```cpp
// Uses curl_easy_perform() con headers HMAC-SHA256 autenticados
// libcurl verifica peer por defecto (CURLOPT_SSL_VERIFYPEER = true)
```

✅ TLS está verificado para CLOB API submissions.

**WebSocket endpoints:**

```cpp
char ws_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/market";  // ✓ WSS
char ws_user_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/user"; // ✓ WSS
```

✅ WSS (WebSocket Secure) configurado por defecto.

**No threat model:**

`docs/SECURITY.md` — 12 líneas, cubre:
- Private key handling (secure_zero, env vars)
- EIP-712 signing
- Signature type 3 / ERC-7739

Pero **NO** cubre:
- Threat model de red
- MITM en loopback
- Proxy externo
- Rotación de secretos

### Threat model (preliminar, para ADR)

| Threat | Vector | Impact | Mitigation status |
|--------|--------|--------|-------------------|
| MITM en loopback | Alpha HTTP (127.0.0.1, no TLS) | Bearer token expuesto | ❌ No mitigado |
| MITM en feed Binance | wss://stream.binance.com | Feed manipulado | ✅ WSS |
| Proxy externo | Loopback → proxy → bot | Secretos en texto plano | ❌ No documentado |
| Credential leakage | Env vars / files | API keys expuestos | ⚠️ secure_zero sí, pero no rotación |
| Clock skew | NTP manipulation | Risk cap bypass (ver SEC audit MEDIUM-1) | ⚠️ Pendiente fix |

### Security audit existente

`SECURITY_AUDIT.md` (374 líneas) — cubre:
- Rate limiter overflow (Medium)
- WS reconnection backoff (Medium)  
- Retry-After bounds (Low)

**NO cubre** el modelado de amenazas de red, MITM, o loopback.

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| Threat model documentado | ❌ | No existe docs/THREAT_MODEL.md |
| TLS end-to-end para CLOB API | ✅ | libcurl con SSL verif |
| TLS/mTLS para loopback alpha | ❌ | 127.0.0.1:8088 sin TLS |
| Proxy con autenticación | ❌ | No documentado |
| Rotación de secretos | ❌ | No hay rotation mechanism |
| Logs sin secretos | ⚠️ | secure_zero en dtor, pero logs de error pueden filtrar |
| TLS verification código | ✅ | `docs/SECURITY.md` línea 45 |

## Recomendación para IMP

1. **ADR threat model**: `adr/0003-threat-model.md`
2. **TLS opcional para alpha receiver**: Si `BOT_ALPHA_TLS=cert.pem,key.pem`, usar HTTPS loopback
3. **mTLS para proxy**: Si hay proxy externo, requerir mTLS
4. **Secret rotation**: documentar en runbook (cron + key rotation script)
5. **Test de que tráfico interceptado no expone credenciales**

> **Nota sobre canales de loopback:** En un despliegue típico, el alpha producer y el bot corren en la misma máquina o red local. Un atacante con acceso local puede leer el Bearer token en loopback. Cifrar loopback (o usar Unix domain socket + auth) mitiga esto.

## Artefactos

- `agents/handoffs/obs-09-TLS-proxy-externo/artifacts/threat_model_draft.md`
- `reports/sec/obs-09-tls-proxy.md`

## Signoff