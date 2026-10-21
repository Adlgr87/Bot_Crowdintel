# AUD-RISK Report — OBS-02: Kelly sobreapuesta

**Date:** 2026-01-22 | **Auditor:** AUD-RISK + AUD-STAT | **Commit:** c157c23

## Resumen

El KellySizer no incluye un CalibrationGuard que reduzca el fractionamiento cuando el modelo está mal calibrado (ECE alto). La lógica de ajustes multiplicativos puede elevar el sizing por encima del quarter-Kelly nominal en ciertas condiciones.

## Hallazgos

### 1. vol_adjust NO amplía (corregido en re-análisis)

```cpp
const double vol_adjust = 1.0 / (1.0 + std::max(0.0, vol_z_score));
```

El `std::max(0.0, vol_z_score)` previene amplificación cuando `vol_z_score < 0`. **No es un bug.**

### 2. ofi_adjust puede exceder 1.0

```cpp
const double ofi_adjust = std::max(0.0, ofi_size_multiplier);
kelly_fractioned *= inv_adjust * vol_adjust * ofi_adjust;
```

Cuando `ofi_size_multiplier > 1.0`, el sizing se eleva por encima del quarter-Kelly. El cap `max_bankroll_pct = 0.05` actúa como techo absoluto, pero:

- Con kelly_raw pequeño (edge débil), el techo no activa
- El sizing puede superar el quarter-Kelly teórico

**Ejemplo:** kelly_raw = 0.08, kelly_fraction = 0.25 → 0.02
Con ofi_adjust = 2.0 → 0.04 (2× el quarter-Kelly nominal)

### 3. Sin CalibrationGuard

- No hay monitoreo de ECE en el hot path
- No hay reducción adaptativa de kelly_fraction
- El `bayes_signal_threshold = 0.03` es un filtro estático, no adaptativo

### 4. Sensibilidad al sesgo de modelo

Con +5% de sesgo en p_model:
- kelly_raw se duplica (0.10 → 0.20 en ejemplo típico)
- No hay detección ni mitigación

## Recomendación

1. Clamp `ofi_size_multiplier` a [0.0, 1.5] en KellySizer
2. Añadir CalibrationGuard: reducir kelly_fraction proporcionalmente a ECE observado
3. Añadir ECE tracking en cold path (bayesian_engine posterior vs market price)
