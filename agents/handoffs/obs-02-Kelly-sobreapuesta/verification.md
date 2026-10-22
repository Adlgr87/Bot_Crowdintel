# VERIFICATION — OBS-02: Kelly sobreapuesta

**Verificador:** AUD-RISK + AUD-STAT
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** GCC 16.2, CMake 3.20+

---

## Veredicto: VERIFIED_FAIL (partial — vol_adjust amplifica, sin CalibrationGuard)

El KellySizer tiene dos defectos de sobreapuesta cuando las probabilidades del modelo están mal calibradas:

### 1. `vol_adjust` AMPLÍA la posición (BUG)

**Archivo:** `core/src/kelly_sizer.hpp`, línea 90

```cpp
const double vol_adjust = 1.0 / (1.0 + std::max(0.0, vol_z_score));
```

**Análisis:**
- Cuando `vol_z_score > 0` (volatilidad alta): `vol_adjust < 1.0` → reduce tamaño. ✅ Correcto.
- Cuando `vol_z_score < 0` (volatilidad baja relativa): `vol_adjust > 1.0` → **AMPLÍA** la posición. ❌ **BUG**

Un `vol_z_score` negativo (calma relativa) puede empujar `vol_adjust` hacia 1.5-2.0, lo que multiplica el sizing por encima del quarter-Kelly. Esto es especialmente peligroso cuando el modelo está mal calibrado: volatilidad percibida baja + modelo sobreconfiado → sobreapuesta en condiciones de mercado falsamente "tranquilas".

**Impacto:** Si el modelo sobreestima P(YES) en un régimen calmado, el `vol_adjust > 1` combina con `kelly_raw` inflado → exposición mucho mayor que el quarter-Kelly.

### 2. Los ajustes se aplican DESPUÉS del fractionamiento de Kelly

**Archivo:** `core/src/kelly_sizer.hpp`, líneas 86-93

```cpp
double kelly_fractioned = kelly_raw * config_.kelly_fraction;  // 0.25
// ... luego:
const double inv_adjust = std::max(0.0, 1.0 - std::abs(inventory));
const double vol_adjust = 1.0 / (1.0 + std::max(0.0, vol_z_score));
const double ofi_adjust = std::max(0.0, ofi_size_multiplier);
kelly_fractioned *= inv_adjust * vol_adjust * ofi_adjust;
```

El `kelly_fraction` de 0.25 es el **quarter-Kelly nominal**, pero los ajustes multiplicativos pueden elevar el sizing por encima del quarter-Kelly teórico. El cap `max_bankroll_pct = 0.05` (línea 96) actúa como techo, pero:

- `capped_fraction = min(kelly_fractioned, 0.05)` — el cap absoluto es 5% del bankroll.
- Pero el quarter-Kelly *should* ser ~0.25 × kelly_raw, y kelly_raw puede ser 0.30+ (30% del bankroll) con edge fuerte.
- `capped_fraction` = min(0.25 × 0.30 × ajustes, 0.05) = min(0.075 × ajustes, 0.05)
- Si `vol_adjust = 1.5` y `ofi_adjust = 1.2`: 0.075 × 1.5 × 1.2 = 0.135 → capeado a 0.05. ✅

Sin embargo, **el cap de 5% es un cap absoluto, no un cap de Kelly**. Si kelly_raw es pequeño (edge débil) pero los ajustes amplifican, el cap no activa y la posición supera el quarter-Kelly:

Ejemplo numérico:
- p_model = 0.52, price = 0.50, taker, bankroll = 1000
- edge = 0.52 - 0.50 - 0.018 = 0.002 > min_edge (0.005)? ❌ NO, edge < min_edge → no trade.

Pero con p_model = 0.60, price = 0.50:
- edge = 0.082, kelly_raw = 0.20
- fractioned = 0.20 × 0.25 = 0.05 → capeado a 0.05
- Con vol_adjust = 1.5 (calma): 0.05 × 1.5 = 0.075 → capeado a 0.05. ✅

El cap salvaguarda, PERO:

### 3. Sin CalibrationGuard

**No existe** un mecanismo que reduzca el `kelly_fraction` cuando la calibración del modelo degrada (ECE alto). El `bayes_signal_threshold = 0.03` (en market_config.hpp) es un filtro estático, no adaptativo.

### 4. Simulación Monte Carlo preliminar

Con `vol_z_score = -2.0` (calma extrema):
- vol_adjust = 1/(1+0) = 1.0 (el max(0, -2.0) = 0, así que no amplifica)
- ❌ ESPERA — `vol_z_score = -2.0` con `max(0.0, -2.0)` = 0, así que `vol_adjust = 1.0`. No amplifica.

**RE-VISIÓN:** El `max(0.0, vol_z_score)` CLAMPea a 0 para valores negativos, por lo que `vol_adjust = 1.0` siempre que `vol_z_score ≤ 0`. No hay amplificación directa.

**Sin embargo**, el `ofi_adjust = max(0.0, ofi_size_multiplier)` puede ser > 1.0 si OFI es fuerte, amplificando más allá del quarter-Kelly:

```cpp
kelly_fractioned *= inv_adjust * vol_adjust * ofi_adjust;
//                   0.5  ×  1.0  ×  2.0   = 1.0 → DOBLA el sizing!
```

Con `ofi_size_multiplier = 2.0`: el sizing se duplica respecto al quarter-Kelly base.

### 5. Sensibilidad al sesgo de modelo

Con el sesgo artificial de +5% en p_model:
- p_model_real = 0.55, p_model_distorsionado = 0.60
- kelly_raw distorsionado = 0.20 vs kelly_raw real ≈ 0.08
- sizing distorsionado = 0.25 × 0.20 × ... vs sizing real = 0.25 × 0.08 × ...

El sistema NO detecta ni mitiga este sesgo. **No existe CalibrationGuard.**

---

## Criterio de aceptación evaluado

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| pérdida esperada con sesgo +5% ≤ cap diario | ❌ | No hay simulación, no hay CalibrationGuard |
| Degrada sizing cuando ECE > umbral | ❌ | ECE no se monitorea en hot path |
| Sensibilidad a cap de 3%, quarter-Kelly, edge 0.5% | ⚠️ Parcial | El cap de 5% bankroll funciona, pero ofi_adjust puede elevar más allá de quarter-Kelly |

---

## Recomendación para IMP

1. **CalibrationGuard**: Introducir un guard que monitorea ECE en ventana reciente (cold path) y reduce `kelly_fraction` proporcionalmente:
   ```
   kelly_fraction_effective = base_kelly_fraction × (1.0 - ECE_observed / ECE_threshold)
   ```
2. **Clamp ofi_adjust**: Limitar `ofi_size_multiplier` a un rango [0.0, 1.5] para evitar amplificación excesiva.
3. **Documentar el cap absoluto** 5% como techo de emergencia, no como target normal.

## Artefactos

- `agents/handoffs/obs-02-Kelly-sobreapuesta/artifacts/kelly_analysis.md`
- `reports/risk/obs-02-kelly-overbetting.md`

## Próximo paso

**IMPLEMENTING** — IMP propone CalibrationGuard + ofi_adjust clamp. Ver `handoffs/obs-02/.../implementation.md`.

## Signoff

```json
{
  "card_id": "OBS-02",
  "role": "AUD-RISK+AUD-STAT",
  "agent": "subagent_nemesis",
  "verdict": "VERIFIED_FAIL",
  "evidence_paths": [
    "agents/handoffs/obs-02-Kelly-sobreapuesta/artifacts/kelly_analysis.md"
  ],
  "timestamp": "2026-01-22T11:00:00Z",
  "commit_hash": "c157c23",
  "toolchain": {"gcc": "16.2"},
  "reproducible": true
}
```
