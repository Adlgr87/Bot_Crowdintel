# AUD-STAT Report — OBS-01: DSH Out-of-Sample Validation Gap

**Date:** 2026-01-22 | **Auditor:** AUD-STAT (NEMESIS) | **Commit:** c157c23

## Resumen

La estrategia DSH (Deterministic Signal) carece completamente de validación out-of-sample. El pipeline de backtest existente (`ml_training/backtest.py`) está dedicado exclusivamente al CfC legacy (red neuronal). No hay backtest, calibración, ni medición de Brier/ECE para la estrategia determinista actual.

## Hallazgos

### 1. No backtest OOS pipeline

- `ml_training/backtest.py`: Usa `cfc_network.hpp` (neural net), NO `bayesian_engine.hpp` o `twap_brownian_bridge.hpp`
- No existe `backtest_dsh.py` ni equivalente
- `tests/replay/l2_backtester.cpp`: Solo replay de ticks, no backtest de estrategia

### 2. Hiperparámetros hardcodeados sin calibración

| Parámetro | Valor | Archivo | Calibrado? |
|-----------|-------|---------|-----------|
| bayes_prior_strength | 24.0 | market_config.hpp:86 | ❌ |
| bayes_signal_threshold | 0.03 | market_config.hpp:87 | ❌ |
| ewma_lambda (BB) | 0.94 | twap_brownian_bridge.hpp:44 | ❌ |
| lambda_fast | 0.94 | volatility_estimator.hpp:24 | ❌ |
| lambda_slow | 0.98 | volatility_estimator.hpp:25 | ❌ |
| kelly_fraction | 0.25 | kelly_sizer.hpp/KConfig | ❌ |
| max_bankroll_pct | 0.05 | kelly_sizer.hpp/KConfig | ❌ |
| min_edge | 0.005 | kelly_sizer.hpp/KConfig | ❌ |

### 3. No métricas de calibración

- ❌ Brier score
- ❌ Log-loss
- ❌ Reliability diagram
- ❌ ECE (Expected Calibration Error)
- ❌ Regime calibration (NORMAL/SHOCK/CALM)

## Impacto en canary

- **H3**: "Model accuracy: CfC calibration ECE < 2% on 7-day backtest" — el motor es ahora DSH, no CfC. No hay validación de calibración.
- **GATE-FINAL**: No puede firmarse sin backtest OOS.

## Recomendación

1. Crear `ml_training/backtest_dsh.py` — pipeline de backtest para estrategia DSH
2. Recolectar datos históricos Polymarket CLOB V2 + Binance BTC/USDT
3. Implementar train/val/test split temporal sin leakage
4. Calibrar hiperparámetros contra datos históricos
5. Reportar Brier, log-loss, ECE, reliability diagram por régimen

## Prioridad: CRITICAL

Este bloqueo es un **gate-blocker para GATE-FINAL**. La canary no puede proceder sin validación OOS.
