# Auditoría técnica profunda — Bot CrowdIntel

_Fecha: 2026-09-26_  
_Alcance: revisión estática del checkout actual de `Adlgr87/Bot_Crowdintel` en la rama de trabajo de Arena. No se ejecutó la suite completa en este sandbox porque no hay CMake/libsecp256k1 preparados aquí; las observaciones se basan en lectura de código, documentación, estructura de build y rutas críticas._

## 0. Resumen ejecutivo

El proyecto tiene un núcleo técnico muy por encima de un bot promedio: C++20, separación hot path/cold path, fixed-point, SPSC, seqlock, firma EIP-712 V2, pruebas criptográficas, benchmark reproducible y documentación honesta. La dirección arquitectónica es buena.

Aun así, hoy lo clasificaría como **prototipo avanzado / motor de ejecución experimental**, no como sistema listo para operar fondos sin más hardening. Los puntos más importantes son:

1. **El feed de señales CrowdIntel no está cableado al runtime live.** Existe `AlphaParser`, pero no hay servidor webhook ni thread live que empuje señales a la cola `SPSC`.
2. **El `PresignedOrderPool` tiene un problema de concurrencia/publicación.** `built_count_` no se publica de forma atómica junto con el buffer activo; además puede servir la misma orden pre-firmada más de una vez.
3. **El parser WSS usa funciones C sobre buffers no necesariamente terminados en `\0`.** Eso puede leer fuera del frame WebSocket.
4. **La publicación del libro L2 no es atómica entre bids y asks.** El motor puede leer una combinación de bids nuevos con asks viejos, aunque el seqlock diga que el snapshot es consistente.
5. **Faltan controles de riesgo de producción:** balance/allowance, inventario, exposición máxima, órdenes abiertas, fills/user-channel, kill-switch, stale-signal/stale-book checks.
6. **Hay un bug funcional en `direction_hint`:** `0 = buy` no se respeta; solo `1 = sell` fuerza dirección, cualquier otro caso deja decidir al motor.
7. **Hay un overflow potencial en `base64url_decode`:** el decodificador no recibe capacidad del buffer destino; la validación de tamaño ocurre después de escribir.
8. **El modelo de pool pre-firmado probablemente tendrá baja tasa de acierto real** porque exige match exacto de `(side, price, size)`, y el tamaño Kelly varía con cada señal.

Mi recomendación: antes de cualquier prueba con dinero real, hacer una fase P0 de corrección de seguridad/correctitud, luego una fase P1 de integración live en modo shadow/paper, y solo después optimización fina de latencia.

---

## 1. Severidad y prioridades

Uso esta escala:

| Prioridad | Significado |
|---|---|
| **P0** | Bloquea dinero real o puede causar bug crítico/memory safety/concurrencia peligrosa. |
| **P1** | Necesario para producción confiable; no siempre bloquea pruebas pequeñas, pero sí operación seria. |
| **P2** | Mejora importante de calidad, observabilidad, performance o mantenibilidad. |
| **P3** | Pulido, documentación, ergonomía, deuda menor. |

---

## 2. Correctitud funcional y arquitectura live

### 2.1 P0 — El feed CrowdIntel no está integrado al modo live

**Evidencia:**

- `alpha/crowdintel/alpha_parser.cpp` define una clase `AlphaParser`, pero no hay header público equivalente.
- `main_hot_path.cpp` en modo live crea `WsMarketListener` para el libro, pero no crea un servidor HTTP/webhook, cliente de cola, lector Kafka/NATS/Redis, ni nada que llene `signals`.
- El único productor de señales real en `main_hot_path.cpp` es `mock_feed`, y solo corre si `BOT_MODE=mock`.

**Impacto:**

En live, el motor puede quedarse indefinidamente en `NO_SIGNAL`. Incluso si el WSS funciona y el libro se actualiza, no habrá triggers reales para enviar órdenes.

**Arreglo recomendado:**

1. Convertir `AlphaParser` en API real:
   - Crear `alpha/crowdintel/alpha_parser.hpp`.
   - Mover declaración allí.
   - Mantener implementación en `.cpp` o volverlo header-only, pero de forma intencional.
2. Crear un componente de ingestión:
   - Opción simple: servidor HTTP local para webhooks.
   - Opción producción: consumidor de NATS/Redis Streams/Kafka.
   - Opción mínima: lector de stdin/socket Unix para pruebas shadow.
3. Validar schema del payload:
   - `market_id` / `token_id` / `slug`.
   - `p_win`.
   - `confidence`.
   - `q_value`.
   - `direction_hint`.
   - `signal_id`.
   - timestamp de origen.
4. Deduplicar por `signal_id`.
5. Rechazar señales que no correspondan al `BOT_TOKEN_ID` configurado.
6. Exponer métricas:
   - señales recibidas,
   - señales rechazadas por schema,
   - señales rechazadas por mercado incorrecto,
   - señales encoladas,
   - señales perdidas por cola llena.

---

### 2.2 P0 — El contrato semántico de `direction_hint` está roto

**Evidencia:**

En `alpha_receiver.hpp`:

```cpp
uint8_t direction_hint; // 0 = buy, 1 = sell, 2 = engine decides
```

Pero en `ExecutionEngine::run_tick()`:

```cpp
if (sig.direction_hint == 1) {
    side = K_SIDE_SELL;
} else {
    // engine decides
}
```

Esto significa que `direction_hint == 0` no fuerza BUY; entra al modo “engine decides”.

**Impacto:**

Una señal explícita de compra puede convertirse en venta si el cálculo de edge relativo favorece SELL. Esto es grave porque viola el contrato del mensaje.

**Arreglo recomendado:**

```cpp
if (sig.direction_hint == 0) {
    side = K_SIDE_BUY;
} else if (sig.direction_hint == 1) {
    side = K_SIDE_SELL;
} else {
    const double buy_edge  = sig.p_win - (double)top.ask.price * 1e-6;
    const double sell_edge = (double)top.bid.price * 1e-6 - sig.p_win;
    side = (buy_edge >= sell_edge) ? K_SIDE_BUY : K_SIDE_SELL;
}
```

Agregar test unitario específico:

- señal `direction_hint=0`, con `sell_edge > buy_edge`, debe producir BUY o rechazo por no edge, pero nunca SELL.
- señal `direction_hint=1`, con `buy_edge > sell_edge`, debe producir SELL o rechazo por no edge, pero nunca BUY.

---

### 2.3 P1 — No hay correlación fuerte señal ↔ mercado/token

**Situación actual:**

`AlphaSignal` trae `market_slug[16]` “debug label only”. El hot path opera siempre el token configurado en `BOT_TOKEN_ID`.

**Impacto:**

Si por error llega una señal de otro mercado, el bot podría operar el mercado configurado usando una probabilidad que no corresponde. Esto es uno de los riesgos funcionales más peligrosos.

**Arreglo recomendado:**

- Añadir al `AlphaSignal` un identificador fuerte:
  - `token_id_hash` de 32 bytes o fingerprint de 64 bits,
  - o `condition_id/outcome_index`,
  - o `market_id` validado contra una tabla cold-path.
- En el hot path, comparar contra un fingerprint precomputado del mercado configurado.
- Rechazar señal si no coincide.

Manteniendo hot path liviano:

```cpp
struct AlphaSignal {
    uint64_t token_fingerprint;
    ...
};
```

El parser cold-path calcula el fingerprint; el hot path hace una comparación `uint64_t`.

---

### 2.4 P1 — Falta lifecycle de órdenes y fills

**Situación actual:**

`LightweightCLOBClient::submit()` extrae `orderID`, pero `ExecutionEngine` no lo usa para tracking. No hay user-channel para fills/cancels/open orders.

**Impacto:**

No sabes si una orden:

- fue aceptada,
- quedó resting,
- fue fill parcial,
- fue fill total,
- fue rechazada por balance,
- debe cancelarse,
- ya aumentó exposición.

**Arreglo recomendado:**

Crear un módulo `OrderManager`:

- `on_submit_attempt(local_id, body_hash, side, price, size)`
- `on_submit_accepted(order_id)`
- `on_submit_rejected(code, reason)`
- `on_fill(order_id, filled_size, avg_price)`
- `on_cancel(order_id)`
- `open_exposure()`
- `market_exposure(token)`

Integrar user-channel de Polymarket para lifecycle.

Para estrategia taker, considerar usar `FOK` o `FAK` en vez de `GTC` si no se desea dejar órdenes resting.

---

### 2.5 P1 — `BOT_TICKS` no ayuda en live si no hay señales

En el loop live, `ticks_done` solo incrementa cuando el resultado no es `NO_SIGNAL`. Si no hay señales, `BOT_TICKS=N` nunca termina.

**Arreglo recomendado:**

Añadir:

- `BOT_MAX_RUNTIME_SEC`,
- `BOT_MAX_IDLE_SEC`,
- modo `--wss-soak` donde `BOT_TICKS=0` es explícito,
- métricas de idle.

---

## 3. Concurrencia, memoria y publicación de datos

### 3.1 P0 — `PresignedOrderPool::built_count_` tiene data race y publicación inconsistente

**Evidencia:**

En `rebuild()`:

```cpp
built_count_ = n;
active_.store(next, std::memory_order_release);
```

En `acquire()`:

```cpp
const auto& slots = buffers_[active_.load(std::memory_order_acquire)];
for (size_t i = 0; i < built_count_; ++i) { ... }
```

`built_count_` no es atómico y no está empaquetado con el buffer activo.

**Problemas concretos:**

1. Data race formal en C++.
2. El consumidor puede leer `built_count_` nuevo mientras todavía ve el buffer activo anterior.
3. Si el nuevo count es mayor, el consumidor puede escanear slots no válidos del buffer viejo.
4. Si el nuevo count es menor, puede dejar de ver slots válidos del buffer actual.

**Arreglo recomendado:**

Usar metadata por buffer y publicar `(index, count, epoch)` de forma atómica.

Ejemplo conceptual:

```cpp
struct PresignedBuffer {
    std::array<PresignedSlot, SLOT_COUNT> slots;
    size_t count;
};

struct PublishedPoolState {
    uint32_t index;
    uint32_t count;
    uint64_t epoch;
};
```

Opciones:

- Empaquetar `index` y `count` en un `std::atomic<uint64_t>`.
- O tener `buffers_[next].count = n;` y publicar solo `active_`; luego el consumidor lee `buffers_[active].count` después del acquire.

Ejemplo simple:

```cpp
struct BufferSet {
    std::array<PresignedSlot, SLOT_COUNT> slots{};
    size_t count = 0;
};

void rebuild(...) {
    uint32_t next = 1u - active_.load(std::memory_order_relaxed);
    auto& b = buffers_[next];
    size_t n = fill(b.slots);
    b.count = n;
    active_.store(next, std::memory_order_release);
}

bool acquire(...) const {
    uint32_t idx = active_.load(std::memory_order_acquire);
    const auto& b = buffers_[idx];
    for (size_t i = 0; i < b.count; ++i) { ... }
}
```

Aun así, para ser formalmente limpio bajo el modelo de memoria C++, conviene asegurarse de que el productor no vuelva a escribir un buffer que el consumidor podría estar leyendo. Con un solo consumidor y doble buffer suele funcionar si el consumidor no retiene referencias, pero para robustez máxima usar triple buffer o epoch/RCU simple.

---

### 3.2 P0/P1 — El pool puede reutilizar la misma orden firmada varias veces

`acquire()` copia el body y no marca el slot como consumido. Si llegan señales idénticas antes del próximo rebuild, puede enviar el mismo `salt + timestamp + body + signature` varias veces.

**Impacto:**

- Rechazos por duplicado.
- Ruido en latencia y métricas.
- Riesgo de comportamiento inesperado si el exchange trata duplicados de forma no trivial.

**Arreglo recomendado:**

- Semántica one-shot: un slot prefirmado se consume una sola vez.
- Como hay un solo consumidor, se puede marcar `valid = 0`, pero debe evitarse carrera con el productor cuando ese buffer vuelva a ser inactivo.
- Mejor: usar epochs y no reutilizar buffer hasta que el consumidor haya avanzado.

Alternativa más simple:

- Mantener pool como acelerador de firma, pero si un slot fue usado, invalidarlo y que el siguiente trade haga inline sign hasta rebuild.

---

### 3.3 P1 — El diseño actual del pool tiene baja probabilidad de hit real

El pool exige coincidencia exacta de:

```text
side + price + size
```

El precio puede coincidir con la grilla, pero el tamaño viene de Kelly y cambia con `p_win`, `price`, bankroll, edge y liquidez visible. En `main_hot_path.cpp`, el presign thread firma un tamaño aproximado:

```cpp
usd = bankroll * kelly_fraction * 0.2;
shares = usd_to_shares_fixed(usd, mid_price);
```

El engine calcula otro tamaño:

```cpp
k = KellyEngine::kelly_buy/sell(sig.p_win, price)
usd = position_usd(k, kelly_fraction, bankroll)
shares = usd_to_shares_fixed(usd, price)
```

**Impacto:**

En producción, el pool puede fallar frecuentemente y caer a firma inline. El benchmark de pool-hit es válido como microbench, pero representa un caso artificial donde el tamaño está construido para coincidir.

**Arreglo recomendado:**

- Pre-firmar múltiples buckets de tamaño: por ejemplo 5, 10, 25, 50, 100, 250 shares o notional buckets.
- Permitir que el engine seleccione el bucket menor o igual al tamaño deseado.
- Dividir una intención grande en clips prefirmados.
- Medir `pool_hit_rate` real por día/mercado.

Métrica crítica nueva:

```text
pool_hit_rate = pool_hits / (pool_hits + inline_fallbacks)
```

---

### 3.4 P0/P1 — `OrderBookL2` publica bids y asks por separado

`OrderBookL2` tiene `set_bids()` y `set_asks()` separados. `WsMarketListener::parse_book_snapshot()` llama ambos uno tras otro.

**Problema:**

Entre `set_bids()` y `set_asks()`, el seqlock queda par/even. El consumidor puede leer:

- bids nuevos,
- asks viejos,
- `sequence` estable.

El seqlock no detecta que el snapshot lógico completo no corresponde al mismo evento.

**Arreglo recomendado:**

Añadir API de publicación conjunta:

```cpp
void set_book(const Level2Entry* bids, size_t nb,
              const Level2Entry* asks, size_t na) {
    write_begin();
    copy bids;
    copy asks;
    write_end();
}
```

Para deltas, aplicar bids+asks sobre copias locales y publicar ambos lados en una sola sección crítica.

---

### 3.5 P1 — Seqlock con datos no atómicos es UB formal en C++

El patrón seqlock clásico funciona a nivel hardware, pero en C++ leer `bids_` mientras otro thread escribe `bids_` es una data race formal si los campos no son atómicos.

**Opciones:**

1. Aceptarlo como low-level systems code y documentarlo, pero TSAN lo reportará.
2. Cambiar a doble buffer de libro completo:
   - productor escribe buffer inactivo,
   - publica índice atómico,
   - consumidor copia desde buffer activo sin data race.
3. Usar campos atómicos para top-of-book, si el hot path solo necesita top.
4. Mantener L2 completo en cold path, pero publicar un `TopOfBookSnapshot` atómico/doble-buffer para el engine.

Mi recomendación para simplicidad y performance: **doble buffer de snapshot completo o top-of-book**.

---

### 3.6 P1 — `engine_idle_wait()` no reinicia el contador de spin

El contador `spins` es `thread_local` y solo crece. Después de cruzar 20,000 iteraciones, el engine dormirá 100 µs en cada idle futuro, incluso si acaba de procesar trabajo.

**Impacto:**

Puede introducir hasta ~100 µs de latencia adicional para la siguiente señal tras un período idle.

**Arreglo recomendado:**

Pasar un evento de reset cuando se procesa algo:

```cpp
struct IdlePolicy {
    uint32_t spins = 0;
    void on_work() { spins = 0; }
    void on_idle() { ... }
};
```

En el loop:

```cpp
if (r == TickResult::NO_SIGNAL) idle.on_idle();
else idle.on_work();
```

---

### 3.7 P2 — Manejo de threads en WSS es confuso

En `main_hot_path.cpp`:

```cpp
wss_thread = std::thread([&] { listener->start(); });
```

Pero `listener->start()` a su vez crea otro thread interno y retorna. Entonces `wss_thread` no es realmente el thread WSS; es un wrapper efímero.

**Arreglo recomendado:**

- O `WsMarketListener::start()` debe ser bloqueante y correr en el thread externo.
- O `main` debe llamar `listener->start()` directamente sin envolverlo en otro `std::thread`.
- Agregar `listener->stop()` explícito en shutdown antes de imprimir summary.

---

## 4. Parsers, protocolos y memory safety

### 4.1 P0 — Parser WSS usa `strstr/strchr` sobre payload no null-terminated

`read_frames()` llama:

```cpp
handle_message(payload, plen);
```

`payload` apunta dentro de `rbuf_`. No se garantiza `payload[plen] == '\0'`.

Pero `handle_message()`, `parse_book_snapshot()`, `parse_price_change()`, `parse_levels()` y `extract_string()` usan `std::strstr`, `std::strchr` y loops `while (*p ...)`.

**Impacto:**

Puede leer más allá del frame, interpretar bytes de otro frame o memoria residual, y en casos extremos leer fuera de límites.

**Arreglo recomendado:**

- Reemplazar todo uso de `strstr/strchr` por versiones bounded:
  - `memmem` controlado,
  - funciones propias `find(j, len, token)`,
  - scanner con `(ptr, end)`.
- Nunca usar `while (*p)` en payload de red.
- Agregar fuzz tests con frames sin terminador nulo.

Ejemplo de patrón correcto:

```cpp
const char* end = j + len;
const char* p = bounded_find(j, end, "\"bids\"");
if (!p) return 0;
const char* arr = bounded_find_char(p, end, '[');
```

---

### 4.2 P0 — `base64url_decode` puede escribir fuera del buffer destino

`base64url_decode(const char* in, size_t len, uint8_t* out)` no sabe la capacidad de `out`. En `LightweightCLOBClient`:

```cpp
secret_len_ = base64url_decode(cfg.api_secret_b64, slen, secret_raw_);
if (secret_len_ > sizeof(secret_raw_)) { ... }
```

La validación ocurre después de escribir. Si `CLOB_SECRET` es largo, `secret_raw_[64]` puede desbordarse.

**Arreglo recomendado:**

Cambiar firma:

```cpp
size_t base64url_decode(const char* in, size_t len, uint8_t* out, size_t cap)
```

y antes de cada `out[o++]`:

```cpp
if (o >= cap) return SIZE_MAX;
```

Agregar tests:

- input válido de 64 bytes exactos,
- input que decodifica 65 bytes,
- input con padding raro,
- input con caracteres inválidos.

---

### 4.3 P1 — `build_wire_body()` verifica overflow al final, no durante escritura

El builder escribe en `WireBody::buf[1536]` y al final retorna `out.len < sizeof(out.buf)`. Si por cualquier motivo una cadena de entrada supera lo esperado, la escritura ya ocurrió.

Hoy la mayoría de campos vienen de buffers truncados o constantes, así que el riesgo práctico está contenido; pero el patrón no es ideal para código expuesto a configuración.

**Arreglo recomendado:**

Crear un `FixedWriter`:

```cpp
struct FixedWriter {
    char* p;
    char* end;
    bool ok = true;
    void append(const char* s, size_t n) {
        if ((size_t)(end - p) < n) { ok = false; return; }
        memcpy(p, s, n);
        p += n;
    }
};
```

Así el builder no puede escribir fuera aunque el input sea inesperado.

---

### 4.4 P1 — Manejo de fragmentación WebSocket parsea fragmento parcial

En `read_frames()`, para `opcode == 0x1` se llama `handle_message(payload, plen)` antes de verificar si `FIN` está activo. Si es un frame de texto fragmentado, parsea el primer fragmento incompleto.

**Arreglo recomendado:**

- Si `opcode == text && fin`, parsear.
- Si `opcode == text && !fin`, iniciar acumulación, no parsear.
- En continuations, parsear solo al llegar `FIN`.

También conviene validar:

- control frames no fragmentados,
- payload de control <= 125,
- continuation sin fragment abierto = error,
- nuevo text/binary mientras fragmenting = error.

---

### 4.5 P1 — Re-sync del libro y validación de hash/secuencia

El parser acepta snapshots y deltas, pero no se observa control fuerte de secuencia/hash. En feeds de mercado, perder un delta o reconectar sin snapshot puede dejar el libro local inconsistente.

**Arreglo recomendado:**

- Mantener estado `HAS_SNAPSHOT / LIVE / STALE`.
- No operar hasta tener snapshot reciente.
- En reconnect, limpiar libro o marcar stale hasta nuevo snapshot.
- Si el feed trae `hash`, validarlo si la documentación lo permite.
- Añadir `book_update_time_mono_ns` y `book_sequence`.
- Rechazar trade si el libro es más viejo que `BOT_MAX_BOOK_AGE_MS`.

---

## 5. Trading, riesgo y controles financieros

### 5.1 P0 — Falta balance/allowance/inventario antes de sizear

El engine clampa contra liquidez visible, pero no contra:

- USDC disponible,
- allowance al exchange,
- shares disponibles para SELL,
- exposición ya abierta,
- órdenes resting pendientes.

**Impacto:**

- Rechazos live.
- Exposición mayor a la prevista.
- SELL sin inventario suficiente.
- Tamaños Kelly calculados sobre bankroll teórico, no capital realmente disponible.

**Arreglo recomendado:**

Crear `RiskManager` cold/hot split:

Cold path actualiza:

- balances,
- allowances,
- open orders,
- fills,
- realized/unrealized PnL.

Hot path consulta snapshot inmutable:

```cpp
struct RiskSnapshot {
    uint64_t max_buy_usdc_raw;
    uint64_t yes_inventory_raw;
    uint64_t max_order_usdc_raw;
    uint64_t max_order_shares_raw;
    bool trading_enabled;
};
```

El engine debe rechazar si no hay capacidad real.

---

### 5.2 P0/P1 — No hay límites globales de pérdida y exposición

Recomendados mínimos:

- `BOT_MAX_ORDER_USD`
- `BOT_MAX_MARKET_EXPOSURE_USD`
- `BOT_MAX_DAILY_LOSS_USD`
- `BOT_MAX_POSITION_SHARES`
- `BOT_MAX_ORDERS_PER_MIN`
- `BOT_MAX_CONSECUTIVE_FAILURES`
- kill-switch por archivo/señal/API:
  - si existe `/opt/crowdintel/KILL`, no operar.

El hot path puede leer un atomic `trading_enabled` y límites precomputados.

---

### 5.3 P1 — Stale signal y stale book checks

`AlphaSignal.timestamp_ns` existe, pero no se usa. `OrderBookL2` tampoco guarda timestamp de última actualización.

**Arreglo recomendado:**

- Usar `CLOCK_MONOTONIC` para arrival timestamps internos.
- Añadir `BOT_MAX_SIGNAL_AGE_MS`, por ejemplo 100–500 ms dependiendo de la estrategia.
- Añadir `BOT_MAX_BOOK_AGE_MS`, por ejemplo 250–1000 ms.
- Rechazar si señal/libro están stale.

---

### 5.4 P1 — Tipo de orden y resting risk

El README usa `GTC` por default. Para una estrategia taker que intenta consumir best bid/ask, `GTC` puede dejar órdenes abiertas si el precio no cruza por rounding, race o movimiento del libro.

**Recomendación:**

- Para taker puro: default `FAK` o `FOK`.
- Para maker: módulo separado, con cancel/replace y control de inventario.
- Si se mantiene `GTC`, `OrderManager` debe cancelar órdenes stale.

---

### 5.5 P1 — Tick-size estático y rounding pueden impedir fills

El engine redondea el precio del top of book con `BOT_TICK_SIZE`. Si `BOT_TICK_SIZE` no coincide con el mercado:

- BUY puede redondear hacia abajo y no cruzar el ask.
- SELL puede quedar menos agresivo.
- La orden puede restar en vez de llenar.

**Arreglo recomendado:**

- Fetch de market metadata al inicio:
  - tick size,
  - min order size,
  - status,
  - neg-risk,
  - token ids,
  - closed/resolved.
- Rechazar startup si metadata no coincide con env.
- Actualizar en `tick_size_change`.

---

### 5.6 P1 — Kelly necesita haircuts por incertidumbre y correlación

Kelly exacto está bien como base matemática, pero en producción la probabilidad `p_win` tiene error. Si el modelo está mal calibrado, Kelly sobreapuesta.

**Mejoras:**

- Kelly fraccional dinámico por calidad/calibración.
- Haircut de edge:
  - `effective_p = shrink(p_win, market_mid, alpha)`
  - o restar `model_uncertainty_sigma`.
- Cap por liquidez y slippage.
- Cap por correlación entre mercados/señales.
- Penalizar mercados con spread amplio.

---

## 6. Seguridad

### 6.1 P0 — Secretos y private key vía environment

Leer `BOT_PRIVATE_KEY_HEX` desde env es práctico, pero para producción expone riesgos:

- aparece en `/proc/<pid>/environ` para usuarios con permisos,
- puede filtrarse en dumps,
- queda en shell history si se exporta manualmente,
- systemd EnvironmentFile requiere disciplina operacional.

**Arreglo recomendado:**

- Soportar `BOT_PRIVATE_KEY_FILE` con permisos 0400.
- Soportar systemd `LoadCredential=`.
- Considerar vault/secret manager.
- Hot wallet dedicada con balance limitado.
- Desactivar core dumps:
  - `ulimit -c 0`,
  - `LimitCORE=0`,
  - `prctl(PR_SET_DUMPABLE, 0)`.
- Usar `mlock`/`mlockall` para buffers de secreto si el host lo permite.

---

### 6.2 P1 — `MarketConfig::load()` no limpia su copia local de la private key

Dentro de `MarketConfig::load()` se parsea `key[32]` para derivar signer mediante un `EIP712Signer tmp`, pero ese array local no se limpia explícitamente antes de retornar.

**Arreglo recomendado:**

- Llamar `secure_zero(key, 32)` antes de cada retorno posterior al parse exitoso.
- O refactorizar para que `MarketConfig` no derive signer y el signer principal derive una sola vez.

---

### 6.3 P1 — Fallback de `FastRandom` no es un CSPRNG sembrado con entropía fuerte

Si no hay RDRAND disponible/compilado, `mix_seed()` usa TSC, reloj monotónico y direcciones. Eso no es entropía criptográfica fuerte.

Para salts públicas quizás basta con unicidad práctica, pero el comentario promete CSPRNG y también se usa para randomizar el contexto secp256k1.

**Arreglo recomendado:**

- Sembrar una vez con `getrandom()`/`getentropy()` en startup.
- Mantener cero syscalls en steady state.
- Si falla la entropía fuerte, abortar en producción.

---

### 6.4 P1 — TLS pinning solo para CLOB HTTP, no WSS

`BOT_TLS_PIN` se aplica al cliente libcurl para órdenes. El WSS manual usa OpenSSL hostname verification, pero no pin equivalente.

**Recomendación:**

- Añadir pin opcional para WSS.
- Soportar rotación de pins.
- Loguear fingerprint observado en modo diagnóstico.

---

### 6.5 P1 — Validación de configuración insuficiente

Variables como `BOT_TICK_SIZE`, `BOT_KELLY_FRACTION`, `BOT_MIN_EDGE`, `BOT_MIN_SIZE_SHARES`, `BOT_SIGNATURE_TYPE`, `BOT_ORDER_TYPE` no tienen validación estricta.

**Ejemplos de riesgo:**

- `BOT_TICK_SIZE=-0.01` puede convertirse a `uint64_t` enorme.
- `BOT_KELLY_FRACTION=100` puede causar sizing absurdo aunque luego se capee parcialmente.
- `BOT_ORDER_TYPE` arbitrario se inserta en JSON sin escaping.

**Arreglo recomendado:**

Validar rangos y enums:

```text
0 < tick_size < 1
0 <= kelly_fraction <= 1
0 <= min_edge <= 1
0 <= max_q_value <= 1
0 <= min_confidence <= 1
signature_type ∈ {0,1,2,3}
order_type ∈ {GTC,GTD,FOK,FAK}
```

---

## 7. Red, latencia y cliente HTTP/WSS

### 7.1 P1/P2 — `LightweightCLOBClient` asigna memoria por orden vía `curl_slist_append`

Cada `submit()` crea una lista de headers nueva con `curl_slist_append()` y la libera al final.

**Impacto:**

- Heap allocations por orden.
- Jitter.
- Contradice parcialmente el objetivo “zero allocation” si se considera submit dentro de la ruta crítica.

**Opciones:**

1. Aceptar que red domina y medir si importa.
2. Mantener libcurl pero reducir allocations:
   - reusar handles,
   - separar headers estáticos,
   - explorar `CURLOPT_HEADERFUNCTION` no ayuda para request headers, pero se puede perfilar.
3. Implementar cliente HTTP/1.1 propio sobre OpenSSL/BoringSSL para POST persistente con buffers fijos.
4. Usar kernel TLS / io_uring solo si mediciones lo justifican.

Mi recomendación: no optimizar esto hasta tener correctness/risk resuelto, pero sí medir.

---

### 7.2 P1 — Timeouts y reconexión WSS

`tcp_connect()` en WSS es bloqueante y no tiene timeout explícito. Puede colgar según red/DNS.

**Arreglo recomendado:**

- Resolver DNS con timeout o pre-resolver.
- Socket no bloqueante + `select/poll` para connect timeout.
- Backoff exponencial con jitter.
- Métricas de reconnect reason.

---

### 7.3 P2 — Red real dominará la latencia

Los 90 ns son del hot path interno sin red. En producción, el presupuesto real incluye:

- llegada del feed,
- parsing,
- decisión,
- HMAC,
- write TLS,
- RTT a CLOB,
- cola del matching engine,
- respuesta.

**Siguiente benchmark útil:**

- `signal arrival timestamp` → `write()` al socket.
- `signal arrival` → `HTTP 2xx/reject`.
- `market data event` → `book update visible`.
- WSS soak con p50/p99/p999 de parsing y gap.
- Pool hit-rate real.

---

## 8. Hardware y sistema operativo

### 8.1 P1/P2 — Kernel tuning script es útil pero agresivo

`kernel_tuning.sh` cambia sysctls globales y parámetros de CPU. Algunas recomendaciones:

- Añadir modo dry-run.
- Detectar root y distro.
- Guardar backup antes de modificar `/etc/sysctl.d` o GRUB.
- No asumir cores `2,3`.
- Configurar governor `performance` explícitamente.
- Afinidad de IRQs/NIC al core correcto.
- Separar tuning de VPS vs bare metal.
- Documentar riesgos de `busy_poll` y C-states.

### 8.2 P1 — Time sync

Para órdenes con timestamps y análisis post-mortem, usar:

- chrony mínimo,
- idealmente PTP si aplica,
- métricas de offset.

Aunque CLOB use timestamp en segundos para auth y ms para order uniqueness, un reloj muy desviado puede causar rechazos.

### 8.3 P2 — Deployment geográfico

La latencia real puede mejorar más con ubicación de host que con micro-optimizaciones. Medir desde distintas regiones/VPS:

- RTT a `clob.polymarket.com`,
- estabilidad p99/p999,
- pérdida/retransmisiones,
- handshake reuse.

---

## 9. Build, CI y calidad

### 9.1 P1 — CI sin sanitizers/fuzzing

Agregar jobs:

- ASAN/UBSAN debug.
- TSAN en componentes que puedan soportarlo o tests específicos double-buffer.
- Fuzzing de parsers:
  - WSS JSON,
  - base64,
  - fixed decimal,
  - uint256 decimal,
  - wire response parser.
- `-D_GLIBCXX_ASSERTIONS` en debug.
- `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` gradualmente.

### 9.2 P1 — Build offline no está realmente libre de OpenSSL headers

`test_core.cpp` incluye `ws_market_listener.hpp`, que incluye OpenSSL headers. Aunque no se linkee red, compilar tests puede requerir `libssl-dev`.

**Arreglo recomendado:**

- Separar parser WSS puro de transporte TLS.
- `ws_market_parser.hpp` sin OpenSSL.
- `ws_market_listener.hpp` solo transporte.
- Tests de parser no dependen de OpenSSL.

### 9.3 P2 — Dependencias no totalmente pineadas

CI clona `secp256k1` por tag y `pip install pycryptodome coincurve` sin version pin.

**Recomendación:**

- Pin a commit hash para secp256k1.
- Pin versiones Python.
- Cachear build de secp si se quiere acelerar.
- Considerar submodule o FetchContent con hash.

### 9.4 P3 — README path menor

En README:

```bash
cd core/build
python3 ../tests/crypto/cross_check_v2.py /tmp/signer.json
```

Desde `core/build`, lo más probable es que deba ser:

```bash
python3 ../../tests/crypto/cross_check_v2.py /tmp/signer.json
```

---

## 10. Backtesting e investigación

### 10.1 P1/P2 — `l2_backtester` no modela PnL económico real

El backtester marca PnL inmediato contra mid:

- BUY al ask ⇒ mid - ask suele ser negativo.
- SELL al bid ⇒ bid - mid suele ser negativo.

Esto mide spread/slippage, no valor esperado de la señal ni resolución del mercado.

**Mejoras:**

- Usar serie temporal futura para markout:
  - 1s, 5s, 30s, 5m.
- Usar outcome final si existe.
- Incluir fees si aplican.
- Incluir partial fills.
- Incluir queue position para maker.
- Simular latencia: feed delay + decision + network.
- Medir Brier score/log loss de `p_win`.
- Walk-forward y out-of-sample.

---

## 11. Observabilidad y operación

### 11.1 P1 — Falta logging estructurado de rechazos

Hoy summary cuenta `submitted`, `submit_failed`, `no_edge`, etc. Para producción necesitas saber por qué falló cada orden:

- HTTP code,
- body de error truncado,
- curl error,
- local order id,
- side/price/size,
- señal asociada,
- edad de señal/libro,
- pool hit/miss.

**Diseño recomendado:**

- Hot path no loguea directo.
- Hot path escribe evento compacto en SPSC de auditoría.
- Thread cold serializa JSONL.
- Rotación de archivos.

Ejemplo de evento:

```json
{"ts":...,"event":"order_rejected","http":400,"reason":"insufficient balance","side":"BUY","price":0.53,"size":10,"signal_id":"..."}
```

### 11.2 P1 — Métricas indispensables

- `signals_received_total`
- `signals_rejected_total{reason}`
- `book_updates_total`
- `book_age_ms`
- `orders_submitted_total`
- `orders_rejected_total{reason}`
- `pool_hits_total`
- `pool_misses_total`
- `pool_hit_rate`
- `inline_sign_latency_ns`
- `submit_latency_ms`
- `wss_reconnects_total`
- `open_orders`
- `exposure_usd`
- `daily_pnl`
- `kill_switch_active`

---

## 12. Docker/systemd/deploy

### 12.1 P1 — systemd corre como root

`deploy_production.sh` instala un unit sin `User=`. Por defecto corre como root.

**Arreglo recomendado:**

- Crear usuario dedicado `crowdintel`.
- `User=crowdintel`, `Group=crowdintel`.
- `NoNewPrivileges=true` ya está bien.
- Añadir:
  - `PrivateDevices=true`, si no rompe necesidades.
  - `ProtectKernelTunables=true`.
  - `ProtectKernelModules=true`.
  - `RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX`.
  - `LockPersonality=true`.
  - `MemoryDenyWriteExecute=true`, probar compatibilidad con libsecp/OpenSSL.
  - `LimitCORE=0`.

### 12.2 P1 — Secrets en EnvironmentFile

Mejor usar systemd credentials:

```ini
LoadCredential=bot_private_key:/etc/crowdintel/private_key
LoadCredential=clob_secret:/etc/crowdintel/clob_secret
```

y que la app soporte rutas `*_FILE`.

### 12.3 P2 — Docker runtime como root

El Dockerfile runtime no crea usuario no-root. Añadir:

```dockerfile
RUN useradd -r -u 10001 crowdintel
USER crowdintel
```

También considerar healthcheck y labels de versión.

---

## 13. Roadmap recomendado

### Fase 0 — P0 antes de dinero real

1. Arreglar `direction_hint`.
2. Arreglar `base64url_decode` con capacidad.
3. Hacer parser WSS bounded, sin `strstr/strchr` sobre payload no terminado.
4. Publicar bids+asks en una sola operación atómica/lógica.
5. Corregir `PresignedOrderPool`:
   - count por buffer,
   - publicación coherente,
   - evitar reuso de slot.
6. Integrar feed de señales live o dejar claro que aún no existe.
7. Añadir stale checks para señal y libro.
8. Validar configuración con rangos estrictos.

### Fase 1 — Producción mínima segura

1. `RiskManager` con balance/allowance/inventario/exposición.
2. `OrderManager` con order IDs, open orders, fills y cancels.
3. User-channel.
4. Fetch market metadata al startup.
5. Modo `shadow`:
   - recibe señales,
   - actualiza libro,
   - calcula decisión,
   - no envía orden.
6. Primera orden real con tamaño mínimo, preferentemente `FOK/FAK`.
7. Logs estructurados y métricas.

### Fase 2 — Latencia y robustez

1. Medir latencia real end-to-end.
2. Optimizar pool con buckets de tamaño.
3. Medir y reducir allocations de submit.
4. Reconsiderar libcurl vs cliente HTTP propio solo si el profiler lo justifica.
5. WSS reconnect/resync robusto.

### Fase 3 — Operación profesional

1. systemd non-root + credentials.
2. Hardening de secrets con mlock/no dumps.
3. Fuzzers y sanitizers en CI.
4. Runbooks de incidentes.
5. Backtesting real con markouts/outcomes.
6. Calibración estadística continua de señales.

---

## 14. Lista corta de “quick wins”

1. Corregir `direction_hint` — cambio pequeño, impacto alto.
2. Cambiar `base64url_decode` para recibir `cap` — seguridad inmediata.
3. Añadir `BOT_MAX_SIGNAL_AGE_MS` y usar `timestamp_ns`.
4. Añadir `BOT_MAX_BOOK_AGE_MS` guardando timestamp en book.
5. Validar enums/rangos de env vars.
6. Crear `alpha_parser.hpp` y testear el parser.
7. Agregar contador `pool_hit/miss`.
8. Reiniciar el spin counter después de procesar trabajo.
9. Corregir path del README para cross-check.
10. Agregar ASAN/UBSAN a CI.

---

## 15. Conclusión

La base es buena y tiene ideas fuertes: pre-firmado, hot path fijo, crypto verificada, tests y documentación honesta. Lo que falta no es “hacerlo más rápido” primero; lo más importante es **hacerlo inequívocamente correcto, observable y controlado**.

El orden que maximiza seguridad y valor sería:

1. corregir memory/concurrency issues,
2. integrar señales reales,
3. agregar risk/order lifecycle,
4. validar live en shadow y con orden mínima,
5. optimizar pool/red con métricas reales.

Si se resuelven esos puntos, el proyecto puede pasar de “prototipo avanzado” a una base seria para trading automatizado experimental en Polymarket.
