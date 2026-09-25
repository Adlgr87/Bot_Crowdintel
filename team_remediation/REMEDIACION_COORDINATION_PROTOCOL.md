# Protocolo de Coordinación — Remediación Cumplimiento

> **Bot_Crowdintel**
> Workflow origen: `WORKFLOW_REMEDIACION_CUMPLIMIENTO.md`
> Brama objetivo: `remediation/compliance`

---

## 1. Regla de Oro

> **Un implementador escribe el código, otro prueba, la auditoría final verifica de principio a fin, y el Director aprueba o exige reparación.**

Ningún entregable se autocertifica. Cada uno requiere validación cruzada y evidencia reproducible.

---

## 2. Director (Inamovible)

El Director **NO acepta** frases como:
- "parece correcto"
- "compila en mi entorno"
- "mejoró el promedio"
- "el benchmark dice X ciclos"
- "el revisor anterior lo revisó"

Exige siempre:
- Comando ejecutado
- Commit o patch aplicado
- Resultado reproducible (ctest output)
- Logs de pruebas
- Métricas de latencia (P50/P99 vs baseline)
- Fallos conocidos y riesgos restantes

---

## 3. Orden de Ejecución y Dependencias

```
Fase 0 (Baseline)
   ├─> Fase 1 (API) ──┐
   ├─> Fase 2 (Risk)  ├┼> Fase 3 (Orders) ─┐
   ├─> Fase 4 (Econ)  │ ─┘                  ├─> Fase 6 (Observability) ─> Fase 7 (QA)
   └─> Fase 5 (Compliance) ─────────────────┘
                            │
                            v
                      Auditoría Final
                            │
                            ▼
                   (Itera hasta 100% PASS)
```

### Paralelismo permitido
- **Fase 0** → bloqueante para todas (debe completarse primero)
- **Fase 1, 2, 4, 5** → paralelas (dependen solo de Fase 0)
- **Fase 3** → requiere Fase 1 completa (necesita order_id real)
- **Fase 6** → requiere Fases 2 y 3 (métricas de riesgo y fills)
- **Fase 7** → requiere Fases 1-6 todas completas
- **Auditoría Final** → post-Fase 7, iterativo hasta 100% PASS

---

## 4. Reglas de Coordinación

### 4.1 Tarea bien definida
Cada tarea debe tener:
- **Entrada**: archivo(s) existentes, estado actual
- **Salida esperada**: archivos creados/modificados, comportamiento esperado
- **Archivos permitidos**: lista blanca de paths (respetar restricciones de seguridad)
- **Tests obligatorios**: criterios de aceptación concretos
- **Criterios de rechazo**: condiciones que invalidan el trabajo

### 4.2 Separar cambios funcionales y de rendimiento
Commits separados por tipo:
```
fix: correct EIP-712 domain separator
test: add rate limiter KAT
feat: add risk engine pre_trade_check
perf: optimize order book lookup
infra: add .gitignore entries
docs: update README compliance section
```

### 4.3 Evidencia obligatoria
Cada claim debe apuntar a: test, log, benchmark, commit, dataset, configuración, o hash.

### 4.4 Dos revisores para cambios críticos
Áreas críticas: criptografía, riesgo, cliente de órdenes, reconciliación, secretos, transporte, apagado de emergencia.
Requiere: implementador + Auditoría Final + Director.

### 4.5 Branches aisladas
- Cada fase trabaja en su propia rama: `remediation/<fase>`
- El Director controla merge a `remediation/compliance`
- La Auditoría Final hace review antes de cada merge

---

## 5. Restricciones de Seguridad (No Negociables)

1. **Nunca** modificar `eip712_signer.hpp`, `polymarket_order.hpp` (wire body) ni el esquema HMAC sin volver a ejecutar los vectores dorados (KAT).
2. **Nunca** imprimir en logs la clave privada, el secreto API ni firmas completas.
3. Las señales de CrowdIntel (webhook externo) son datos no confiables: siempre pasan por filtros estadísticos **y** por el risk engine; ninguna señal externa puede desactivar el kill switch.
4. Toda constante de límite de riesgo debe ser sobreescribible por variable de entorno, con default conservador.
5. Todo control nuevo debe ser de costo O(1) y branch-predicted en el hot path.
6. `ctest` completo debe pasar y no degradar la latencia P50 del hot path más de un 10%.

---

## 6. Criterios de Aceptación por Fase

| Fase | Criterio | Evidencia |
|------|----------|-----------|
| 0 | `.env` ignorado, build limpio, cliente único | `git check-ignore`, `cmake build`, `grep` |
| 1 | Rate limiter, backoff, pooling, parseo HTTP | Unit test + ctest |
| 2 | Risk engine, kill switch, balance, ventanas | Unit test + ctest |
| 3 | Order manager, position tracker, fills, anti-retry | Unit test + ctest |
| 4 | Fee model, gas, slippage, net-EV | Unit test + ctest |
| 5 | Market metadata, compliance guard, tick size | Unit test + ctest |
| 6 | Telemetry, alertas, métricas | Unit test + ctest |
| 7 | ctest verde, latencia +10%, secretos limpios, docs | ctest + benchmark + grep + review |
| Final | Todo end-to-end | `AUDIT_FINAL_REPORT.md` |

---

## 7. Protocolo de Auditoría Final

La auditoría final es el quality gate absoluto. Su protocolo:

1. **Compilación desde cero**: Limpia `build/`, recompila con `cmake --build`, verifica cero warnings.
2. **ctest completo**: Ejecuta `ctest --output-on-failure`, requiere 100% PASS.
3. **Benchmark de latencia**: Ejecuta `latency_bench`, compara P50/P99 contra baseline en `docs/PERF_METRICS.md`, tolerancia +10%.
4. **Escáner de secretos**: `grep -rn "YOUR_API_KEY\|private_key\|secret" core/ alpha/ --include="*.cpp" --include="*.hpp" --include="*.py"` → cero coincidencias de credenciales reales.
5. **Verificación criptográfica**: Ejecuta `test_signer`, verifica KAT de Keccak-256 y EIP-712 con vectores canónicos.
6. **Hot path audit**: Escanea `run_tick()` y código de firma para `std::cout`, `malloc`, `new`, `std::string` → cero en hot path.
7. **Risk engine audit**: Verifica que `kill_switch` imposibilite cualquier firma.
8. **Order lifecycle audit**: Verifica que `client_order_id` se rastree end-to-end.
9. **Compliance audit**: Verifica tick size dinámico y market state.
10. **Documentation audit**: Verifica README sin cifras fabricadas, docs alineadas.
11. **Observability audit**: Verifica audit log append-only, alertas configurables.
12. **Integridad audit**: Verifica eip712_signer.hpp no modificado.

**Resultado**: Cualquier fallo → reporta con evidencia → Director dispara reparación → itera.
