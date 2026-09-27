# Plan de Implementación — Bot CrowdIntel

_Fecha: 2026-09-26_  
_Autor: Agent Team OMNISCIENT_  
_Rama: `remediation/compliance`_

---

## Estado actual del repositorio

El repositorio ya ha implementado la mayoría de las funcionalidades de P1:
- ✅ RiskEngine con kill switch, rate limits, exposición
- ✅ OrderManager con tracking de órdenes y anti-retry
- ✅ ComplianceGuard con jurisdiction y restricted tokens
- ✅ FeeModel con dynamic fees
- ✅ MarketMetadataCache con tick size y market status
- ✅ Telemetry con logging asíncrono y métricas
- ✅ PositionTracker con PnL tracking
- ✅ Async submission queue (SPSC)
- ✅ TickResult enum extendido

**Sin embargo, se identificaron los siguientes gaps críticos (P0):**

| # | Issue | Severidad | Estado |
|---|-------|-----------|--------|
| 1 | AlphaSignal struct mismatch con alpha_parser.cpp | P0 (compilación rota) | 🔴 Pendiente |
| 2 | direction_hint no respetado (engine decide side basado en EV) | P0 (correctitud) | 🔴 Pendiente |
| 3 | PresignedOrderPool: data race, publicación incoherente, falta one-shot | P0 (concurrency) | 🔴 Pendiente |
| 4 | OrderBookL2: falta set_book() atómico | P0 (concurrencia) | 🔴 Pendiente |
| 5 | Falta stale check para señales (BOT_MAX_SIGNAL_AGE_MS) | P0 (risk) | 🔴 Pendiente |
| 6 | Validación estricta de configuración (ranges/enums) | P0 (risk) | 🔴 Pendiente |
| 7 | Shadow mode no implementado | P1 | 🟡 Pendiente |

> **Nota**: `base64url_decode` y el parser WSS con `strstr/strchr` mencionados en la auditoría **no existen en el código actual**. El código ya evolucionó: `lightweight_client.hpp` usa OpenSSL para HMAC y `ws_market_listener.hpp` usa `std::string::find` (bounded). Estos items están resueltos.

---

## Matriz de tareas

### Fase P0 — Correcciones críticas

| Tarea | Descripción | Archivo(s) | Prioridad |
|-------|-------------|------------|-----------|
| P0.1 | Arreglar AlphaSignal: añadir `type`, `confidence`, `q_value`, `timestamp_ns` | `core/src/alpha_receiver.hpp` | P0 |
| P0.2 | Arreglar direction_hint: respetar dirección explícita BUY/SELL | `core/src/execution_engine.cpp` | P0 |
| P0.3 | Arreglar PresignedOrderPool: doble buffer + one-shot + métricas | `core/src/presigned_pool.hpp` | P0 |
| P0.4 | Arreglar OrderBookL2: añadir set_book() atómico | `core/include/order_book.hpp` | P0 |
| P0.5 | Añadir stale check para señales | `core/src/alpha_receiver.hpp`, `core/src/execution_engine.cpp` | P0 |
| P0.6 | Validación estricta de configuración | `core/src/market_config.hpp` | P0 |

### Fase P1 — Producción mínima segura

| Tarea | Descripción | Archivo(s) | Prioridad |
|-------|-------------|------------|-----------|
| P1.1 | Implementar shadow mode (BOT_MODE=shadow) | `core/src/main_prod.cpp` | P1 |
| P1.2 | Integrar alpha_parser.hpp real | `alpha/crowdintel/alpha_parser.hpp` | P1 |
| P1.3 | Métricas pool_hit/miss | `core/src/presigned_pool.hpp`, `core/src/telemetry.hpp` | P1 |

### Fase P2/P3 — Documentación y cierre

| Tarea | Descripción | Prioridad |
|-------|-------------|-----------|
| P2.1 | ASAN/UBSAN en CI | P2 |
| P3.1 | Documentación FINAL_REVIEW_2026-09-26.es.md | P3 |

---

## Criterios de aceptación P0

1. ✅ `alpha_parser.cpp` compila con los campos actualizados de AlphaSignal
2. ✅ Tests unitarios demuestran BUY forzado (direction_hint=0)
3. ✅ Tests unitarios demuestran SELL forzado (direction_hint=1)
4. ✅ PresignedOrderPool: no data race, one-shot semantics, pool_hit/miss metrics
5. ✅ OrderBookL2: `set_book()` publica bids+asks atómicamente
6. ✅ Signal staleness: señales viejas se rechazan con `BOT_MAX_SIGNAL_AGE_MS`
7. ✅ Config validation: todos los ranges/enums validados con errores claros
8. ✅ Todos los tests pasan con ASAN

---

## Asignación de agentes

| Área | Agente | Herramienta |
|------|--------|-------------|
| Análisis + planos | AXIOM | Claude Code CLI |
| PresignedPool + OrderBook | HERMES | OpenHands |
| AlphaSignal + direction_hint + stale | FORGE | GROQ |
| Config validation + tests | SWE | sweagent CLI |
| Review final | AEGIS | (directo) |
| CI/CD + ASAN | LEGION | OpenSwarm |
