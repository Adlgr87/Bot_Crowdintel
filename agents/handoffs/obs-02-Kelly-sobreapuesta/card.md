### OBS-02: Kelly con probabilidades mal calibradas sobreapuesta

- **Origen**: OBS-02
- **Severidad**: HIGH
- **Agente verificador asignado**: AUD-RISK + AUD-STAT
- **Agente implementador asignado**: IMP
- **Dependencias**: `kelly_sizer.hpp`, `risk_manager.hpp`
- **Criterio de aceptación**: pérdida esperada en escenario de sesgo +5% ≤ cap diario definido; el sistema degrada el sizing automáticamente cuando ECE del modelo excede umbral.
- **Riesgo de regresión**: Modificar el sizing puede reducir volatilidad de PnL. Tests de kelly_sizer deben seguir pasando.
- **Rollback**: feature flag `BOT_KELLY_CALIBRATION_GUARD` (default OFF) para revertir sin tocar core.
- **Deadline (timebox)**: 7 días (HIGH)