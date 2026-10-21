# Monitoring Metrics Gap Analysis — VER-06

## Current state: Zero Prometheus instrumentation

```bash
$ grep -r "prometheus\|PROMETHEUS\|metric\|histogram\|counter\|gauge" core/src/
# Resultado: 0 matches — NO metrics instrumentation
```

## mmap_state.hpp — existing shared state (NOT Prometheus metrics)

Has atomic counters for cross-process reporting:

| Field | Type | Purpose | Is Prometheus metric? |
|-------|------|---------|----------------------|
| pnl_cents | atomic<int64_t> | PnL tracking | ❌ (defined, never written) |
| daily_buy_volume_cents | atomic<int64_t> | Daily volume | ❌ (internal state, not exported) |
| fills_total | atomic<int64_t> | Fill count | ❌ |
| fills_buy_count | atomic<int64_t> | Buy fills | ❌ |
| fills_sell_count | atomic<int64_t> | Sell fills | ❌ |
| circuit_state | atomic<uint32_t> | CB state | ❌ |
| window_state | atomic<uint32_t> | Shield state | ❌ |
| rejection_count | atomic<int32_t> | Rejections | ❌ |
| kill_switch_active | atomic<uint8_t> | Kill state | ❌ |

## CANARY_DEPLOYMENT.md metrics references (all unimplemented)

```promql
# Hot-path latency — NOT implemented
histogram_quantile(0.50, rate(crowdintel_hotpath_ns_bucket[5m]))
histogram_quantile(0.99, rate(crowdintel_hotpath_ns_bucket[5m]))

# CfC model health — NOT implemented (DSH replaced CfC)
rate(crowdintel_cfc_inference_count[5m])
rate(crowdintel_cfc_nan_guard_triggered[5m])
rate(crowdintel_cfc_model_drift[5m])

# WindowShield state — NOT implemented
crowdintel_shield_state

# Strategy PnL — NOT implemented (no PnL tracking)
rate(crowdintel_pnl_realized_usd[5m])
crowdintel_daily_buy_volume_usd
```

## Canary checklist H1-H15 audit

| H# | Checklist item | Metric needed | Implemented? |
|----|----------------|---------------|--------------|
| H1 | Signal identity validation | signal_id counter | ❌ |
| H2 | Hot-path latency p99 < 50μs | hotpath_ns histogram | ⚠️ (CPU-only in benchmark) |
| H3 | Model calibration ECE < 2% | ece_gauge, calibration_count | ❌ |
| H4 | Circuit breaker trips | cb_trips counter | ❌ |
| H5 | WindowShield transitions | shield_state gauge | ⚠️ (in mmap_state, not exported) |
| H6 | Kill switch status | kill_switch gauge | ⚠️ (in mmap_state, not exported) |
| H7 | Realized PnL tracking | pnl_realized gauge | ❌ |
| H8 | Fill count + breakdown | fills_total/buy/sell counter | ⚠️ (in mmap_state, not exported) |
| H9 | Rejection reasons | rejection_count counter | ⚠️ (in mmap_state, not exported) |
| H10 | Slippage vs mid | slippage_bps gauge | ❌ |
| H11 | Daily buy volume cap | daily_buy_volume counter | ⚠️ (in execution_engine, not exported) |
| H12 | Alpha signal age | signal_age gauge | ❌ |
| H13 | Exposure vs cap | exposure_usd gauge | ❌ |
| H14 | Feed staleness | feed_stale gauge | ❌ |
| H15 | Spike detector | spike_count counter | ❌ |

**Summary: Only 5/15 canary metrics have ANY implementation in mmap_state (not exported as Prometheus). 0/15 have Prometheus instrumentation.**

## Required Prometheus instrumentation

```cpp
// Example metric additions needed:

// Histogram: hot-path latency
prometheus::Histogram hotpath_latency = prometheus::Histogram(
    "crowdintel_hotpath_ns",
    prometheus::Histogram::Bucket{100, 500, 1000, 2000, 5000, 10000, 20000, 50000}
);

// Gauge: ECE (calibration)
prometheus::Gauge calibration_ece = prometheus::Gauge("crowdintel_calibration_ece");

// Counter: circuit breaker trips
prometheus::Counter cb_trips = prometheus::Counter("crowdintel_cb_trips_total");

// etc.
```
