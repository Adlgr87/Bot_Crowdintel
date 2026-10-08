# Monitoring Dashboard Configuration

## Bot Metrics Endpoint
- **Port:** 9090 (`BOT_METRICS_PORT=9090` in `infra/deploy/canary.env`)
- **Endpoint:** `/metrics` (`BOT_METRICS_ENDPOINT=/metrics`)
- **Format:** Prometheus exposition format
- **Scraping:** Prometheus scrapes every 5s in canary mode

## Grafana Dashboard

### Production Dashboard
- **URL:** https://grafana.crowdintel.io/d/dsh_canary/dsh-canary-dashboard
- **Dashboard ID:** `dsh_canary`
- **Variables:**
  - `$bot_mode` — `mock` | `live`
  - `$canary_stage` — `S0` | `S1` | `S2` | `S3` | `S4`
  - `$time_window` — 5m | 15m | 1h | 24h

### Key Panels

| Panel | Metric | Visualization | Alert Threshold |
|-------|--------|--------------|-----------------|
| Pipeline Latency | `crowdintel_latency_p99_us` | Time series (p50/p99) | >100μs (warning) |
| P&L | `crowdintel_daily_pnl_pct` | Time series | <-5% (critical) |
| Win Rate | `crowdintel_winrate_24h` | Gauge (0-100%) | <50% (warning) |
| BB Calibration | `crowdintel_bb_ece` | Single stat | >0.05 (warning) |
| Edge | `crowdintel_min_edge` | Bar gauge | <0.5% (warning) |
| Max Drawdown | `crowdintel_max_drawdown` | Single stat | >3% (warning) |
| CircuitBreaker State | `crowdintel_cb_state` | State timeline | OPEN (critical) |
| OFI Pressure | `crowdintel_ofi_extreme_total` | Counter + heatmap | any (info) |
| Memory RSS | `crowdintel_rss_mb` | Time series | >100MB (critical) |
| Rate Limited | `crowdintel_429_total` | Counter + rate | >5/min (warning) |
| Windows Completed | `crowdintel_windows_completed_total` | Counter | logging (info) |

## AlertManager Routing

```yaml
route:
  group_by: ['alertname']
  group_wait: 10s
  group_interval: 1m
  repeat_interval: 5m
  routes:
    - match:
        severity: critical
      receiver: 'pagerduty'
    - match:
        severity: warning
      receiver: '#trading-alerts'
    - match:
        severity: info
      receiver: '#trading-logs'
```

## Local Development Metrics

For local testing, metrics can be inspected via:
```bash
# Run the bot in mock mode with metrics
BOT_MODE=mock ./build/bin/crowdintel_bot --metrics-port=9090

# Query metrics endpoint
curl http://127.0.0.1:9090/metrics

# Run daily report against mock journal
python3 scripts/daily_report.py --journal /tmp/mock_journal.ndjson --output /tmp/daily_report.json
```

## Alert Threshold Reference

| Metric | Warning | Critical | Action |
|--------|---------|----------|--------|
| `latency_p99_us` | >100μs | >1000μs | Investigate hot path |
| `daily_pnl_pct` | <-3% | <-5% | Rollback canary |
| `winrate_24h` | <50% | <40% | Halt trading |
| `bb_ece` | >0.05 | >0.10 | Recalibrate model |
| `min_edge` | <0.5% | <0.3% | Check fee model |
| `max_drawdown` | >3% | >5% | Reduce position |
| `cb_open_seconds` | >30s | >60s | Emergency halt |
| `cb_open_total` | >3/hr | >5/hr | Investigate |
| `rss_mb` | >100MB | >200MB | Restart bot |
| `429_rate` | >5/min | >20/min | Reduce request rate |
