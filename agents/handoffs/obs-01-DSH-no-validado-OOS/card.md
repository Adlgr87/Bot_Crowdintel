### OBS-01: La estrategia DSH no está validada out-of-sample

- **Origen**: OBS-01 / VER-01
- **Severidad**: CRITICAL
- **Agente verificador asignado**: AUD-STAT
- **Agente implementador asignado**: IMP
- **Dependencias**: Ninguna (análisis estadístico sobre datos históricos)
- **Criterio de aceptación**: Brier del DSH ≤ baseline de mercado − margen definido (p. ej. 2%) en test, con intervalos de confianza bootstrap; ECE < 5%; walk-forward estable.
- **Riesgo de regresión**: No hay código que modificar en esta fase; la verificación es análisis estadístico. Riesgo de false-negative si el backtest tiene leakage.
- **Rollback**: No aplica (verificación, no implementación).
- **Deadline (timebox)**: 5 días (CRITICAL)
- **Descripción**: La estrategia DSH (Deterministic Signal: Brownian Bridge TWAP + Volatility EWMA + OFI + Kelly) no ha sido validada out-of-sample. El `ml_training/backtest.py` existe solo para el CfC legacy (red neuronal). No hay pipeline de backtest para la estrategia determinista actual.

## Pregunta de verificación

1. Reproducir el pipeline DSH sobre datos históricos de Polymarket CLOB V2, con fees (0.072), slippaje modelado y adverse selection.
2. Split temporal estricto (train/val/test) sin leakage; test intocable hasta el final.
3. Comparar contra baselines: (a) 50/50, (b) precio de mercado como probabilidad, (c) media móvil.
4. Reportar Brier score, log-loss, reliability diagram, ECE, calibración por régimen (NORMAL/SHOCK/CALM).
5. Repetir con σ estimada solo con datos previos a t (walk-forward).
