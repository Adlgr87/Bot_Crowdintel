# Plan de integración Polymarket CLOB V2 — revisión 2026-10-02

## Alcance y conclusión ejecutiva

Este documento es una especificación operativa para `Bot_Crowdintel`, no una promesa de rentabilidad. Ninguna configuración puede garantizar ser el sistema más rápido o rentable: la prioridad es **no enviar una orden cuando el estado, la firma, el reloj o la reconciliación sean inciertos**. El README y el ledger actual marcan el despliegue como **NO-GO para capital desatendido** porque aún falta reconciliación privada de órdenes/fills/inventario.

Los cambios preparados en este checkout son:

- `config.prod.toml`: política no secreta revisable (el ejecutable actual consume ENV, no TOML).
- `infra/config/kernel_tuning.conf`: política de kernel/chrony y objetivos de medición.
- Este documento: matriz oficial de parámetros, mapeo de arquitectura, gates y SLOs.

No se han inventado credenciales, token IDs ni datos de mercado. Es imposible entregar un `.env` de producción real sin conocer la wallet, el mercado y el API key del operador; cualquier archivo que los incluyera sería inseguro y no sería funcional.

## 1. Parámetros oficiales

### Conectividad y autenticación

| Elemento | Valor / comportamiento | Implementación requerida |
|---|---|---|
| Red | Polygon mainnet, `chainId=137` | Validar chain ID también por RPC en preflight. |
| REST CLOB | `https://clob.polymarket.com` | `CLOB_HOST`; HTTPS y verificación de hostname/CA. |
| Market WSS | `wss://ws-subscriptions-clob.polymarket.com/ws/market` | Sin auth; suscribir `{"assets_ids":[...],"type":"market"}`. |
| User WSS | `wss://ws-subscriptions-clob.polymarket.com/ws/user` | Auth con API creds; fuente de fills/órdenes, no sustituto de reconciliación REST. |
| RPC | No hay un RPC Polygon privilegiado publicado por Polymarket | Configurar un proveedor Polygon HTTPS/WS redundante elegido por el operador; no poner claves en Git. |
| Auth L1 | EIP-712 `ClobAuthDomain`, versión `1`, chain 137 | Se usa para crear/derivar credenciales L2. |
| Auth L2 | `POLY_ADDRESS`, `POLY_SIGNATURE`, `POLY_TIMESTAMP`, `POLY_API_KEY`, `POLY_PASSPHRASE` | HMAC-SHA256 de método/path/body fuera del hot path. |
| Credenciales | `createOrDeriveApiKey()`/equivalente oficial | Guardar key, secret y passphrase en ficheros 0400 vía `LoadCredential`; nunca en ENV persistente, logs o Git. |
| Firma de orden | CLOB V2 EIP-712 | Para V2 el `Order` contiene `timestamp`, `metadata`, `builder`; no contiene `nonce`, `taker` ni `feeRateBps`. |

**Corrección importante:** `feeRateBps` no debe agregarse al payload firmado V2. Era parte del formato anterior. En V2 la comisión la determina el protocolo al hacer match y se consulta en `clob-markets`; el modelo documentado es `fee = C × feeRate × p × (1-p)`, con comisión para taker y makers normalmente sin comisión. El código ya refleja la eliminación de `feeRateBps`; añadirlo rompería el hash de tipo y la compatibilidad.

Firma types: `0 EOA`, `1 POLY_PROXY`, `2 GNOSIS_SAFE`, `3 POLY_1271` (deposit wallet/ERC-7739). Este repositorio falla cerrado para tipo 3 hasta verificar el wrapper ERC-7739; no convertir una firma de 65 bytes en una falsa firma tipo 3.

### Contratos Polygon publicados

- pUSD CLOB V2: `0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB`
- CTF: `0x4D97DCd97eC945f40cF65F87097ACe5EA0476045`
- CTF Exchange V2: `0xE111180000d2663C0091e4f400237545B87B996B`
- Neg Risk CTF Exchange V2: `0xe2222d279d744050d28e00520010520000310F59`
- USDC.e histórico: `0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174` — no asumirlo como collateral de CLOB V2; la migración documenta pUSD.

El dominio de orden V2 es `name="Polymarket CTF Exchange"`, `version="2"`, `chainId=137` y el exchange estándar o Neg Risk según `neg_risk` del libro. El dominio de auth sigue siendo versión 1.

### Libro y ejecución

- `min_order_size` es un parámetro **por mercado**. El valor documentado habitual es `5` shares, no una garantía universal ni necesariamente 5 USDC.
- `tick_size` también es por mercado y puede cambiar. Valores soportados por la API incluyen `0.1`, `0.01`, `0.005`, `0.0025`, `0.001`, `0.0001`. Precio permitido: múltiplo exacto del tick; el rango operativo normal es `0.001..0.999`.
- Consultar `/book`/`/books` y `GET /clob-markets/{condition_id}`; ante `tick_size_change`, reemplazar el tick y regenerar órdenes.
- `GTC`: orden resting hasta fill/cancel; `GTD`: expiración Unix seconds y usar sólo cuando la duración sea compatible con el venue; `FOK`: todo el tamaño inmediatamente o nada; `FAK`: ejecutar lo disponible y cancelar el resto. Para el primer canary, FAK y tamaño mínimo.
- El stream market emite `book`, `price_change`, `last_trade_price`, `tick_size_change`, y opcionalmente `best_bid_ask`, `new_market`, `market_resolved`. Tras desconexión, invalidar el libro, reconectar con backoff y obtener snapshot completo antes de habilitar trading.
- En market/user WSS enviar texto `PING` cada 10 s y exigir `PONG`. Para proteger órdenes resting, enviar heartbeat CLOB **cada 5 s** con el último `heartbeat_id`; si no llega uno válido en 10 s, el venue cancela las órdenes (la comprobación puede tardar hasta 5 s). El heartbeat es por cuenta/API credential, no por mercado; el watchdog local sí debe mantener estado por mercado.

### Cuotas y comisiones

Valores actuales documentados (ventanas deslizantes/IP y buckets por signer; no asumir que son permanentes):

- General: `15,000/10 s`; CLOB general `9,000/10 s`.
- `/book`, `/price`, `/midpoint`: `1,500/10 s`; lotes `/books`, `/prices`, `/midpoints`: `500/10 s`.
- Ledger `/trades`, `/orders`, `/notifications`, `/order`: `900/10 s`.
- `POST /order`: burst `5,000/10 s`, sustained `120,000/10 min`.
- `DELETE /order`: `5,000/10 s`, `120,000/10 min`.
- `POST /orders`: `2,000/10 s`, `21,000/10 min`.
- `DELETE /orders`: `2,000/10 s`, `15,000/10 min`.
- `DELETE /cancel-all`: `250/10 s`, `6,000/10 min`.
- `DELETE /cancel-market-orders`: `1,500/10 s`, `21,000/10 min`.

El scheduler debe aplicar token bucket por endpoint y signer, alertar a 70%/85%/95%, y reservar margen para cancelaciones. Preferir cancelación por mercado; `cancel-all` sólo como emergencia. No reintentar ciegamente una escritura con timeout ambiguo.

### Ciclo de mercado y paridad

Descubrir por Gamma/market metadata → verificar `active`/`order_enabled` → snapshot CLOB → operar → observar `market_resolved`/cierre → reconciliar y liquidar. Un mercado multi-resultado/Neg Risk no es automáticamente arbitraje: hay que mapear condition ID, token IDs, sets y reglas de resolución.

`Yes + No != 1` sólo es una señal bruta. El edge neto debe restar spread, impacto, fee de taker, latencia, riesgo de resolución y coste de inventario. No cruzar dos libros como si fueran una ejecución atómica salvo que el producto/venue lo garantice.

## 2. Mapeo de `Bot_Crowdintel`

### Hot path

- `core/src/ws_market_listener.hpp` parsea snapshots/deltas y heartbeat; `core/include/order_book.hpp` publica top-of-book coherente. La profundidad usa mutex en el hilo de ingestión; por tanto no afirmar “lock-free L2” de extremo a extremo. El hot loop lee la publicación atómica, que sí evita bloqueo.
- `core/include/spsc_ring_buffer.hpp` es tipado y bounded. Ajustar capacidad con replay/telemetría; una ráfaga de 500 órdenes/s de burst no equivale a frecuencia de ticks. El tamaño debe cubrir el peor backlog medido más margen y hacer backpressure fail-closed.
- `core/crypto/eip712_signer.hpp` usa V2 y **no** `feeRateBps`; mantenerlo así y contrastar digest con SDK V2.
- HTTPS/HMAC está en `lightweight_client.hpp`, fuera del decisor. No existe bypass seguro que elimine TLS/HMAC; moverlo al hot path sería una regresión de seguridad. El objetivo real es preconstruir lo posible y medir tick→wire por separado.
- `malloc` de contexto secp ocurre en inicialización, no debe ocurrir durante decisión; auditar con allocator tracing en el binario de producción. Ningún benchmark CPU prueba ausencia de page faults, locks del kernel o latencia de red.

### Cold path

- `alpha/crowdintel`: validar origen con bearer, timestamp, nonce/id único y market/token allow-list. Rechazar duplicados y señales expiradas.
- FDR: agrupar hipótesis por ventana/mercado, ordenar p-values, usar Benjamini–Hochberg `p(i) <= i/m × q`; empezar `q=0.05`, mínimo 200 observaciones y recalibrar por categoría. No llamar “q-value” a un simple confidence score.
- Kelly: el motor existente usa Kelly fraccional y límites. Usar `f=0.10` inicial, cap por orden/exposición y actualizar sólo con fills reconciliados; nunca convertir una reserva en PnL.
- Insider/anomalía: z-score de volumen, intensidad de trades, distancia temporal al evento y cambio de probabilidad, con ventana inicial 120 s y z-score 4.0. Es una alerta de riesgo, no prueba de insider trading; bloquear o reducir tamaño cuando la procedencia es incierta.

### Optimización

Aplicar primero LTO/PGO al flujo medido de parseo → decisión → construcción; no optimizar funciones de red con microbenchmarks. AVX2/Sapphire Rapids puede ayudar a copias/hash, pero secp256k1 debe medirse y validarse contra KATs. Mantener build `portable`/`x86-64-v3` y no distribuir `-march=native`. La optimización genética sólo es admisible para hiperparámetros de estrategia fuera de línea, con walk-forward, costes y holdout; nunca para límites de riesgo, firmas o controles de seguridad.

## 3. Operación, degradación y despliegue

1. `BOT_MODE=mock` por defecto; 24 h de replay/shadow sin órdenes.
2. WSS cae: invalidar libro y no cotizar; REST sólo sirve para snapshot/health, con rate limiter. No presentar REST como equivalente a tiempo real.
3. WSS vuelve: snapshot, hash, tick, min size, neg risk y market state deben concordar; después habilitar.
4. Modo live sólo con EOA tipo 0 al principio, FAK, un mercado, wallet de canary y reconciliación externa.
5. Región: medir desde varias regiones; “Ámsterdam <1 ms a Polygon” no es un hecho garantizable. Polygon no equivale a un único punto físico de matching y el RTT de AWS no fija tick-to-fill.
6. Reservar 1–2 CPUs y aplicar `infra/config/kernel_tuning.conf` sólo tras benchmark y revisión del host. `chrt -f` requiere privilegio y puede congelar el host si se usa sin watchdog.

## 4. Métricas y gates

Medir con `CLOCK_MONOTONIC_RAW` y calibrar TSC a nanosegundo; guardar ciclos y frecuencia junto a cada muestra. Segmentar: WSS receive→parse, parse→decision, decision→sign, sign→TLS write, first byte y semantic ack. P99 `<22 µs` tick-to-wire, `<14 µs` firma y `<2 µs` jitter son **objetivos de ingeniería**, no valores documentados por Polymarket ni garantías de venue.

El rendimiento neto debe ser: fills − fees − slippage − inventario − errores/ambigüedades − infraestructura + rebates realmente acreditados. Reportar fill ratio, adverse selection, cancel/replace, alpha calibration, Brier/log loss, drawdown y PnL por categoría; no optimizar sólo hit-rate.

### Checklist de entrada en vivo

- [ ] chain ID 137 y contratos verificados contra la página oficial vigente
- [ ] token/condition/neg-risk/tick/min-size leídos en preflight
- [ ] allowances y balance pUSD/posición reconciliados
- [ ] API credential pertenece a la signer/maker correctos
- [ ] reloj dentro de ±10 ms como gate operativo; chrony estable y alertado
- [ ] 24 h mock/shadow sin desconexión no recuperada
- [ ] heartbeat WSS 10 s y heartbeat de órdenes 5 s verificados
- [ ] stop-loss, max daily loss y kill switch probados
- [ ] checkpoint de estado cada 60 s y backups fuera del proceso
- [ ] Telegram/alerta equivalente probada (en la config queda desactivada hasta provisionarla)
- [ ] canal privado + REST de reconciliación de fills/órdenes/inventario habilitado
- [ ] canary humano, FAK, tamaño mínimo, sin retry ante timeout ambiguo

## 5. Fuentes oficiales

- [Contracts](https://docs.polymarket.com/resources/contracts)
- [CLOB V2 migration](https://docs.polymarket.com/v2-migration)
- [Authentication](https://docs.polymarket.com/developers/CLOB/authentication)
- [Place Orders](https://docs.polymarket.com/trading/orders/create)
- [Orderbook / market stream](https://docs.polymarket.com/trading/orderbook)
- [Real-time data and heartbeat](https://docs.polymarket.com/market-data/websocket/overview)
- [User order updates and reconnect recovery](https://docs.polymarket.com/trading/realtime-order-updates)
- [Order heartbeat](https://docs.polymarket.com/trading/orders/overview)
- [Rate limits](https://docs.polymarket.com/api-reference/rate-limits)
- [CLOB market info](https://docs.polymarket.com/api-reference/markets/get-clob-market-info)
- [Deposit wallet / ERC-7739](https://docs.polymarket.com/trading/deposit-wallets)

Las cifras y direcciones anteriores deben volver a verificarse durante cada release: la documentación de V2 ya cambió collateral y el modelo de fees respecto a integraciones históricas.
