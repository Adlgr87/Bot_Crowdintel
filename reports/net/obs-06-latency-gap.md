# AUD-NET Report — OBS-06: Latencia micro vs real

**Date:** 2026-01-22 | **Auditor:** AUD-NET | **Commit:** c157c23

## Resumen

El microbenchmark `latency_bench.cpp` mide SOLO el hot path CPU (~754ns p50), excluyendo explícitamente red, TLS, HMAC y socket I/O. No hay medición de latencia end-to-end (E2E) desde `on_book_update` hasta HTTP 200/201 del CLOB.

## Evidencia

### latency_bench.cpp línea 1-2:

```cpp
// CPU-path microbenchmark.  It deliberately excludes network/HMAC/TLS and
// labels results accordingly.
```

### Métricas actuales

| Métrica | p50 | p99 | Scope |
|---------|-----|-----|-------|
| run_tick() bare | ~754ns | ~1.2μs | CPU only (mock client) |
| Full layered (P1-P4) | ~754ns | ~2μs | CPU only |

### Lo que NO se mide

- DNS lookup, TCP handshake, TLS handshake
- HTTP POST to CLOB (`/order` endpoint)
- CLOB server processing time
- HTTP 200/201 response time

### LATENCY_BUDGET.md

El presupuesto define:
- p50 < 15μs, p99 < 50μs
- "Current (bare): ~754ns p50, ~1.2μs p99"

**El "bare" de 754ns es CPU-only.** El LATENCY_BUDGET.md no incluye la latencia E2E.

### README.md línea 27:

> "This separation is an optimization boundary, not evidence that an order reaches the venue in sub-microsecond time."

✅ El README reconoce esto, pero LATENCY_BUDGET.md no lo refleja.

## E2E latency estimate

| Component | Est. latency | Included in benchmark? |
|-----------|-------------|----------------------|
| WebSocket → kernel buffer | 0.01-5ms | ❌ |
| JSON parse L2 | 1-5μs | ❌ |
| SPSC ring push | 50ns | ⚠️ (parte del hot path) |
| run_tick() (DSH) | 2μs | ✅ |
| Order construction | 1-2μs | ✅ |
| EIP-712 sign | 100ns | ✅ |
| libcurl POST (TCP+TLS+HTTP) | 1-50ms | ❌ |
| CLOB server processing | 5-50ms | ❌ |
| HTTP response | 1-10ms | ❌ |
| **Total E2E** | **~7-114ms** | ❌ |

## Conclusión

- ✅ Hot path CPU está optimizado (754ns)
- ❌ No hay E2E latency measurement
- ❌ LATENCY_BUDGET.md no incluye latencia de red/venue
- ❌ El <12μs del hot path no es representative de E2E canary performance

## Recomendación

1. Instrumentar E2E latency: timestamp en `client_.submit()` → HTTP response received
2. Test E2E con mock server simulando network latency
3. Actualizar LATENCY_BUDGET.md: separar hot-path CPU (SLO primario) de E2E (SLO secundario)
4. Artefacto: `artifacts/latency_e2e.json`
