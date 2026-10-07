# Canary Deployment — Bot_Crowdintel BTC 5m/15m

## Pre-requisites (blocking)

1. **Mainnet simulation passed**: 24h paper trading on `test.crowdintel.io`
   - Required: 500+ trades executed, 0 slippage violations
   - Required: PnL track error < 0.1% vs. Polymarket API settlement
2. **Latency budget verified**: p99 hot-path ≤ 45μs (10μs headroom)
3. **Model accuracy**: CfC calibration ECE < 2% on 7-day backtest
4. **Circuit breakers**: WindowShield, SpikeDetector, RiskManager all exercised

## Canary Stages

| Stage | Duration | Markets | Order Size | Volume Cap | Criteria |
|-------|----------|---------|------------|------------|----------|
| S1 | 2h | BTC 5m only | 1 share | 50 shares/day | Zero execution errors, <0.5% slippage vs. spot VWAP |
| S2 | 8h | BTC 5m + 15m | 5 shares | 200 shares/day | WindowShield transitions logged, 0 halted trades missed |
| S3 | 24h | BTC 5m + 15m, all pairs | 25 shares | 500 shares/day | No kill-switch activation, <0.5% latency regression |
| S4 | 48h | All enabled markets | Full size | No cap | Gradual ramp: 25% → 50% → 100% position sizing |

## Rollback Triggers (automatic, sub-5s)

| Metric | Threshold | Action |
|--------|-----------|--------|
| Hot-path p99 > 50μs | 3 consecutive windows | Throttle to legacy mode (kill ExtendedEngineLayers) |
| WindowShield HALTED | 5 consecutive windows | Auto-disable CfC, fall back to BayesianEngine |
| SpikeDetector | price deviation > 0.5% in < 1s | Freeze trading for 10s (anti-sniping circuit) |
| Daily loss | -0.5% NAV | Hard kill-switch (requires manual reset) |
| Gas spike | Polygon gas > 1500 gwei | Pause submission (resume at <800 gwei) |

## Monitoring (Prometheus labels)

```promql
# Hot-path latency
histogram_quantile(0.50, rate(crowdintel_hotpath_ns_bucket[5m]))  # p50
histogram_quantile(0.99, rate(crowdintel_hotpath_ns_bucket[5m]))  # p99

# CfC model health
rate(crowdintel_cfc_inference_count[5m])
rate(crowdintel_cfc_nan_guard_triggered[5m]) > 0  # alert
rate(crowdintel_cfc_model_drift[5m])              # calibration drift

# WindowShield state
crowdintel_shield_state  # 0=IDLE 1=COUNTDOWN 2=SETTLEMENT 3=HALTED 4=CLOSE_ONLY

# Strategy PnL
rate(crowdintel_pnl_realized_usd[5m])
crowdintel_daily_buy_volume_usd  # must reset at UTC midnight
```

## Scaling

- **Horizontal**: Stateless; can run N instances. Use static partition by
  market ID (hash market → instance) to avoid duplicate orders.
- **Vertical**: Thread per market. Hot path is lock-free (SPSC rings).
- **Cold path**: Binance WS ingestion, model reload, config hot-reload
  run on a separate thread with priority `SCHED_IDLE`.

## Deployment

```bash
# Build (release + network)
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=ON -DCROWDINTEL_CPU_TARGET=native \
  -DSECP256K1_ROOT=/tmp/secp256k1
cmake --build build -j$(nproc)

# Enable BTC specialization
export CROWDINTEL_ENABLE_BTC_SPECIALIZATION=1
export CROWDINTEL_CFC_MODEL=infra/models/cfc_btc_5m_v1.bin
export CROWDINTEL_BINANCE_FEED=wss://stream.binance.com:9443

# Run (paper trading first)
./build/bin/crowdintel_bot --paper --canary-stage=S1
```
