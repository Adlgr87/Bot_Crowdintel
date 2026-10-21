# VERIFICATION — OBS-06: Latencia micro vs latencia real

**Verificador:** AUD-NET
**Fecha:** 2026-01-22
**Commit base:** c157c23

---

## Verdict: VERIFIED_FAIL — Solo microbenchmark CPU, no E2E measurement

### Finding

The observation **CONFIRMS** that the only latency measurement is a CPU-path microbenchmark that **explicitly excludes network/I/O**. There is no end-to-end latency measurement from `on_book_update` to HTTP 200/201 from the CLOB.

### Evidence

**Archivo:** `tests/benchmarks/latency_bench.cpp`, línea 1-2:

```cpp
// CPU-path microbenchmark.  It deliberately excludes network/HMAC/TLS and
// labels results accordingly.  Presigned slots are consumable, so the fixture
// rebuilds a fresh eight-size BUY ladder outside each measured batch.
```

**Measured:** `run_tick()` bare hot-path ~754ns (CPU only, mock client)
**NOT measured:** End-to-end from on_book_update → HTTP 200/201 del CLOB real

**LATENCY_BUDGET.md línea 4-5:**
```
| Budget | < 15μs | < 50μs |
| Current (bare) | ~754ns | ~1.2μs |
| With P1–P4 | ~754ns | ~2μs |
```

→ El "current (bare)" de 754ns es **CPU-only sin red**. El LATENCY BUDDGET no incluye:
- DNS lookup
- TCP handshake
- TLS handshake
- HTTP POST time
- CLOB server processing time

**README.md línea 27:**
> "This separation is an optimization boundary, not evidence that an order reaches the venue in sub-microsecond time."

✅ El README reconoce esto, PERO el LATENCY_BUDGET.md no refleja la latencia E2E real.

### ¿Hay instrumentación de latencia E2E?

```
$ grep -r "e2e\|end.to.end\|latency_e2e\|http_code.*time\|CLOB.*lat" core/ tests/ docs/
# Resultado: 0 matches for E2E measurement
```

El código mide RDTSC para hot path (line 18 in ARCHITECTURE_MAP: `ExecutionEngine::run_tick() ← RDTSC instrumented`), pero:
- Solo el hot path CPU
- No hay timestamp de "order submitted to HTTP response received"
- No hay latencia de red documentada

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| p99 end-to-end < SLO | ❌ | No se mide E2E |
| Hot-path < 12μs reportado como métrica secundaria | ⚠️ | 754ns medido, pero no hay E2E |
| LATENCY_BUDGET.md actualizado con E2E | ❌ | Solo CPU path |
| SLO definido para latencia E2E | ❌ | SLO es CPU-only (50μs p99) |

## Recomendación para IMP

1. **Instrumentar E2E latency**: Timestamp en `client_.submit()` → HTTP response received
2. **Test de E2E**: Medir con mock server que simule latency de red
3. **Actualizar LATENCY_BUDGET.md**: Separar hot-path CPU (SLO primario) de E2E (SLO secundario)
4. **Artefacto**: `artifacts/latency_e2e.json` con p50/p99/p99.9 E2E

## Artefactos

- `agents/handoffs/obs-06-latency-micro-vs-real/artifacts/latency_gap.md`
- `reports/net/obs-06-latency-gap.md`

## Signoff