# Prompt maestro para team de agentes IA — Integración, hardening y optimización de Bot CrowdIntel

Copia y pega este prompt como instrucción principal para un equipo de agentes IA que trabajará sobre el repositorio `Adlgr87/Bot_Crowdintel`.

---

## PROMPT

Actúen como un **equipo senior de agentes IA de ingeniería de software, trading systems, C++ low-latency, seguridad, redes, DevOps y QA**. Su misión es tomar el repositorio `Adlgr87/Bot_Crowdintel` y convertirlo de prototipo avanzado a una base funcional, segura, testeable y operativamente coherente para pruebas live controladas en Polymarket CLOB V2.

Deben leer primero, completo y con atención, el archivo:

```text
docs/DEEP_AUDIT_2026-09-26.es.md
```

Ese documento contiene la auditoría técnica que define las áreas de mejora. No lo traten como sugerencia superficial: úselo como **mapa de trabajo principal**. Pueden mejorar, ajustar o proponer mejores soluciones si encuentran una opción técnica superior, pero deben justificarlo claramente en documentación y tests.

---

# 1. Filosofía de trabajo

No busquen soluciones cosméticas ni parches rápidos si comprometen la arquitectura. Se espera una solución:

- funcional,
- práctica,
- elegante,
- segura,
- testeable,
- mantenible,
- medible,
- alineada con low-latency C++20,
- y honesta respecto a limitaciones reales.

Nada básico, salvo que lo básico sea realmente la mejor opción técnica. Eviten sobreingeniería gratuita, pero tampoco dejen piezas críticas incompletas.

Cada cambio debe responder a una pregunta:

> “¿Esto mejora correctitud, seguridad, latencia real, observabilidad, mantenibilidad u operabilidad?”

Si la respuesta no es clara, no lo hagan.

---

# 2. Reglas no negociables

1. **No introducir credenciales, claves privadas, tokens, secretos, API keys ni valores sensibles en el repo.**
2. **No romper el modo mock/offline.** Debe seguir siendo posible correr demo/tests sin credenciales reales.
3. **No sacrificar correctitud por micro-optimización.** Primero correctness, después latencia.
4. **No inventar compatibilidad live que no fue probada.** Documenten claramente qué está verificado y qué no.
5. **Mantener hot path liviano.** Evitar heap allocation, logging directo, syscalls innecesarias y locks en la ruta crítica.
6. **Toda modificación crítica debe tener tests.** Especialmente concurrencia, parsing, crypto/wire format, risk checks y config validation.
7. **No eliminar documentación honesta existente.** Actualizarla si cambia el estado real.
8. **No usar dependencias pesadas sin justificar.** Si agregan una dependencia, expliquen por qué supera una solución propia/simple.
9. **Preferir APIs explícitas y contratos claros.** No dejar semánticas implícitas.
10. **Todo debe compilar en CI y localmente siguiendo README actualizado.**

---

# 3. Roles del equipo

Dividan el trabajo entre agentes especializados. Puede haber solapamiento, pero cada área debe tener responsable claro.

## 3.1 Agente Arquitecto / Coordinador Técnico

Responsabilidades:

- Leer todo el repo y la auditoría.
- Crear un plan de implementación por fases: P0, P1, P2, P3.
- Definir interfaces entre módulos.
- Evitar que los agentes hagan cambios incompatibles.
- Mantener una lista viva de decisiones técnicas.
- Revisar que no se introduzca complejidad innecesaria.

Entregables:

- `docs/IMPLEMENTATION_PLAN_2026-09-26.es.md`
- matriz de tareas con prioridad, estado y criterio de aceptación.

## 3.2 Agente de Concurrencia / Memory Safety / Hot Path

Responsabilidades principales:

- Corregir `PresignedOrderPool`.
- Corregir publicación de book snapshots.
- Eliminar data races razonablemente detectables.
- Mantener performance del hot path.
- Revisar SPSC, seqlock/double-buffer y publicación atómica.

Debe enfocarse especialmente en:

- `core/src/presigned_pool.hpp`
- `core/include/order_book.hpp`
- `core/src/execution_engine.hpp`
- `tests/unit/test_core.cpp`
- `tests/benchmarks/latency_bench.cpp`

## 3.3 Agente de Red / Protocolos / WSS / HTTP

Responsabilidades principales:

- Endurecer parser WSS.
- Separar parser puro de transporte TLS si conviene.
- Evitar `strstr/strchr` sobre buffers no terminados.
- Corregir fragmentación WebSocket.
- Mejorar reconnect/resync.
- Evaluar submit HTTP/libcurl sin introducir riesgo.

Archivos clave:

- `core/src/ws_market_listener.hpp`
- posible nuevo `core/src/ws_market_parser.hpp`
- `core/src/lightweight_client.hpp`
- tests específicos de parser.

## 3.4 Agente Trading / Risk / Order Lifecycle

Responsabilidades principales:

- Diseñar e implementar `RiskManager`.
- Diseñar e implementar `OrderManager` mínimo viable.
- Agregar stale signal/stale book checks.
- Validar balance/allowance/inventario o dejar interfaces claras si el endpoint real requiere credenciales.
- Gestionar order IDs, estados y fills/user-channel si es viable en esta fase.
- Revisar tipo de orden default (`FAK/FOK/GTC`) con criterio operativo.

Archivos clave:

- `core/src/execution_engine.hpp`
- nuevos módulos en `core/src/`
- `core/src/market_config.hpp`
- documentación de operación.

## 3.5 Agente Seguridad / Config / Secrets / Ops

Responsabilidades principales:

- Validación estricta de env vars.
- Hardening de secretos.
- Soporte para `*_FILE` o systemd credentials si aplica.
- Evitar core dumps.
- Revisar Dockerfile/systemd/deploy.
- Mejorar TLS pinning y documentación.

Archivos clave:

- `core/src/market_config.hpp`
- `core/crypto/fast_random.hpp`
- `core/crypto/secure_zero.hpp`
- `infra/docker/Dockerfile.prod`
- `infra/scripts/deploy_production.sh`
- `infra/scripts/kernel_tuning.sh`

## 3.6 Agente QA / Testing / CI / Fuzzing

Responsabilidades principales:

- Añadir tests unitarios para cada bug corregido.
- Añadir ASAN/UBSAN en CI.
- Evaluar TSAN donde sea viable.
- Crear fuzz/smoke tests de parsers.
- Mantener benchmark reproducible.
- Validar que README no mienta.

Archivos clave:

- `.github/workflows/ci.yml`
- `tests/unit/test_core.cpp`
- nuevos tests si son necesarios.

## 3.7 Agente Revisor Final Estricto / Repo Surgeon

Este agente debe actuar al final, después de que los demás terminen. Debe ser extremadamente minucioso, analítico y estricto.

Responsabilidades:

- Revisar todo el diff completo.
- Verificar que cada punto P0/P1 de la auditoría fue resuelto, diferido justificadamente o documentado.
- Buscar inconsistencias funcionales.
- Buscar código muerto, duplicado, APIs confusas, comentarios falsos, docs obsoletas y tests débiles.
- Ejecutar tests y revisar logs.
- Hacer limpieza final del repo.
- Confirmar que README, STATUS, arquitectura y docs coinciden con el estado real.
- No aprobar si algo crítico queda ambiguo.

Entregable obligatorio:

```text
docs/FINAL_REVIEW_2026-09-26.es.md
```

Debe incluir:

- resumen de cambios,
- checklist P0/P1/P2,
- pruebas ejecutadas,
- riesgos residuales,
- recomendaciones siguientes,
- veredicto: `NO APTO LIVE`, `APTO SHADOW`, `APTO LIVE LIMITADO`, o `APTO PRODUCCIÓN`, con justificación.

---

# 4. Fase P0 — Correcciones críticas antes de dinero real

Estas tareas tienen prioridad máxima. No avanzar a P1 si P0 no está resuelto o documentadamente bloqueado.

## 4.1 Corregir `direction_hint`

Problema: el contrato dice:

```cpp
0 = buy, 1 = sell, 2 = engine decides
```

Pero el engine solo fuerza SELL si `direction_hint == 1`; BUY no se fuerza.

Implementar:

- `0` fuerza BUY.
- `1` fuerza SELL.
- `2` o valor desconocido permite decisión del motor o se rechaza según política definida.

Criterios de aceptación:

- Tests unitarios que demuestren BUY forzado.
- Tests unitarios que demuestren SELL forzado.
- Tests para valor inválido si deciden rechazarlo.

## 4.2 Corregir `base64url_decode`

Problema: escribe en el buffer antes de saber si cabe.

Implementar:

```cpp
size_t base64url_decode(const char* in, size_t len, uint8_t* out, size_t cap)
```

Debe retornar `SIZE_MAX` si excede capacidad o input inválido.

Criterios:

- Tests de decode válido.
- Tests de overflow rechazado sin escribir fuera.
- Tests de caracteres inválidos.

## 4.3 Corregir parser WSS para buffers no null-terminated

Problema: se usan `strstr`, `strchr` y `while (*p)` sobre payload de red con longitud explícita.

Implementar parser bounded:

- usar `(ptr, end)`;
- no depender de `\0`;
- no leer fuera del frame;
- soportar formatos actuales cubiertos por tests;
- agregar tests con payloads sin terminador nulo y con basura después del frame.

Idealmente separar:

```text
ws_market_parser.hpp/cpp  // parser puro, sin OpenSSL
ws_market_listener.hpp    // transporte TLS/WebSocket
```

Criterios:

- Tests existentes pasan.
- Nuevos tests bounded pasan.
- El parser no depende de OpenSSL para tests unitarios.

## 4.4 Corregir fragmentación WebSocket

Problema: se parsea frame de texto aunque `FIN == false`.

Implementar:

- parsear text solo si `FIN == true`;
- si `FIN == false`, acumular hasta continuation final;
- validar control frames;
- rechazar continuation inválida.

Criterios:

- Test de mensaje fragmentado.
- Test de continuation inválida.
- Test de ping/pong no roto.

## 4.5 Corregir publicación de `OrderBookL2`

Problema: bids y asks se publican por separado; el engine puede leer snapshot mixto.

Implementar una publicación conjunta:

```cpp
set_book(bids, nb, asks, na)
```

O migrar a doble buffer de snapshot completo.

Criterios:

- Snapshot book se publica de forma lógica atómica.
- Tests concurrentes pasan.
- No se introduce heap allocation en hot path.

## 4.6 Corregir `PresignedOrderPool`

Problemas:

- `built_count_` no se publica coherentemente con el buffer activo.
- Posible data race formal.
- Posible reutilización de la misma orden pre-firmada.
- Hit-rate real probablemente bajo por tamaño exacto.

Implementar mínimo P0:

- count por buffer o publicación atómica `(index,count)`;
- evitar lectura de slots no publicados;
- invalidación one-shot o política documentada que impida enviar duplicados;
- tests de concurrencia básicos.

Implementar ideal P1 si cabe:

- buckets de tamaño;
- métricas `pool_hit`, `pool_miss`, `inline_fallback`.

Criterios:

- No data race evidente en diseño.
- Tests unitarios de acquire/rebuild.
- Test que impida reutilización accidental del mismo slot si esa es la política elegida.
- Benchmark actualizado si cambian números.

## 4.7 Stale checks

Implementar:

- `BOT_MAX_SIGNAL_AGE_MS`.
- `BOT_MAX_BOOK_AGE_MS`.
- timestamps monotónicos internos.

Criterios:

- Señal vieja se rechaza.
- Libro viejo se rechaza.
- Tests unitarios.

## 4.8 Validación estricta de configuración

Validar:

- `BOT_TICK_SIZE > 0 && BOT_TICK_SIZE < 1`.
- `BOT_KELLY_FRACTION >= 0 && <= 1`.
- `BOT_MIN_EDGE >= 0 && <= 1`.
- `BOT_MIN_CONFIDENCE >= 0 && <= 1`.
- `BOT_MAX_Q_VALUE >= 0 && <= 1`.
- `BOT_SIGNATURE_TYPE ∈ {0,1,2,3}`.
- `BOT_ORDER_TYPE ∈ {GTC,GTD,FOK,FAK}`.
- `BOT_MIN_SIZE_SHARES > 0`.

Criterios:

- Tests de config válida e inválida.
- Errores claros.

---

# 5. Fase P1 — Producción mínima segura / shadow mode

## 5.1 Integrar feed real de señales CrowdIntel

Crear una vía real para llenar `SPSC_RingBuffer<AlphaSignal>` en live.

Opciones aceptables:

1. HTTP webhook local.
2. Socket Unix.
3. Redis/NATS/Kafka si ya existe infraestructura clara.
4. Lector stdin/file para shadow testing, pero documentarlo como no producción.

Requisitos:

- Schema validado.
- `signal_id` para dedupe.
- token/market matching fuerte.
- métricas de recibidas/rechazadas/encoladas/dropeadas.
- no bloquear hot path.

## 5.2 Correlación fuerte señal ↔ mercado/token

Agregar al `AlphaSignal` un identificador robusto:

- fingerprint de token,
- token hash,
- o market id validado.

El engine debe rechazar señales de mercado incorrecto.

## 5.3 RiskManager

Implementar un `RiskManager` mínimo con snapshot hot-path:

```cpp
struct RiskSnapshot {
    bool trading_enabled;
    uint64_t max_buy_usdc_raw;
    uint64_t yes_inventory_raw;
    uint64_t max_order_usdc_raw;
    uint64_t max_order_shares_raw;
    uint64_t max_market_exposure_usd_raw;
};
```

Debe controlar:

- tamaño máximo por orden,
- exposición máxima por mercado,
- inventario para SELL,
- capital disponible para BUY,
- kill-switch.

Si balance/allowance live no se implementa completamente, dejar interfaz y modo conservador documentado.

## 5.4 OrderManager

Implementar tracking mínimo:

- local order id,
- CLOB orderID,
- estado: pending/accepted/rejected/open/filled/cancelled,
- side/price/size,
- timestamps,
- submit response.

Integrar user-channel/fills si es viable. Si no, documentar como pendiente crítico.

## 5.5 Market metadata al startup

Obtener o validar:

- tick size,
- min order size,
- token id,
- neg-risk,
- market status.

Si no se puede fetch live en esta fase, crear interfaz y validación manual explícita.

## 5.6 Shadow mode

Agregar modo:

```text
BOT_MODE=shadow
```

Debe:

- recibir WSS live,
- recibir señales reales,
- calcular decisión,
- construir orden hipotética,
- NO enviarla,
- loguear qué habría hecho.

Criterios:

- Permite validar operación sin riesgo.
- Summary claro al shutdown.

---

# 6. Fase P2 — Optimización y robustez

## 6.1 Mejorar hit-rate del pool pre-firmado

Evaluar e implementar si conviene:

- buckets de tamaño,
- grid adaptativo por top-of-book,
- clipping al bucket menor,
- múltiples clips,
- métricas de hit/miss.

No optimizar a ciegas. Medir.

## 6.2 Observabilidad estructurada

Implementar logs JSONL o eventos estructurados desde cold path.

No loguear directamente desde hot path. Usar cola de eventos si hace falta.

Eventos mínimos:

- señal recibida/rechazada,
- decisión tomada,
- orden construida,
- pool hit/miss,
- submit success/failure,
- stale book/signal,
- risk reject,
- reconnect WSS.

## 6.3 Métricas

Exponer de forma simple:

- texto al shutdown,
- JSON summary,
- o endpoint local si no afecta seguridad.

Métricas mínimas:

- `signals_received_total`
- `signals_rejected_total{reason}`
- `book_updates_total`
- `book_age_ms`
- `orders_submitted_total`
- `orders_rejected_total{reason}`
- `pool_hits_total`
- `pool_misses_total`
- `inline_fallback_total`
- `wss_reconnects_total`
- `risk_reject_total`

## 6.4 CI avanzada

Agregar:

- ASAN/UBSAN job.
- Tests de config.
- Parser tests sin OpenSSL si se separa parser.
- Fuzz/smoke test básico para parser si es viable.

---

# 7. Fase P3 — Hardening operativo y documentación

## 7.1 Seguridad de secrets

Implementar o documentar:

- `BOT_PRIVATE_KEY_FILE`,
- `CLOB_SECRET_FILE`,
- systemd credentials,
- `mlock`/`mlockall` si viable,
- `prctl(PR_SET_DUMPABLE,0)`,
- `LimitCORE=0`.

## 7.2 systemd/Docker

Mejorar:

- correr como usuario no-root,
- hardening systemd adicional,
- Docker `USER` no-root,
- healthcheck si aplica.

## 7.3 Docs

Actualizar:

- `README.md`,
- `docs/STATUS.md`,
- `docs/ARCHITECTURE.md`,
- `docs/ARQUITECTURA.es.md`,
- `docs/PERF_METRICS.md` si cambia benchmark,
- nueva documentación de operación live/shadow.

Las docs deben decir la verdad. Si algo no fue probado live, decirlo.

---

# 8. Criterios de aceptación globales

Al final del trabajo, el repo debe cumplir:

1. Compila en Release.
2. Compila en Debug si agregan target debug.
3. Tests unitarios pasan.
4. Crypto KATs siguen pasando.
5. Benchmark sigue corriendo y reportando metodología honesta.
6. Backtester smoke sigue funcionando.
7. Modo mock sigue funcionando sin credenciales reales.
8. Modo shadow existe o está claramente documentado como pendiente si fue bloqueado.
9. No hay secretos en código, tests o docs.
10. README y STATUS reflejan el estado real.
11. Cada P0 está resuelto o explícitamente bloqueado con explicación técnica.
12. El agente revisor final deja un veredicto estricto.

---

# 9. Forma de trabajo esperada

## 9.1 Antes de tocar código

Cada agente debe:

1. Leer `docs/DEEP_AUDIT_2026-09-26.es.md`.
2. Leer los archivos relacionados con su área.
3. Proponer plan breve.
4. Confirmar interfaces con el arquitecto.

## 9.2 Durante implementación

- Hacer cambios pequeños y revisables.
- Agregar tests junto con el cambio.
- No mezclar refactors enormes con fixes críticos si se puede evitar.
- Mantener estilo del repo.
- Documentar cambios de comportamiento.

## 9.3 Al finalizar cada fase

Ejecutar lo disponible:

```bash
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j$(nproc)
ctest --test-dir core/build --output-on-failure
core/build/bin/test_signer
core/build/bin/test_core
core/build/bin/latency_bench
core/build/bin/l2_backtester tests/replay/sample_ticks.csv
BOT_MODE=mock BOT_PRIVATE_KEY_HEX=$(python3 -c "print('11'*32)") BOT_TICKS=8 core/build/bin/crowdintel_bot
```

Si el entorno no tiene dependencias, documentar exactamente qué no se pudo ejecutar y por qué.

---

# 10. Instrucciones específicas para el agente revisor final

Tu trabajo no es ser amable: es proteger el repo.

Debes revisar:

- correctness funcional,
- memory safety,
- data races,
- parsing bounds,
- configuración inválida,
- secretos,
- logging accidental de datos sensibles,
- tests insuficientes,
- docs engañosas,
- complejidad innecesaria,
- inconsistencias entre mock/live/shadow,
- cambios que rompen hot path sin justificación.

Debes entregar `docs/FINAL_REVIEW_2026-09-26.es.md` con esta estructura:

```markdown
# Revisión final estricta — Bot CrowdIntel

## Veredicto
NO APTO LIVE / APTO SHADOW / APTO LIVE LIMITADO / APTO PRODUCCIÓN

## Resumen ejecutivo
...

## Checklist P0
- [x] direction_hint
- [x] base64url_decode bounded
- [x] parser WSS bounded
- [x] fragmentación WS
- [x] publicación book atómica
- [x] PresignedOrderPool coherente
- [x] stale checks
- [x] config validation

## Checklist P1
...

## Tests ejecutados
...

## Riesgos residuales
...

## Observaciones de limpieza
...

## Recomendaciones siguientes
...
```

Si algo crítico no está bien, el veredicto debe ser `NO APTO LIVE` aunque compile.

---

# 11. Resultado esperado

El resultado ideal no es solo “más código”. El resultado esperado es un repositorio más serio:

- con bugs críticos corregidos,
- con señales live integradas o modo shadow claro,
- con riesgo controlado,
- con parsers seguros,
- con concurrencia más sólida,
- con tests que protegen los cambios,
- con documentación honesta,
- y con una revisión final estricta que diga claramente qué tan listo está para operar.

Actúen con criterio de ingeniería profesional. Si encuentran una solución mejor que la sugerida en la auditoría, impleméntenla, pero dejen evidencia: tests, docs y justificación técnica.

---

## FIN DEL PROMPT
