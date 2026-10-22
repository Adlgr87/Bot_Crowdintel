# AUD-OPS Report — VER-06: Métricas insuficientes

**Date:** 2026-01-22 | **Auditor:** AUD-OPS | **Commit:** c157c23

## Resumen

No hay ninguna instrumentación Prometheus en el código C++. Los 15 métricas descritas en CANARY_DEPLOYMENT.md y la checklist H1-H15 **no existen**. El mmap_state.hpp tiene contadores internos para cross-process reporting, pero NO son exportados como métricas Prometheus.

## Evidencia

```bash
$ grep -r "prometheus\|PROMETHEUS\|histogram\|counter\|gauge" core/src/
# Resultado: 0 matches — ZERO metrics instrumentation
```

### mmap_state vs Prometheus

| mmap_state field | Is Prometheus metric? | Exported? |
|-----------------|----------------------|-----------|
| pnl_cents | ❌ | No (never written) |
| daily_buy_volume_cents | ❌ | No (internal) |
| fills_total / buy / sell | ❌ | No (internal) |
| circuit_state | ❌ | No (internal) |
| kill_switch_active | ❌ | No (internal) |

### CANARY_DEPLOYMENT.md metrics (all unimplemented)

```promql
# NONE of these metrics exist in code:
histogram_quantile(0.99, rate(crowdintel_hotpath_ns_bucket[5m]))  # hotpath
rate(crowdintel_pnl_realized_usd[5m])  # PnL
crowdintel_daily_buy_volume_usd  # volume
crowdintel_shield_state  # window shield
crowdintel_cb_*  # circuit breaker
```

### Canary checklist H1-H15

| H# | Metric needed | Implemented? |
|----|--------------|-------------|
| H2 | hotpath_ns histogram | ❌ (CPU-only in benchmark) |
| H3 | calibration ECE | ❌ |
| H4 | circuit breaker trips | ❌ |
| H5/H6 | shield + kill switch | ⚠️ (in mmap_state, not exported) |
| H7 | PnL realized | ❌ |
| H8 | fill count | ⚠️ (in mmap_state, not exported) |
| H9 | rejection count | ⚠️ (in mmap_state, not exported) |
| H10 | slippage vs mid | ❌ |
| H11 | daily buy volume | ⚠️ (in execution_engine, not exported) |
| H13 | exposure vs cap | ❌ |
| H14 | feed staleness | ❌ |
| H15 | spike detector | ❌ |

**Summary: 0/15 Prometheus metrics implemented. 5/15 have internal counters in mmap_state.**

## Conclusión

The monitoring infrastructure does not exist. CANARY_DEPLOYMENT.md describes metrics but the code doesn't instrument any of them. No Prometheus, no alert rules, no runbooks, no SLO definitions.

## Recomendación

1. Añadir Prometheus instrumentation (histograms, gauges, counters)
2. Export mmap_state counters to Prometheus
3. Create alert rules YAML with thresholds
4. Create runbooks for each alert
5. Define SLOs for hot-path latency, PnL, circuit state
