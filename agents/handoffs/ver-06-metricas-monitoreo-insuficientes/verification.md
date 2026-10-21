# VERIFICATION — VER-06: Métricas de monitoreo insuficientes

**Verificador:** AUD-OPS
**Fecha:** 2026-01-22
**Commit base:** c157c23

---

## Verdict: VERIFIED_FAIL — No Prometheus metrics, no alertas, no SLOs in code

### Finding

The observation **CONFIRMS** that there are **zero monitoring metrics** instrumented in the C++ code. `CANARY_DEPLOYMENT.md` describes Prometheus metrics but none exist in the actual codebase.

### Evidence

**Buscar instrumentación de métricas:**

```
$ grep -r "prometheus\|metric\|counter\|gauge\|histogram\|PROMETHEUS" core/src/
# Resultado: 0 matches
```

```
$ grep -r "alert\|SLI\|SLO\|runbook" core/src/
# Resultado: 0 matches
```

**CANARY_DEPLOYMENT.md líneas 33-49** describe métricas Prometheus que **NO existen en código**:

```promql
histogram_quantile(0.50, rate(crowdintel_hotpath_ns_bucket[5m]))  # p50
histogram_quantile(0.99, rate(crowdintel_hotpath_ns_bucket[5m]))  # p99
rate(crowdintel_cfc_inference_count[5m])
rate(crowdintel_cfc_nan_guard_triggered[5m])
rate(crowdintel_cfc_model_drift[5m])
crowdintel_shield_state
rate(crowdintel_pnl_realized_usd[5m])
crowdintel_daily_buy_volume_usd
```

→ Ninguna de estas métricas está implementada en el código C++.

**MONITORING_DASHBOARD.md:**

```
$ cat docs/MONITORING_DASHBOARD.md | wc -l
# Resultado: 89 lines — describe dashboards JSON pero no hay código de instrumentación
```

**¿Qué métricas debería existir (según canary H1-H15)?**

| Metric | Canary ref | Implemented? |
|--------|-----------|--------------|
| hotpath latency p50/p99 | H2, H4 | ❌ |
| Calibration ECE / reliability | H3 | ❌ |
| Model drift / inference count | H3 | ❌ |
| NaN guard triggered | H3 | ❌ |
| Shield state transitions | H5, H6 | ❌ |
| PnL realized/unrealized | H7, H13 | ❌ |
| Daily buy volume | H11 | ⚠️ (mmap_state has daily_buy_volume_cents) |
| Fill count buy/sell | H8 | ⚠️ (mmap_state has fills_buy/sell_count) |
| Circuit breaker trips | H9 | ❌ |
| Rejection count | H10 | ⚠️ (mmap_state has rejection_count) |
| Kill switch active | H12 | ⚠️ (mmap_state has kill_switch_active) |
| Feed staleness | H14 | ❌ |
| Spike detector triggers | H15 | ❌ |

**mmap_state.hpp** HAS algunos contadores (líneas 28-49):
```cpp
std::atomic<int64_t> pnl_cents;
std::atomic<int64_t> daily_buy_volume_cents;
std::atomic<int64_t> daily_sell_volume_cents;
std::atomic<int64_t> position_shares;
std::atomic<int64_t> fills_total;
std::atomic<int64_t> fills_buy_count;
std::atomic<int64_t> fills_sell_count;
std::atomic<uint32_t> circuit_state;
std::atomic<uint32_t> window_state;
std::atomic<int32_t> rejection_count;
std::atomic<uint8_t> kill_switch_active;
```

Pero estos son **estados compartidos vía mmap**, no métricas Prometheus con histogramas/percentiles. No hay:
- Latencia p50/p99 (histogramas)
- Calibration ECE
- Alertas definidas
- Runbooks

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| Cada métrica H1-H15 tiene alerta | ❌ | Solo 7/15 métricas en mmap_state, ninguna como Prometheus |
| Runbooks | ❌ | No existen runbooks |
| Dashboards versionados | ⚠️ | MONITORING_DASHBOARD.md existe pero no hay código |
| SLOs definidos | ❌ | Ningún SLO en código/config |

## Recomendación para IMP

1. **Instrumentar Prometheus metrics** via `mmap_state.hpp` shared state + external exporter
2. **Latencia histogram**: p50/p99/p99.9 del hot path (usar histogramas)
3. **Alertas**: YAML con umbrales y notificaciones
4. **Runbooks**: markdown con procedimientos de cada alerta

## Artefactos

- `agents/handoffs/ver-06-metricas-monitoreo-insuficientes/artifacts/metrics_gap.md`
- `reports/ops/ver-06-monitoring-gap.md`

## Signoff