# ADR-0003: Network Threat Model — TLS & External Endpoints

| Field | Value |
|-------|-------|
| **Status** | Proposed |
| **Date** | 2026-01-22 |
| **Author** | AUD-SEC |
| **Deciders** | AXIOM, AUD-NET, AUD-OPS |
| **Related cards** | OBS-09, VER-08 |

## Context

The alpha HTTP receiver listens on `127.0.0.1:8088` **without TLS**. The Bearer token travels in plaintext across loopback. No threat model document exists. No mTLS for external proxies.

## Decision

Add optional TLS for the alpha receiver and document a formal threat model.

### TLS configuration

```cpp
// market_config.hpp — new fields:
char alpha_tls_cert[256] = "";     // path to cert.pem (empty = plaintext)
char alpha_tls_key[256] = "";      // path to key.pem
bool alpha_require_mtls = false;   // if proxy is external
```

### Trust boundaries

| # | Boundary | Protocol | TLS? | Auth |
|---|----------|----------|------|------|
| 1 | Internet → CLOB API | HTTPS | ✅ | HMAC-SHA256 |
| 2 | Internet → CLOB WS | WSS | ✅ | None (market feed) |
| 3 | Alpha producer → Loopback | HTTP | ❌ | Bearer token (plaintext) |
| 4 | Loopback → Bot | HTTP | ❌ | Bearer token (plaintext) |

### Threat model (STRIDE)

| Threat | STRIDE | Mitigation |
|--------|--------|------------|
| MITM on loopback | Spoofing | TLS loopback (optional `BOT_ALPHA_TLS`) |
| Token exposure | Information disclosure | Bearer token in env, NOT in process args |
| Replay | Repudiation | EIP-712 timestamp + salt |
| Clock skew | Tampering | `clock_skew_guard` in rate_limiter |

## Consequences

### Positive
- Documented threat model
- TLS available for alpha receiver
- mTLS requirement documented for external proxies

### Negative
- New config complexity
- Need to test TLS loopback path

## Action items

1. Implement optional TLS in alpha_http_receiver.hpp
2. Create docs/THREAT_MODEL.md
3. Add TLS test cases
4. Document proxy mTLS requirement in runbook
