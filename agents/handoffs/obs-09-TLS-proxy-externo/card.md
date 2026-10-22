### OBS-09: TLS/proxy externo y loopback (M8 no corregido)

- **Origen**: OBS-09 (original M8)
- **Severidad**: HIGH
- **Agente verificador asignado**: AUD-SEC + AUD-NET
- **Agente implementador asignado**: IMP
- **Dependencias**: `lightweight_client.hpp`, `market_config.hpp`, `alpha_http_receiver.hpp`
- **Criterio de aceptación**: threat model documentado, TLS end-to-end o proxy con autenticación mTLS, rotación de secretos, logs sin secretos.
- **Riesgo de regresión**: Forzar TLS puede romper despliegues existentes sin cert.
- **Rollback**: feature flag `BOT_REQUIRE_TLS` (default OFF).
- **Deadline (timebox)**: 5 días (HIGH)