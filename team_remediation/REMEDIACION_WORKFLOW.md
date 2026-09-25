# 🔄 WORKFLOW DE REMEDIACIÓN — Detallado por Fases

> **Fuente**: `WORKFLOW_REMEDIACION_CUMPLIMIENTO.md`
> **Repo**: `Adlgr87/Bot_Crowdintel` (rama `main`, CLOB V2)
> **Brama objetivo**: `remediation/compliance`

---

## 🎯 Objetivo General

Elevar el bot de "técnicamente correcto pero desprotegido" a "operable con capital real":
1. Respeto de rate limits de API + backoff (evitar suspensión por abuso).
2. Risk engine real con kill switch y límites de exposición/pérdida.
3. Order manager + reconciliación de fills/posiciones (no fire-and-forget).
4. Modelo de comisiones/gas/slippage integrado en el filtro de edge.
5. Compliance guard (mercado activo, jurisdicción, tick size dinámico).
6. Observabilidad: auditoría append-only + alertas.

---

## FASE 0 — Línea base e higiene (Baseline)

**Entregable**: rama `remediation/compliance` con CI verde.

| T0-# | Tarea | Archivo(s) | Criterio de aceptación |
|---|---|---|---|
| T0-1 | Añadir `*.env`, `.env*`, `secrets/` a `.gitignore` | `.gitignore` | `git check-ignore .env` devuelve match |
| T0-2 | Eliminar o marcar como demo `main_hot_path.cpp` | `core/src/main_hot_path.cpp`, `core/CMakeLists.txt` | Build determinista sin referencias rotos; `grep -r "YOUR_API_KEY_HERE"` vacío |
| T0-3 | Unificar cliente: confirmar lightweight_client.hpp como de producción | `core/src/lightweight_client.hpp`, `CMakeLists.txt` | Solo un cliente HTTP en el código de producción |
| T0-4 | Registrar línea base: ctest + benchmark P50/P99 | `docs/PERF_METRICS.md` | Números base registrados antes de tocar nada |

### Branch
```
remediation/00-baseline
```

### Estado actual del baseline (medido antes de cambios)
- `ctest`: 2/2 tests pass (keccak_known_answer + latency_benchmark), ~1.4s
- `latency_bench` P50: ~47 µs, P99: ~52-90 µs (calibrado TSC)
- Compilación: `cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)` — exitosa
- **Problemas conocidos**: `.gitignore` no excluye `.env`; `main_hot_path.cpp` hardcodea direcciones; `lightweight_client.hpp` no parsea respuestas

---

## FASE 1 — Rate limiting y cliente HTTP (API)

**Dependencia: Fase 0. Prioridad máxima.**

| T1-# | Tarea | Archivo(s) objetivo | Criterio de aceptación |
|---|---|---|---|
| T1-1 | `RateLimiter` por endpoint (token bucket) configurable por env | `core/include/rate_limiter.hpp` (nuevo) | Test: >X req/s bloquea antes de salir a red |
| T1-2 | Backoff exponencial con jitter ante 429/5xx; leer `Retry-After` | módulo cliente HTTP | Test: 429 → retardos crecientes; no retry ciego |
| T1-3 | Connection pooling / keep-alive persistente | módulo cliente HTTP | Una conexión TLS reutilizada |
| T1-4 | Parseo real de respuesta HTTP: 200/429/4xx/5xx, extraer `order_id`/error | módulo cliente HTTP | 429 no es éxito; order_id capturado |
| T1-5 | Eliminar claim de 10,000 órdenes/s | `README.md`, `WORKFLOW_MAESTRO_*.md` | Sin cifras de abuso |

### Branch
```
remediation/10-api-ratelimit
```

### Estructura del RateLimiter (token bucket)
```cpp
class RateLimiter {
public:
    RateLimiter(double rate_per_sec, double burst);
    bool try_acquire();                    // O(1), branch-predicted
    std::chrono::microseconds next_available();  // para scheduling
private:
    std::atomic<double> tokens_;
    const double rate_per_sec_;
    const double burst_;
    std::atomic<uint64_t> last_refill_ns_;
};
```

### Response del cliente HTTP
```cpp
enum class HttpStatus : int {
    OK = 200,
    TOO_MANY = 429,
    BAD_REQUEST = 400,
    UNAUTHORIZED = 401,
    FORBIDDEN = 403,
    SERVER_ERROR = 500,
    BAD_GATEWAY = 502,
    SERVICE_UNAVAILABLE = 503,
};

struct HttpResponse {
    HttpStatus status;
    std::string body;
    std::optional<std::string> retry_after;
    std::optional<std::string> order_id;  // extraído del JSON body
};
```

---

## FASE 2 — Risk engine y kill switch (Risk)

**Dependencia: Fase 0.**

| T2-# | Tarea | Archivo(s) objetivo | Criterio de aceptación |
|---|---|---|---|
| T2-1 | Parámetros de riesgo en `MarketConfig` (env-driven) | `core/src/market_config.hpp` | Cargados desde env con defaults conservadores; test de parseo |
| T2-2 | `RiskEngine::pre_trade_check(order)` antes de firmar | `core/include/risk_engine.hpp`, `core/src/execution_engine.cpp` | Order > max_order_usd → RISK_BLOCKED |
| T2-3 | Kill switch atómico | `risk_engine.hpp`, `execution_engine.cpp` | Flag activo → run_tick devuelve KILL_SWITCH, no firma |
| T2-4 | Consulta de balance USDC/POL (cold path) | `core/src/balance_checker.hpp` | Balance bajo → pre_trade_check bloquea |
| T2-5 | Ventanas deslizantes órdenes/cancelaciones/minuto | `risk_engine.hpp` | > N órdenes/60s → bloqueo hasta resetear |

### Branch
```
remediation/20-risk-engine
```

### TickResult extendido
```cpp
enum class TickResult {
    OK,
    NO_SIGNAL,
    NO_EDGE,
    NOT_PROFITABLE,
    RISK_BLOCKED,
    KILL_SWITCH,
    MARKET_NOT_TRADABLE,
    DUPLICATE_ORDER,
};
```

### Parámetros de riesgo (env-driven)
| Parámetro | Default | Env Var |
|---|---|---|
| max_daily_loss_usd | 500.0 | `RISK_MAX_DAILY_LOSS_USD` |
| max_exposure_per_market | 5000.0 | `RISK_MAX_EXPOSURE_PER_MARKET` |
| max_exposure_per_side | 2000.0 | `RISK_MAX_EXPOSURE_PER_SIDE` |
| max_order_usd | 500.0 | `RISK_MAX_ORDER_USD` |
| max_open_orders | 5 | `RISK_MAX_OPEN_ORDERS` |
| max_orders_per_min | 10 | `RISK_MAX_ORDERS_PER_MIN` |
| max_cancels_per_min | 20 | `RISK_MAX_CANCELS_PER_MIN` |
| max_price_deviation_bps | 500 | `RISK_MAX_PRICE_DEVIATION_BPS` |
| min_usdc_balance | 100.0 | `RISK_MIN_USDC_BALANCE` |
| min_pol_balance | 10.0 | `RISK_MIN_POL_BALANCE` |
| max_position_divergence | 0.01 | `RISK_MAX_POSITION_DIVERGENCE` |

---

## FASE 3 — Order manager, reconciliación y positions (Orders)

**Dependencia: Fase 1.**

| T3-# | Tarea | Archivo(s) objetivo | Criterio de aceptación |
|---|---|---|---|
| T3-1 | Registro `client_order_id`, estados | `core/include/order_manager.hpp` | Toda orden registrada con id y estado inicial |
| T3-2 | User-channel fills feed; actualizar position tracker | `core/src/ws_market_listener.hpp` (extender), `core/include/position_tracker.hpp` | Test mock: posición interna actualizada y coincide |
| T3-3 | Reconciliación periódica (cold path) | `position_tracker.hpp` | Divergencia inyectada → kill switch o alerta |
| T3-4 | Anti-reintento ciego: consultar estado antes de reenviar | `order_manager.hpp`, `execution_engine.cpp` | Timeout → se consulta estado; si filled, no reenvía |
| T3-5 | Cancelación de órdenes obsoletas | `core/src/presigned_pool.hpp`, `order_manager.hpp` | Salto de precio → pool inválido, órdenes canceladas |
| T3-6 | Detección de self-trade | `order_manager.hpp` | Orden propia en book → bloqueada o cancelada |

### Branch
```
remediation/30-order-manager
```

---

## FASE 4 — Modelo de comisiones, gas y slippage (Economics)

**Dependencia: Fase 0.**

| T4-# | Tarea | Archivo(s) objetivo | Criterio de aceptación |
|---|---|---|---|
| T4-1 | `FeeModel`: maker/taker + fórmula dinámica | `core/include/fee_model.hpp` | Test: fee máximo cerca p=0.5, mínimo en extremos |
| T4-2 | Estimación gas/Polygon + slippage | `fee_model.hpp` | Coste sube con tamaño y spread |
| T4-3 | Reemplazar filtro edge > min_edge por net_ev | `core/src/execution_engine.cpp` | Edge positivo, net_ev negativo → NOT_PROFITABLE |
| T4-4 | Documentar comisiones dinámicas | `docs/` | Comentario + nota README |

### Branch
```
remediation/40-fee-model
```

### Fórmula dinámica de comisiones
```
fee = C × 0.25 × (p·(1−p))²
```
Donde:
- `C` = comisión base por contrato (configurable via `FEE_COMMISSION_RATE`)
- `p` = probabilidad del mercado (q_value de CrowdIntel)
- Activo solo si `FEE_DYNAMIC_ENABLED=true` por mercado

---

## FASE 5 — Compliance guard y metadata de mercado (Compliance)

**Dependencia: Fase 0.**

| T5-# | Tarea | Archivo(s) objetivo | Criterio de aceptación |
|---|---|---|---|
| T5-1 | Fetch metadata en arranque: tick size, estado, resolución | `core/include/market_metadata.hpp` | Tick size aplicado; mercado cerrado → bloquea |
| T5-2 | `ComplianceGuard`: lista restringida, jurisdicción | `core/include/compliance_guard.hpp` | token_id en blocklist → rechazo; jurisdicción desactivada → arranque bloqueado |
| T5-3 | Rechazar si mercado no activo o resolución próxima | `compliance_guard.hpp`, `execution_engine.cpp` | `TickResult::MARKET_NOT_TRADABLE` |
| T5-4 | (Opcional) Almacenar criterio/fuente de resolución | `market_metadata.hpp` | Persistencia fría |

### Branch
```
remediation/50-compliance-guard
```

---

## FASE 6 — Observabilidad, auditoría y alertas (Observability)

**Dependencia: Fases 2-3.**

| T6-# | Tarea | Archivo(s) objetivo | Criterio de aceptación |
|---|---|---|---|
| T6-1 | Logger async append-only | `core/src/telemetry.hpp` | audit.log JSON lines; no eventos perdidos en hot path |
| T6-2 | Alertas configurables | `telemetry.hpp` | Umbral cruzado → evento alerta |
| T6-3 | Métricas expuestas | `telemetry.hpp` | Endpoint o log periódico |

### Branch
```
remediation/60-telemetry
```

---

## FASE 7 — QA, seguridad y docs (QA)

**Dependencia: Fases 1-6.**

| T7-# | Tarea | Criterio de aceptación |
|---|---|---|
| T7-1 | ctest completo verde + tests unitarios para cada módulo | Cobertura positiva y negativa |
| T7-2 | Regresión de latencia: hot path P50/P99 dentro +10% baseline | Benchmark comparativo |
| T7-3 | Escaneo de secretos | Sin secretos en commits; git diff limpio |
| T7-4 | Actualizar docs/STATUS.md | Open items resueltos marcados |
| T7-5 | Actualizar README | Sección cumplimiento + límites de riesgo |
| T7-6 | Revisión adversarial final | Checklist firmada por QA |

### Branch
```
remediation/70-qa-docs
```

---

## 🔒 Auditoría Final (Post-Fase 7)

**Rol**: Verificación extremadamente rigurosa de principio a fin.

**Protocolo**:
1. Compilación desde cero → 0 warnings
2. `ctest --output-on-failure` → 100% PASS
3. `latency_bench` → P50/P99 ≤ baseline × 1.10
4. `grep -rn` secretos → 0 coincidencias
5. `test_signer` → KAT Keccak-256 + EIP-712 verificados
6. Hot path audit → 0 std::cout, 0 malloc/new en hot path
7. Risk engine audit → kill switch imposibilita firma
8. Order lifecycle audit → client_order_id rastreado end-to-end
9. Compliance audit → tick size dinámico, mercado bloqueado
10. Documentation audit → README sin cifras fabricadas
11. Observabilidad audit → audit log append-only, alertas configurables
12. Integridad audit → eip712_signer.hpp no modificado

**Output**: `team_remediation/verification/AUDIT_FINAL_REPORT.md`

**Regla**: Cualquier fallo → reporta con evidencia → reparación iterativa → re-verifica.
```
STATUS: PASS (12/12 ítems)
```
