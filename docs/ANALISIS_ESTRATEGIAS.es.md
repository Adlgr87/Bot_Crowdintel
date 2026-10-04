# Encaje del bot con los programas de incentivos de Polymarket

_Fecha del análisis: 2026-10-04. Alcance: el código de este repositorio (commit `f151bc7`) contra la documentación oficial y los endpoints públicos de Polymarket consultados ese mismo día._

> **Advertencia de método.** Todo lo que se afirma sobre el bot está verificado en el código (se citan archivo y línea). Todo lo que se afirma sobre Polymarket proviene de la documentación oficial o de respuestas en vivo de la API pública, con la fecha indicada. Las condiciones de los programas (tasas, pesos, recompensas por mercado) cambian sin aviso: este documento es una foto, no un contrato. Antes de armar cualquier cosa, re-verifique con las consultas de la sección 9.

---

## 1. Resumen ejecutivo

1. **El bot es un motor de ejecución *taker*, no un *market maker*.** Siempre cruza el spread (`top.ask`/`top.bid` redondeado al tick) y solo sabe hacer `POST /order`; no hay cancelación, ni consulta de órdenes abiertas, ni `post-only`
   (`core/src/execution_engine.hpp:128-134`, `core/src/lightweight_client.hpp:40`).
2. **El bot no genera la ventaja: la consume.** Su "alpha" entra por HTTP con `p_win`, `confidence`, `q_value` y `direction_hint`
   (`alpha/crowdintel/alpha_parser.hpp:37-47`). Sin un productor de señales que estime probabilidades, no hay dinero. Esto es decisivo para elegir "campaña".
3. **De los seis programas que ofrece Polymarket, solo uno encaja de forma directa con este código hoy: el Taker Rebate Program**, y dentro de él la categoría **Crypto pesa 2.3, la más alta de la tabla** (Sección 3).
4. **Sí, existió una campaña de Bitcoin y era la que mejor le calzaba al perfil del bot: "Crypto TWAP Rewards"**, un fondo de **$1M** repartido entre mercados cripto de 5m/15m/4h que resuelven por TWAP, con **BTC llevándose la mayor tajada ($300k en 5m, $225k en 15m, $50k en 4h)**. **Esa campaña terminó en agosto de 2026** y la documentación oficial lo dice explícitamente. Si a usted le hablaron de ella entre junio y agosto, es exactamente esa. Hoy no paga.
5. **Lo que sí sigue en pie en Bitcoin** es el complejo **"BTC Up or Down" / "Bitcoin above ___"** (5m, 15m, 1h, 4h, diario, semanal, mensual), y es el mejor mercado del universo Polymarket para *este* bot por cinco razones concretas: oráculo externo verificable (Binance / Chainlink) con `priceToBeat` publicado, tick `0.001`/`0.01` soportado, tamaño mínimo 5 shares, comisión cripto 0.07 idéntica al default del repo y cadencia de eventos de 24/día.
6. **Pero hoy el bot no puede operar esas ventanas rotativas** (5m/15m/1h): está cableado a **un solo mercado/token por proceso** (`core/src/market_config.hpp:34-38`) y el sistema **no se reinicia a propósito** porque no existe reconciliación de fills (`docs/STATUS.md`). Lo alcanzable *tal cual está* son los mercados BTC de vida larga (diarios/semanales de strike), y lo ideal tras dos desarrollos: rotación de ventanas + canal privado de usuario (Sección 7).
7. **Con los topes actuales ($1.000 banco, Kelly 0.10, $100/orden, $250 exposición) los umbrales de platino/diamante del programa de rebates son inalcanzables.** A precio 0.50, llegar a Gold (18%) exige ~$174.000 de volumen taker y las rebajas **no son retroactivas**. Con este capital, la rebaja es un *descuento de costo*, no una fuente de ingresos.

---

## 2. Radiografía del bot (lo que realmente puede y no puede hacer)

### 2.1 Capacidades verificadas

| Capacidad | Evidencia | Consecuencia estratégica |
|---|---|---|
| Órdenes de mercado FAK/FOK (default `FAK`), también GTC/GTD en config | `execution_engine.hpp:166`, `market_config.hpp:41` | Aunque se configure GTC/GTD, el precio se calcula **cruzando** el libro (`:128-131`): sigue siendo taker. No hay camino maker ni `post-only`. |
| Ingesta de alpha por HTTP con bearer, campos `p_win`/`confidence`/`q_value`/`direction_hint` | `alpha_parser.hpp:37-66` | El bot monetiza *cualquier* señal con probabilidad; la calidad de la señal es todo. |
| Kelly exacto para binarios + fracción + topes | `alpha/strategy/kelly_engine.hpp:29-56` | Sizing correcto para mercados tipo "paga $1 si ocurre". |
| Gate de comisión con la fórmula oficial del venue | `execution_engine.hpp:248-252` (`fee = feeRate·p·(1−p)`) | Coincide **exactamente** con la fórmula vigente de Polymarket (Sección 3). |
| Dedupe Bloom de dos épocas, TTL de señal 2 s, edad máxima de libro 3 s | `execution_engine.hpp:268-300`, `market_config.hpp:59-61` | Diseñado para señales de horizonte corto y repetitivo. |
| Pool de firmas pre-generadas (8 buckets por lado, TTL 3 s) | `presigned_pool.hpp` | Latencia de firma fuera del camino crítico: apto para ventanas que se repiten. |
| Ticks soportados: 0.1, 0.01, 0.005, 0.0025, **0.001**, 0.0001; tick desconocido falla cerrado | `polymarket_order.hpp:142-152` | Los mercados BTC en 0.001 **sí** están soportados. |
| Tick dinámico desde el WSS | `order_book.hpp:118-125` | Acepta cambios de tick en vivo. |

### 2.2 Límites que definen qué estrategia es posible

| Límite | Evidencia | Qué descarta |
|---|---|---|
| **Un solo mercado por proceso**: `market_slug`, `token_id_dec`, `market_hash` únicos; el WSS se suscribe a un solo `asset_id`; el parser de alpha valida contra un hash de mercado | `market_config.hpp:34-38`, `ws_market_listener.hpp:166-167`, `alpha_parser.hpp:19-33` | Las series que **rotan de mercado cada 5/15/60 minutos** (BTC 5m/15m/1h) requieren un proceso nuevo por ventana → hoy imposible en caliente. |
| **Sin canal privado de usuario ni reconciliación** de órdenes/fills/balances/allowances | `docs/STATUS.md` (bloqueos 1-2), `docs/DEPLOYMENT.md` §Gate | No se sabe si una orden se llenó. Las reservas locales **no** se acreditan como inventario. Imposible operar desatendido. |
| **`Restart=no`** deliberado y reactivación manual | `docs/DEPLOYMENT.md` §6 | No se puede rotar de ventana reiniciando el proceso. Es una decisión de seguridad, no un pendiente estético. |
| **Sin cancelación** de órdenes | solo se usa `POST /order` (`lightweight_client.hpp:40`, `:89`) | Cualquier programa que pague por *liquidez en reposo* (Liquidity Rewards) o por *fills de maker* (Maker Rebates) es inalcanzable sin desarrollo. |
| **Tipo de firma 3 (deposit wallet / ERC-7739) falla cerrado** | `docs/REMEDIATION_STATUS.md` obs. 19 | Si su cuenta de Polymarket usa *deposit wallet*, el bot **no puede firmar** para ella. Solo tipos 0/1/2. |
| Topes de riesgo: banco $1.000, Kelly 0.10, $100/orden, $250 exposición, $50 pérdida diaria, `min_edge` 0.02, `min_confidence` 0.85, `max_q` 0.05, mínimo 5 shares | `market_config.hpp:48-58` | Techo de volumen diario muy bajo → irrelevante para escalar tiers de rebate (Sección 6.4). |

---

## 3. El catálogo real de "campañas" de Polymarket (4-oct-2026)

Polymarket no las llama campañas: son **programas de incentivos**. Son seis y son independientes entre sí.

### 3.1 Taker Rebate Program — *para quien cruza el spread*

- **Vigente desde el 28-may-2026.** Paga en pUSD cada día a medianoche UTC; mínimo acumulado **$1**.
- Fórmula de volumen ponderado:
  `wV = Tamaño de la operación ($) × (1 − Precio de entrada) × Peso de categoría × Bonos`
- **Pesos de categoría: Crypto = 2.3 (el más alto)**, Economics/Culture/Weather/Other = 1.7, Politics/Finance/Tech/1.3, Sports = 1.0, **Geopolitics = 0** (y sin comisión, pero no genera wV).
- Tiers por wV de los últimos 30 días:

| Tier | wV 30 días | Rebaja | Bono de ascenso |
|---|---:|---:|---:|
| Bronze | $2.000 | 3% | $10 |
| Silver | $20.000 | 8% | $50 |
| Gold | $200.000 | 18% | $250 |
| Platinum | $1.000.000 | 32% | $1.500 |
| Diamond | $4.000.000 | 44% | $7.500 |
| Obsidian | $10.000.000 | 50% | $25.000 |

- **La rebaja aplica solo desde el momento en que alcanza el tier: no hay retroactividad.** Subir cuesta comisiones completas.
- Solo cuentan operaciones **taker**. El *market making* no genera wV.

### 3.2 Maker Rebates Program — *para quien pone liquidez y es ejecutado*

- Financiado con las comisiones de taker. **Crypto: 20%** (Finance/Politics/Econ/Culture/Weather/Other/Tech/Mentions: 25%; Sports: 15%).
- Reparto "fee-curve weighted": `fee_equivalent = C × feeRate × p × (1−p)`; su rebaja es su porción del total del mercado. Pago diario, mínimo $1.
- **Requiere ser maker ejecutado** → incompatible con el bot actual.

### 3.3 Liquidity Rewards — *para quien mantiene órdenes en reposo*

- Pago diario; mínimo **$1 por día** (los días por debajo no se acumulan).
- Muestreo del libro **una vez por minuto en offset aleatorio** (hasta 1.440 muestras/día), puntaje cuadrático por orden: `S(v,s) = ((v−s)/v)² × b`, con `v` = `rewardsMaxSpread` del mercado y `s` = distancia al punto medio ajustado por tamaño.
- `Q_min` premia dos lados; con punto medio en [0.10, 0.90] el un lado puntúa dividido por `c = 3.0`; fuera de ese rango **hace falta dos lados** para puntuar.
- Parámetros por mercado: `rewardsMinSize` (shares) y `rewardsMaxSpread` (centavos), más las asignaciones fechadas `clobRewards[].rewardsDailyRate`.
- **Requiere ciclo de vida de cotizaciones** (colocar, repreciar, cancelar) → hoy el bot no puede.

### 3.4 Crypto TWAP Rewards — la campaña de Bitcoin (terminada)

- Asignación extraordinaria de **$1.000.000** para sostener liquidez en mercados cripto que liquidan por TWAP (5m, 15m, 4h), durante **agosto de 2026**:

| Ventana | Pool | BTC |
|---|---:|---:|
| 5 minutos | $550k | **$300k** |
| 15 minutos | $350k | **$225k** |
| 4 horas | $100k | **$50k** |
| (resto: SOL/ETH/HYPE/XRP y BNB/DOGE) | | |

- **La documentación oficial dice: "This program has ended. The August allocation is no longer active."**

### 3.5 Holding Rewards — *para quien mantiene posición*

- Remuneración anualizada sobre el valor de la posición, muestreada por hora, pagada diaria. Reportada por terceros en ~3.25% anual (tasa discrecional de Polymarket).
- Se activa por mercado (`holdingRewardsEnabled`). **En los mercados BTC consultados el 4-oct-2026 aparece `false`** → no aplica ahí.
- Además es lo contrario a la filosofía del bot: inmoviliza capital y no rota inventario.

### 3.6 Referral Program y Builder Program

- Comisiones por referidos (10% directo / 5% indirecto de las comisiones netas generadas) y tarifas de builder por enrutar órdenes.
- No son estrategias de trading: son **negocios de distribución**. Ninguno explota el diseño del bot. (Nota: los integradores con *omnibus wallets* quedan excluidos del Taker Rebate.)

---

## 4. Matriz de encaje: programa × bot

Escala 0–5 (5 = encaja tal cual está hoy).

| Programa | Encaje | Por qué | Trabajo faltante |
|---|:--:|---|---|
| **Taker Rebate (Crypto)** | **5/5** | El bot es taker puro y crypto pesa 2.3. Ya paga comisiones idénticas (`feeRate` 0.07). Acredita wV automáticamente. | Ninguno técnico. Solo elegir mercado cripto con comisión habilitada. |
| **Direccional taker en BTC (sin programa, PnL puro)** | **4/5** | `1` mercado + oráculo externo + `priceToBeat` publicado + tick 0.001 + mínimos de 5 shares. Es su terreno natural. | Productor de alpha para BTC; canal privado + reconciliación para desatenderlo. |
| **Liquidity Rewards (cripto/BTC)** | **1/5** | Paga por órdenes en reposo dentro de la banda; el bot siempre cruza y no puede cancelar. | Módulo maker completo: `post-only`, cancel/replace, refresco de quotes, modelo de puntaje, sesgo por inventario. |
| **Maker Rebates** | **1/5** | Igual que el anterior, más ejecución como maker. | Lo mismo, más gestión de fills. |
| **Holding Rewards** | **0/5** | Exige mantener posición; el bot rota inventario y no puede reconciliarlo. Deshabilitado en los mercados BTC consultados. | Cambio de estrategia, no de código. |
| **Referral / Builder** | **0/5** | No es una estrategia de ejecución. | Nada que el bot pueda hacer. |
| **Geopolitics (libre de comisión)** | **0/5** | Sin comisión pero **sin wV** y sin incentivo: cero aporte al programa y liquidez pobre. | — |

---

## 5. El caso Bitcoin, en detalle

### 5.1 Qué le dijeron y qué era realmente

"Había uno de Bitcoin donde el bot encajaba perfecto" = **Crypto TWAP Rewards**, la inyección de $1M de agosto de 2026 destinada a sostener liquidez mientras Polymarket migraba esos mercados a resolución por TWAP. BTC fue el activo más beneficiado. **Encajaba "perfecto" solo en la parte de recompensa por liquidez, que es precisamente la parte que este bot no puede cobrar**: es un programa *maker*, y el bot es *taker*. Lo que el bot sí podía hacer con esa campaña era cruzarla (pagar comisión), no cobrarla.

**Estado hoy:** la campaña terminó. Los mercados siguen existiendo y siguen teniendo parámetros de recompensa configurados (`rewardsMinSize` 50 shares, `rewardsMaxSpread` 4.5¢ en los BTC consultados), pero en las consultas del 4-oct-2026 el mercado BTC diario mostraba `rewards: {rates: null, ...}` en el endpoint CLOB: **sin asignación financiada activa**. Es decir: hoy Bitcoin no es una cosecha de rewards; es un mercado para *operar*.

### 5.2 Los mercados BTC verificados en vivo (2026-10-04 ~04:55 UTC)

| Mercado (serie) | Tick | Spread | Liquidez | Volumen | Resolución | Comisión |
|---|---:|---:|---:|---:|---|---|
| `btc-up-or-down-hourly` — "Bitcoin Up or Down - Oct 4, 1AM ET" | 0.01 | 1¢ (0.50 / 0.51) | ~$15.5k | — | Vela 1m Binance BTC/USDT: cierre ≥ apertura ⇒ **Up** | crypto 0.07, solo taker, rebate maker 20% |
| `btc-up-or-down-hourly` — ventana 12AM ET (cerrando) | **0.001** | 1¢ (0.03 / 0.04) | ~$2.8k | $14.4k | idem; `eventMetadata.priceToBeat = 84,840.88` | idem |
| `btc-up-or-down-daily` | 0.01 | 1¢ | ~$40.8k | ~$39.8k/24h | Vela 1m Binance a las 12:00 ET vs. día anterior (empate ⇒ 50-50) | idem |
| "Bitcoin above $74,000 on Oct 4" (strike diario) | **0.001** | 0.1¢ | ~$116.9k | ~$319.8k/24h | Vela 1m Binance BTC/USDT 12:00 ET | idem |
| BTC 5m / 15m / 4h | — | — | — | — | **TWAP de Chainlink** (la página del 5m referencia `btc-usd-twap-60s-streams`) | idem |

Notas operativas verificadas: `orderMinSize` = 5 shares, `secondsDelay` = 0 (sin retardo de*matching*), `feesEnabled` = true con `feeSchedule {rate: 0.07, takerOnly: true, rebateRate: 0.2}`, y los mercados de 5m/15m **resuelven contra un TWAP, no contra el último precio**: un alpha que mire spot y no replique el oráculo se equivoca sistemáticamente.

### 5.3 Por qué Bitcoin encaja con *este* bot (y no con cualquier bot)

1. **La ventaja es verificable desde afuera.** El resultado depende de Binance (o Chainlink). El bot no necesita "adivinar" el mundo: necesita estimar `P(cierre ≥ apertura)` mejor y antes que el libro. Eso es exactamente lo que su arquitectura de 2 s de TTL de señal, firmas pre-generadas y libro L2 ya soporta.
2. **`priceToBeat` viene publicado en la metadata del evento** (`eventMetadata.priceToBeat`), es decir, el umbral exacto está disponible por API: el productor de alpha no tiene que inferirlo.
3. **La categoría cripto es la que más wV genera (2.3)** y el factor `(1−p)` significa que comprar el lado barato acumula wV más rápido por dólar (Sección 6.4).
4. **La comisión cripto (0.07) es exactamente el default del repo** (`market_config.hpp:53`) y la fórmula del venue es idéntica a `net_edge()` del motor: no hay sorpresa de costos.
5. **Tick 0.001 y mínimo de 5 shares**: ambos soportados (`polymarket_order.hpp:142-152`, `min_size_shares` = 5).

### 5.4 El punto incómodo: la comisión es grande justo en el centro

`fee_por_share = 0.07 · p · (1−p)`. Con `min_edge` por defecto en 0.02 (2¢/share):

| Precio `p` | Comisión por share | Comisión como % del nocional | Margen neto si el edge es exactamente 2¢ |
|---:|---:|---:|---:|
| 0.50 | 1.75¢ | **3.50%** | **+0.25¢/share** (0.5% del nocional) |
| 0.80 | 1.12¢ | 1.40% | +0.88¢ (1.1%) |
| 0.90 | 0.63¢ | 0.70% | +1.37¢ (1.5%) |
| 0.95 | 0.33¢ | 0.35% | +1.67¢ (1.8%) |
| 0.10 | 0.63¢ | 6.30% | +1.37¢ (13.7%) |
| 0.03 | 0.20¢ | 6.80% | +1.80¢ (60%) |

Lectura: **a 50¢ el default del repo deja 0.25¢ de colchón; cualquier slippage o error de modelo lo borra.** En los extremos la comisión es casi despreciable *por share* y el mismo umbral deja mucho más aire. Recomendación: elevar `BOT_MIN_EDGE` cuando el precio esté cerca de 0.5 (p. ej. 0.03–0.04) y mantener 0.02 en los extremos.

---

## 6. Dónde estaría el dinero, en orden de realidad

### 6.1 Nivel A — *hoy*, con supervisión humana: BTC de vida larga

**Mercados "Bitcoin above ___" del día y "BTC Up or Down Daily".** No rotan (viven 24 h), tienen tick 0.001 en muchos strikes, libros de $40k–$117k y volumen de $40k–$320k/día, y el oráculo es la vela de 1 minuto de Binance a las 12:00 ET. El bot puede quedarse apuntado a uno de ellos sin necesitar rotación. El rebate de taker (3% Bronze al principio) reduce parte de la comisión.

**Por qué es el mejor punto de partida:** es el único escenario donde el software *tal como está* puede participar y donde además se cumple el objetivo del repositorio (canary supervisado, `docs/DEPLOYMENT.md` §5). Con una cuenta de tipo 0/1/2.

### 6.2 Nivel B — *tras dos desarrollos*: BTC horario (el punto dulce)

24 ventanas/día, cada una con su `priceToBeat`, libros de $3k–$16k y spreads de 1¢. Requiere:
1. **Rotación de ventanas** (multi-mercado + arranque del siguiente token sin reiniciar), y
2. **Canal privado + reconciliación** para poder reiniciar/rotar sin exponerse a duplicar órdenes.

Es la mejor relación cadencia/complejidad de todo el catálogo para un bot de latencia con alpha de oráculo. Las ventanas de 15m/5m son el siguiente escalón, pero agregan un problema doble: rotación cada 5–15 minutos y oráculo TWAP de Chainlink que hay que replicar exactamente.

### 6.3 Nivel C — *tras volverse maker*: Liquidity Rewards en cripto

El techo teórico más alto del catálogo (así se repartieron $1M en agosto). Pero exige un módulo que hoy no existe: cotizar `post-only` en ambos lados dentro de `rewardsMaxSpread`, refrescar antes de que el muestreo de cada minuto lo pille fuera de banda, modelar el puntaje `((v−s)/v)²·b`, sesgar por inventario y cancelar/reemplazar (el bot solo tiene `POST /order`). Con el nivel de competencia que se observa en BTC (libros a 1¢ del medio, es decir, ya dentro de la banda de 4.5¢), el reparto sería muy diluido salvo en ventanas concretas.

### 6.4 Lo que dicen los números del rebate (por qué no es una mina de oro con este capital)

A precio 0.50, `wV` por dólar operado = `1 × 0.5 × 2.3 = 1.15`. Con los topes actuales ($100/orden, $250 de exposición, $50 de pérdida diaria):

| Objetivo | wV necesario | Volumen taker @0.50 | Comisiones pagadas por el camino (0.07 cripto) | Rebaja a partir de ahí |
|---|---:|---:|---:|---:|
| Silver | $20.000 | ~$17.400 | ~$609 | 8% |
| Gold | $200.000 | ~$174.000 | ~$6.090 | 18% |
| Platinum | $1.000.000 | ~$870.000 | ~$30.400 | 32% |

Y aun llegando, la rebaja aplica **solo hacia adelante**. Traducción: con un banco de $1.000 y un tope de pérdida diaria de $50, escalar tiers con volumen no es un plan de negocio; es pagar comisiones para comprar un descuento futuro. **El rebate debe tratarse como reducción de costo de una estrategia que ya es rentable por alpha**, nunca como la estrategia.

> **Advertencia explícita:** la documentación de Polymarket se reserva el derecho de ajustar o retirar rebajas por *wash trading*, auto-cruce (*self-matching*) u operación no auténtica. Generar volumen con ida y vuelta sin riesgo no es una estrategia: es una violación de términos con riesgo de perder el acceso. Todo lo de este documento asume operaciones direccionales con criterio.

### 6.5 Ranking final

1. **Direccional taker en BTC (up/down diario y strikes "above") + Taker Rebate cripto.** Único 5/5 real. Nivel A hoy.
2. **BTC horario** (mismo stack, 24× cadencia). Nivel B tras rotación + reconciliación.
3. **BTC 5m/15m.** Nivel B+ (rotación agresiva + alpha TWAP Chainlink).
4. **Liquidity Rewards / Maker Rebates cripto.** Nivel C: techo más alto, requisito: convertirse en maker.
5. **Holding Rewards / Referral / Geopolitics.** No aplican.

---

## 7. Trabajo concreto para llegar a los niveles B y C

| # | Trabajo | Archivos/áreas | Por qué |
|---:|---|---|---|
| 1 | **Rotación de mercados**: token/slug/hash por ventana, WSS multi-activo o re-suscripción limpia, pre-calentado del pool de firmas al abrir la ventana | `market_config.hpp:34-38`, `ws_market_listener.hpp:166`, `alpha_parser.hpp:19-33`, `presigned_pool.hpp` | Sin esto, 5m/15m/1h son imposibles sin reiniciar. |
| 2 | **Canal privado de usuario + reconciliación** (órdenes, fills, balances, allowances) por REST al inicio y continuo por WSS | `docs/STATUS.md` bloqueos 1-2; el evento `user` (topic `order`/`trade`) ya está documentado por Polymarket | Es el bloqueo de producción declarado por el propio repo. Habilita reinicios/rotaciones seguras. |
| 3 | **Verificar tipo de cuenta**: si es *deposit wallet* (tipo 3), el bot no puede firmar hoy | `market_config.hpp` (`BOT_SIGNATURE_TYPE`), `docs/REMEDIATION_STATUS.md` obs. 19 | Sin esto, todo lo demás es teoría. |
| 4 | **Productor de alpha para BTC** (fuera del repo C++): websocket de Binance BTC/USDT + vol realizada/implícita → `p_win` para el resultado de la ventana, POST a `/signal` con `market`, `direction_hint` y `signal_id` estable | `alpha/crowdintel/alpha_parser.hpp` (contrato de entrada) | El bot no predice; esto es el 80% del resultado. Debe replicar el oráculo exacto (Binance 1m; TWAP Chainlink donde aplique). |
| 5 | **Calibrar riesgo al mercado**: `BOT_MIN_EDGE` por banda de precio, `BOT_MIN_SIZE_SHARES=5`, `BOT_INITIAL_POSITION_SHARES` reconciliado, política de inventario al cierre de ventana (vender o mantener a resolución) | `market_config.hpp:48-61` | El default de 2¢ es marginal a 0.50 (Sección 5.4). |
| 6 | **Módulo maker (solo Nivel C)**: `post-only`, cancel/replace por batch, refresco periódico, estimador del puntaje de rewards, sesgo por inventario | nuevo; base en `polymarket_order.hpp` (ya soporta GTC/GTD y ticks) | Requisito de Liquidity Rewards y Maker Rebates. |
| 7 | **Higiene documental**: el repo habla de USDC/collateral; el venue migró a **pUSD** y las recompensas se pagan en pUSD | `README.md`, `docs/*` | Evitar suposiciones de balance/allowance desactualizadas en el preflight. |
| 8 | **Riesgo operativo nuevo**: reinicios del *matching engine* y modo *post-only* posterior | `docs.polymarket.com/trading/matching-engine` | Afecta a cualquier bot que intente ser maker. |

---

## 8. Plan de validación sin riesgo (antes de cualquier dólar)

1. **Replay**: usar `tests/replay/l2_backtester.cpp` recordando su límite declarado: marca a siguiente tick, sin simulación de impacto; **no** predice PnL de fills.
2. **Sombra**: WSS real + alpha real + `MockClient`, comparando importes calculados contra una orden generada por el SDK oficial para el mismo mercado (esto ya está en el runbook, `docs/DEPLOYMENT.md` §4).
3. **Canary**: una ventana BTC, una orden FAK al mínimo, cuenta tipo 0, reconciliación externa **después de cada orden**; verificar comisión cobrada y rebate acreditado al día siguiente (mínimo $1 para que se pague).
4. **Medición**: separar latencia de decisión, cola, DNS, TLS, request y ack semántico; el benchmark de CPU del repo no es un SLO.

---

## 9. Cómo re-verificar todo esto en 5 minutos

```bash
# 1) ¿Qué mercados pagan recompensas HOY y cuánto por día?
curl -s "https://clob.polymarket.com/rewards/markets/current?page_size=100" \
  | jq -r '.data[] | [.total_daily_rate, .rewards_min_size, .rewards_max_spread, .condition_id] | @tsv' \
  | sort -rn | head -20

# 2) Ficha de un mercado BTC concreto (tick, mínimo, comisión, recompensa)
curl -s "https://clob.polymarket.com/markets/<condition_id>" \
  | jq '{question, minimum_tick_size, minimum_order_size, rewards, seconds_delay}'

# 3) Mercado horario en vivo, con el "precio a vencer" del oráculo
curl -s "https://gamma-api.polymarket.com/markets/slug/bitcoin-up-or-down-october-4-2026-1am-et" \
  | jq '{question, orderPriceMinTickSize, orderMinSize, spread, liquidityNum,
         rewardsMinSize, rewardsMaxSpread, feeSchedule,
         priceToBeat: .events[0].eventMetadata.priceToBeat}'

# 4) Escaneo con ranking (mismo repo)
python3 tools/rewards_scan.py --self-test        # valida el parser sin red
python3 tools/rewards_scan.py --live --grep btc  # requiere red saliente
```

Al confirmar el encaje, compruebe **en este orden**: (a) tipo de cuenta/firma, (b) `feesEnabled`/`feeSchedule` del mercado elegido, (c) `orderPriceMinTickSize` y `orderMinSize`, (d) fuente de resolución exacta (Binance 1m vs. TWAP Chainlink), (e) `rewards.rates` si espera cobrar liquidez.

---

## 10. Fuentes

**Documentación oficial (consultada 2026-10-04):**
`docs.polymarket.com/programs/taker-rebates`, `/programs/maker-rebates`, `/programs/liquidity-rewards` (incluye el anuncio de Crypto TWAP Rewards y su finalización), `/trading/fees`, `/trading/market-making`, `/trading/realtime-order-updates`, `/market-data/market-details`, `/trading/matching-engine`.

**API pública en vivo (2026-10-04, ~04:55 UTC):**
`clob.polymarket.com/rewards/markets/current`, `clob.polymarket.com/markets/{condition_id}`, `gamma-api.polymarket.com/markets/slug/{slug}`, `gamma-api.polymarket.com/events?tag_slug=crypto`, `gamma-api.polymarket.com/public-search`.

**Código de este repositorio:** `core/src/execution_engine.hpp`, `core/src/market_config.hpp`, `core/src/polymarket_order.hpp`, `core/src/lightweight_client.hpp`, `core/include/order_book.hpp`, `core/src/ws_market_listener.hpp`, `alpha/strategy/kelly_engine.hpp`, `alpha/crowdintel/alpha_parser.hpp`, `docs/STATUS.md`, `docs/DEPLOYMENT.md`, `docs/REMEDIATION_STATUS.md`.
