# VERIFICATION — OBS-01: DSH no validado OOS

**Verificador:** AUD-STAT
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** GCC 16.2, CMake 3.20+, Python 3.x (backtest), secp256k1 @ pin

---

## Veredicto: VERIFIED_FAIL

La estrategia DSH no está validada out-of-sample. El pipeline de backtest existente (`ml_training/backtest.py`) está diseñado exclusivamente para el **CfC legacy** (red neuronal de 32 hidden units). No existe un pipeline de backtest para la estrategia determinista actual (Brownian Bridge TWAP + Volatility EWMA + OFI + Kelly).

---

## Evidencia

### 1. Inventario de infraestructura de backtest

| Archivo | Propósito | ¿Aplica a DSH? |
|---------|-----------|-----------------|
| `ml_training/backtest.py` | Backtest para CfC neural net (train_config, model weights) | ❌ No — usa `cfc_network.hpp`, no `bayesian_engine.hpp` |
| `ml_training/train_cfc.py` | Entrenamiento CfC offline | ❌ No |
| `ml_training/config.yaml` | Hiperparámetros CfC (lr=1e-3, batch=64, etc.) | ❌ No |
| `ml_training/export_weights.py` | Exporta pesos .bin del CfC | ❌ No |
| `ml_training/kat_vectors.json` | Vectores de test del CfC | ❌ No |
| `tests/replay/l2_backtester.cpp` | Replay de ticks L2 (incompleto) | ⚠️ Parcial — solo replay, no estrategia DSH |
| `tests/benchmarks/latency_bench.cpp` | Microbenchmark CPU path | ❌ No — solo latencia |

### 2. Análisis del código de la estrategia DSH

El hot path DSH está en `core/src/engine_extensions.hpp` + `core/src/execution_engine.hpp`. Los componentes son:

- **TwapBrownianBridge** (`twap_brownian_bridge.hpp`): P(TWAP_final > K) = Φ(d), d = (A_t·t + S_t·τ − K·T) / (σ·√(τ³/3))
- **VolatilityEstimator** (`volatility_estimator.hpp`): EWMA multi-escala, regime detection (NORMAL/SHOCK/CALM)
- **OfiLinearFilter** (`ofi_linear_filter.hpp`): OFI z-score
- **KellySizer** (`kelly_sizer.hpp`): quarter-Kelly, cap 5% bankroll
- **LadderBuilder** (`ladder_builder.hpp`): quote construction
- **BayesianEngine** (`bayesian_engine.hpp`): Beta-Binomial posterior (P4)

**Ningún componente tiene un backtest integrado que valide la estrategia completa OOS.**

### 3. ¿Qué datos están disponibles?

- `tests/replay/sample_ticks.csv` — 50 ticks de muestra para replay (insuficiente para OOS).
- No hay dataset histórico de Polymarket CLOB V2 ni datos de Binance BTC/USDT para validar la estrategia.

### 4. Gap de calibración

El `BayesianEngine` usa `bayes_prior_strength = 24.0` (prior N0) y `bayes_signal_threshold = 0.03`. Ninguno de estos hiperparámetros ha sido calibrado con datos históricos. La estrategia actual opera con valores hardcoded sin validación estadística.

### 5. Baseline de referencia (teórico)

Sin datos OOS, no se puede comparar contra:
- (a) Baseline 50/50
- (b) Precio de mercado como probabilidad
- (c) Media móvil

---

## Impacto

- **CRITICAL**: La canary deployment (H1-H15) requiere "CfC calibration ECE < 2% on 7-day backtest" pero el motor actual es DSH, no CfC. No hay validación de calibración para DSH.
- El GATE-FINAL no puede firmarse sin backtest OOS.
- La estrategia DSH opera sin evidencia estadística de rentabilidad.

## Recomendación de implementación (para IMP)

1. Crear `ml_training/backtest_dsh.py` — pipeline de backtest para la estrategia determinista DSH sobre datos históricos de Polymarket CLOB V2.
2. Implementar split temporal train/val/test sin leakage.
3. Calibrar hiperparámetros (N0, threshold, Kelly fraction, vol estimators).
4. Reportar Brier, log-loss, ECE, reliability diagram, calibración por régimen.
5. Walk-forward con σ solo usando datos previos a t.

## Artefactos

- `reports/stat/obs-01-verification.md` (este archivo)
- `agents/handoffs/obs-01-DSH-no-validado-OOS/artifacts/code_inventory.json` (inventario)
- `agents/handoffs/obs-01-DSH-no-validado-OOS/artifacts/component_audit.md` (auditoría de componentes)

## Signoff

```json
{
  "card_id": "OBS-01",
  "role": "AUD-STAT",
  "agent": "subagent_nemesis (auditor estadístico)",
  "verdict": "VERIFIED_FAIL",
  "evidence_paths": [
    "agents/handoffs/obs-01-DSH-no-validado-OOS/artifacts/code_inventory.json",
    "agents/handoffs/obs-01-DSH-no-validado-OOS/artifacts/component_audit.md"
  ],
  "timestamp": "2026-01-22T10:00:00Z",
  "commit_hash": "c157c23",
  "toolchain": {"gcc": "16.2", "cmake": "3.20+", "python": "3.x", "secp256k1": "6e2c8bc4"},
  "reproducible": true
}
```
