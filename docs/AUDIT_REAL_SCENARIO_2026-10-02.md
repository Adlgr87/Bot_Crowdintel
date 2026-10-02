# Auditoría de preparación para escenario real — 2026-10-02

## Veredicto

**NO-GO para capital real desatendido.** El repositorio tiene un camino mock/offline avanzado y varios controles fail-closed, pero no puede declararse “todo conectado y listo para producción” porque faltan piezas que afectan directamente a la seguridad de fondos:

1. No existe cliente privado WebSocket `/ws/user`.
2. No existe reconciliación autenticada de órdenes, fills, balances, allowances e inventario al arrancar y después de reconectar.
3. El proceso no puede resolver una respuesta ambigua del gateway consultando el estado de la cuenta antes de continuar.
4. `config.prod.toml` es documentación revisable; el binario sólo consume variables de entorno.
5. No se ha validado una cuenta, mercado, token, allowance y firma contra un CLOB real desde este entorno.
6. Las metas de latencia no están verificadas en hardware de producción.

La conclusión no significa que el código esté inutilizable. Significa que las garantías verificables son de investigación, mock y shadow; no de capital desatendido.

## Componentes verificados por inspección

| Área | Estado | Observación |
|---|---|---|
| EIP-712 CLOB V2 | Implementado offline | Chain 137, dominio V2, exchange estándar/Neg Risk. Correctamente no incluye `feeRateBps`, que fue eliminado en V2. |
| CLOB REST | Parcial | `LightweightCLOBClient` calcula auth L2 y valida respuesta semántica; requiere cuenta/credenciales reales para comprobar wire compatibility. |
| Market WSS | Parcial | TLS, upgrade, suscripción market, snapshot/deltas, invalidación y reconnect están implementados. No sustituye al user channel. |
| User WSS | Faltante | No hay `/ws/user`, fills privados ni eventos de órdenes. |
| Order gateway | Implementado con límites | Cola SPSC, egress asíncrono y retries sólo si el cliente los marca seguros. Un HTTP 2xx no equivale a fill. |
| Inventario/PnL | Parcial | Hay reservas conservadoras; no existe fuente autoritativa de fills/inventario. |
| Riesgo/Kelly | Parcial/implementado | Límites y Kelly están en el motor, pero el PnL no puede ser autoritativo sin reconciliación. |
| Credenciales | Fail-closed | `_FILE`, permisos, zeroización y `mlockall` en live. Tipo 3 se rechaza. |
| Heartbeat | Parcial | Market WSS envía PING y verifica PONG; heartbeat de órdenes CLOB no está integrado en el proceso. |
| Configuración | Parcial | ENV es la configuración efectiva; TOML no se parsea. Token, fees y tick deben venir de metadata actual. |
| Kernel/CPU | Documentado | El archivo de tuning no aplica cambios automáticamente y no demuestra jitter. |
| Tests | CI preparado | El entorno local no tiene CMake/secp256k1 instalados; no se pudo repetir CTest aquí. |

## Conectividad comprobada

El código apunta a:

- REST: `https://clob.polymarket.com`
- Market WSS: `wss://ws-subscriptions-clob.polymarket.com/ws/market`
- User WSS requerido: `wss://ws-subscriptions-clob.polymarket.com/ws/user` (no implementado)

Se intentó una comprobación HTTP pública contra `/ok`, `/time` y Gamma desde el sandbox. Las tres conexiones terminaron en `SSL_ERROR_SYSCALL`, por lo que no se pudo certificar conectividad de red desde este entorno. Esto no prueba que el venue esté caído; sólo impide una comprobación de conectividad desde el sandbox.

## Riesgos críticos antes de live

### P0 — Reconciliación ausente

Después de timeout, caída o reinicio, una orden puede existir en el venue aunque el proceso no la haya observado. Sin `/ws/user` y lecturas REST de órdenes/trades/balances, el bot no sabe si debe cancelar, reservar, liberar o detenerse.

**Acción:** implementar `UserChannel`, `AccountSnapshot`, `FillLedger` y un reconciler idempotente. La regla debe ser: estado desconocido ⇒ no nuevas órdenes.

### P0 — Heartbeat de órdenes ausente

El market WSS heartbeat no protege órdenes resting. Polymarket documenta el heartbeat de órdenes cada 5 segundos y la cancelación si no se mantiene. El bot debe mantener el `heartbeat_id`, reintentar sólo antes de ambigüedad y detener la cotización si falla.

**Acción:** integrar `/v1/heartbeats` en el cold path y exponer estado ready/not-ready.

### P1 — Metadata no dinámica

El bot valida tick configurado, pero no existe en el camino de ejecución mostrado una consulta obligatoria de `min_order_size`, `tick_size`, `neg_risk` y fee por mercado antes de armar una cuenta real.

**Acción:** preflight obligatorio y cache invalidada por `tick_size_change`/market lifecycle.

### P1 — Configuración declarativa no efectiva

`config.prod.toml` no es leído por el binario. Copiarlo al host no cambia el comportamiento. La única configuración efectiva es ENV, principalmente `infra/config/production.env.example` y archivos de credenciales.

**Acción:** o implementar parser TOML con schema estricto, o eliminar la apariencia de que TOML es ejecutable y generar ENV validado durante deployment.

### P1 — Tipo de firma 3

Está correctamente bloqueado. No activar hasta implementar y comparar el wrapper ERC-7739 con el SDK V2. El primer canary debe ser tipo 0, o tipo 1/2 sólo después de verificar maker/signer/API owner.

### P2 — Latencia

El benchmark actual es CPU/offline. No mide DNS, TCP, TLS, HMAC, kernel scheduling, retransmisión, venue queue, first byte ni semantic acknowledgement. No se debe anunciar P99 `<22 µs` como propiedad del sistema completo.

## Criterio de aprobación

El sistema sólo puede avanzar a un canary supervisado cuando se obtenga evidencia de:

- Build y CTest de CI del commit exacto.
- Firma EIP-712 igual a la del SDK V2 para la cuenta exacta.
- Preflight de chain, contrato, token, tick, min size, fee, balance y allowance.
- User WSS estable y reconciliación REST después de desconexión inyectada.
- Heartbeat de órdenes aceptado durante al menos 24 horas.
- Shadow/replay 24 horas sin estado divergente.
- Canary FAK mínimo, sin retry de timeout ambiguo, con reconciliación independiente antes de una segunda orden.

## Fuentes operativas

- https://docs.polymarket.com/developers/CLOB/authentication
- https://docs.polymarket.com/trading/realtime-order-updates
- https://docs.polymarket.com/trading/orders/overview
- https://docs.polymarket.com/market-data/websocket/overview
- https://docs.polymarket.com/v2-migration
- https://docs.polymarket.com/resources/contracts
- https://docs.polymarket.com/api-reference/rate-limits
