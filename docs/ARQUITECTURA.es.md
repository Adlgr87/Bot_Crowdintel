# Arquitectura — resumen operativo

La especificación técnica vigente está en [ARCHITECTURE.md](ARCHITECTURE.md), el estado en [STATUS.md](STATUS.md) y el procedimiento de despliegue en [DEPLOYMENT.md](DEPLOYMENT.md).

## Flujo

```text
productor alpha
  → HTTP POST /signal autenticado y acotado
  → normalización/validación
  → SPSC tipada
  → motor de ejecución ← libro L2 publicado por WSS
  → riesgo/frescura/deduplicación
  → pool prefirmado consumible o firma inline
  → SPSC de egreso
  → HTTPS persistente + HMAC L2
  → respuesta semántica CLOB
```

El hot path no realiza DNS, TLS, JSON ni sockets. Esa separación reduce jitter de CPU, pero **no** convierte una medición submicrosegundo del pool en latencia de orden end-to-end.

## Invariantes

- El top del libro se publica atómicamente; la profundidad está protegida. No se usa un seqlock sobre arrays no atómicos para ocultar carreras.
- Las colas son SPSC reales: exactamente un productor y un consumidor.
- El pool usa tres buffers con lectores contabilizados; un slot pasa a consumido una sola vez por CAS y no cruza una generación de tick.
- Precio, shares y amounts se calculan en enteros. Un tick no reconocido invalida el libro y falla cerrado.
- BUY reserva el peor coste; SELL exige inventario confirmado. Una reserva nunca se contabiliza como fill.
- Encolar no significa aceptación. Se exige HTTP 2xx, `success:true` y ausencia de error semántico.
- Un timeout ambiguo no se reintenta a ciegas. El kill switch bloquea enqueue/egress y el apagado descarta órdenes aún no enviadas.
- El tipo de firma 3 falla cerrado: necesita el wrapper ERC-7739, no una ECDSA plana de 65 bytes.

## Estado de seguridad

Los secretos pueden entrar por archivos `_FILE` regulares, no-symlink y con permisos privados; entradas vacías, truncadas, ilegibles o con caracteres de control se rechazan. El despliegue usa `LoadCredential=` de systemd, memoria bloqueada, usuario sin login, capacidades vacías y core dumps deshabilitados. El proceso conserva necesariamente la clave del signer en RAM; un host privilegiado comprometido sigue fuera del modelo de protección.

## Etapa y bloqueo para producción autónoma

El canal privado autenticado y la reconciliación de órdenes, fills, balances,
allowances e inventario **ya están implementados** (Fases 1-8, con pruebas sobre un
venue loopback) y la auditoría de 2026-10-03 corrigió 27 defectos, uno de ellos
bloqueante: `crowdintel-preflight` no ligaba la wallet, así que su puerta no podía
aprobarse nunca. El estado detallado está en [STATUS.md](STATUS.md) y en
[../README.md](../README.md).

Lo que sigue bloqueando el trading real desatendido no es código, es verificación:
ninguna orden, fill, heartbeat ni reconciliación se ha observado contra el venue real,
y siguen `[NO VERIFICADO]` el path y el body exactos del heartbeat, la cadencia real
de cancelación y si el `orderID` del venue coincide con el digesto EIP-712 local. Por
eso el servicio no se reinicia automáticamente y la salida a real pasa por ejecutar
**H1–H15** de [CANARY_CHECKLIST.md](CANARY_CHECKLIST.md).

Cualquier canary debe ser mínimo, supervisado, FAK, reconciliado externamente tras
cada orden y comparado antes con el SDK oficial actual.
