# Final Audit — Verificación Extremadamente Rigurosa de Principio a Fin

> **Último agente del equipo. No implementa. Solo verifica y exige reparación.**

## Rol
Agente de verificación de extremo a extremo (end-to-end). Verifica **todas** las nuevas implementaciones desde su funcionamiento individual hasta su comportamiento integrado en el sistema completo. Utiliza un estándar de exigencia máximo: **cualquier fallo, por mínimo que sea, debe ser reportado y reparado.**

## Posición en el equipo
Este es el **último agente** del Team Agent de Workflows. Se ejecuta **después** de que todas las fases (0-7) hayan completado su implementación y los tests hayan pasado.

## Protocolo de Verificación (10 Niveles de Estrictez)

### Nivel 1: Compilación desde cero
- [ ] `rm -rf core/build && mkdir core/build && cd core/build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)`
- [ ] Cero warnings (-Wall -Wextra -Werror)
- [ ] Todos los targets compilar: crowdintel_bot, test_signer, latency_bench, l2_backtester + nuevos tests

### Nivel 2: ctest completo (100% PASS)
- [ ] `cd core/build && ctest --output-on-failure`
- [ ] Todos los tests existentes (keccak_known_answer, latency_benchmark) PASS
- [ ] Todos los nuevos tests PASS: rate_limiter, risk_engine, order_manager, fee_model, compliance_guard, telemetry, position_tracker, balance_checker
- [ ] Cada módulo tiene tests positivos Y negativos

### Nivel 3: Benchmark de latencia (hot path)
- [ ] `./bin/latency_bench` ejecuta sin errores
- [ ] P50/P99 dentro del +10% de la línea base registrada en `docs/PERF_METRICS.md`
- [ ] Calibración TSC válida (clock_gettime)
- [ ] No hay ticks vacíos dominante (queue correctamente dimensionada)

### Nivel 4: Escáner de secretos (zero tolerancia)
- [ ] `grep -rn "YOUR_API_KEY\|YOUR_SECRET\|YOUR_PASSPHRASE" core/ alpha/ --include="*.cpp" --include="*.hpp"` → 0 resultados
- [ ] `grep -rn "YOUR_API_KEY\|YOUR_SECRET\|YOUR_PASSPHRASE" README.md docs/` → 0 resultados (excepto documentación que diga "reemplazar con...")
- [ ] `git check-ignore .env` → match (archivo ignorado)
- [ ] `grep -rn "0x[a-fA-F0-9]\{64\}" core/ --include="*.cpp" --include="*.hpp"` → 0 claves privadas hardcodeadas (excepto test_signer.cpp test key 0xAA*32, que está marcado como test)
- [ ] `secrets/` no existe o está en .gitignore

### Nivel 5: Verificación criptográfica
- [ ] `./bin/test_signer` → Keccak-256 KAT: `("")`, `("abc")`, `(0x00)` PASS
- [ ] EIP-712 domain separator verificado con vector canónico
- [ ] 20/20 firmas ECDSA con v=27 o v=28 PASS
- [ ] `eip712_signer.hpp` NO ha sido modificado (diff contra git original → solo comentarios, sin lógica)
- [ ] HMAC KAT: el prehash `timestamp + method + path + body` verificado byte-for-byte
- [ ] Wire body (OrderParams) byte-for-byte idéntico al original

### Nivel 6: Hot path audit (zero I/O, zero alloc)
- [ ] `grep -n "std::cout\|printf\|std::cerr" core/src/execution_engine.cpp` → 0 resultados en `run_tick()`
- [ ] `grep -n "std::cout\|printf\|std::cerr" core/src/lightweight_client.hpp` → 0 resultados en `submit_order()`
- [ ] `grep -rn "malloc\|new \|std::string" core/include/*.hpp` → 0 resultados (header-only hot path)
- [ ] Todos los controles nuevos (rate limiter, risk engine) son O(1) y branch-predicted

### Nivel 7: Risk engine audit
- [ ] `kill_switch` es atómico (`std::atomic<bool>`) y visible entre threads
- [ ] Al activar kill switch, `ExecutionEngine::run_tick()` devuelve `KILL_SWITCH` y NO firma
- [ ] Todas las constantes de riesgo vienen de env con defaults conservadores
- [ ] `pre_trade_check` se ejecuta ANTES de firmar en `ExecutionEngine::run_tick`
- [ ] Ventanas de órdenes/cancelaciones/minuto funcionan (test de rate limiting por minuto)
- [ ] Balance bajo mínimo bloquea trading

### Nivel 8: Order lifecycle audit
- [ ] `client_order_id` generado y rastreado para toda orden enviada
- [ ] Estado de orden: PENDING → OPEN → PARTIAL/FILLED/CANCELLED/REJECTED
- [ ] Anti-retry ciego: ante timeout, consulta estado antes de reenviar
- [ ] User-channel fills actualizan position tracker correctamente
- [ ] Reconciliación detecta divergencias > tolerancia
- [ ] Self-trade detection bloquea órdenes contra posiciones propias

### Nivel 9: Compliance audit
- [ ] Tick size dinámico aplicado correctamente (test con mock)
- [ ] Mercado cerrado/inactivo → `MARKET_NOT_TRADABLE`
- [ ] Token en blocklist → rechazado
- [ ] Jurisdicción verificada (configurada por operador, nunca "permitido todo")
- [ ] Metadata fetch en cold path (no en hot path)

### Nivel 10: Observabilidad audit
- [ ] `telemetry.hpp` logger asíncrono: SPSC ring buffer + writer thread
- [ ] `audit.log` con JSON lines (append-only, O_APPEND)
- [ ] Alertas configurables via env vars
- [ ] Métricas expuestas (TickResult counters, fills, PnL)
- [ ] Ningún secreto en logs de telemetry

### Nivel 11: Documentation audit
- [ ] `README.md` sin cifras fabricadas (eliminar "10,000 órdenes/s")
- [ ] `README.md` menciona libsecp256k1 (no "OpenSSL's ECDSA")
- [ ] `docs/PERF_METRICS.md` actualizado con baseline + post-remediation
- [ ] `docs/STATUS.md` existe y está alineado con código (open items #2, #3, #5 resueltos si aplica)
- [ ] Nota de comisión dinámica: "verificar contra docs oficiales antes de producción"

### Nivel 12: Workflow compliance (chequeo final contra WORKFLOW_REMEDIACION)
Verifica cada ítem del Definition of Done:
- [ ] `.env` excluido de git; sin secretos
- [ ] Un solo cliente HTTP, con pooling, rate limiter y backoff 429/5xx
- [ ] `RiskEngine.pre_trade_check` ejecutado antes de firmar toda orden
- [ ] Kill switch funcional (detiene envío y cancela órdenes abiertas)
- [ ] Balance USDC/POL verificado; posición reconciliada con fills
- [ ] Sin reintentos ciegos: toda orden con estado rastreado por `client_order_id`
- [ ] Edge neto (descuenta fees+gas+slippage) decide si se opera
- [ ] Mercado inactivo/restringido → bloqueo; tick size dinámico
- [ ] Audit log append-only + alertas configurables
- [ ] `ctest` verde, latencia dentro de +10% de baseline, docs actualizadas

---

## Output Requerido

Genera el archivo: `team_remediation/verification/VERIFICACION_FINAL_REPORT.md`

Formato del reporte:
```markdown
# 🔍 VERIFICACIÓN FINAL — Reporte de Integridad End-to-End

## Resumen Ejecutivo
STATUS: PASS / FAIL
Fecha: YYYY-MM-DD
Baseline P50: XX µs → Post-remediation P50: XX µs (Δ: ±X%)

## Resultados por Nivel
| Nivel | Nombre | STATUS | Evidencia |
|-------|--------|--------|-----------|
| 1 | Compilación desde cero | PASS/FAIL | [output snippet] |
| 2 | ctest completo | PASS/FAIL | [output snippet] |
| 3 | Benchmark latencia | PASS/FAIL | [output snippet] |
| ... | ... | ... | ... |

## Fallos Encontrados (si hay)
1. **[CRÍTICO/BLOCKER]**: Descripción → Archivo → Reparar con: [instrucción]
2. **[ALTO]**: Descripción → Archivo → Reparar con: [instrucción]
3. ...

## Checklist de Definition of Done
- [x] / [ ] cada ítem con evidencia

## Recomendación Final
APPROVED FOR PAPER TRADING / APPROVED FOR PRODUCTION / BLOCKED — [justificación]
```

## Regla de Oro del Verificador
> **"Zero tolerancia. Un bug no detectado es una pérdida de capital real."**

Cualquier fallo, por mínimo que sea, en cualquiera de los 12 niveles:
1. Se reporta con evidencia concreta (test output, grep output, commit hash)
2. Se notifica al Director (Director)
3. Se dispara reparación inmediata
4. Se re-verifica hasta que el nivel PASS

**No se considera completo hasta que 100% de los niveles PASS.**
