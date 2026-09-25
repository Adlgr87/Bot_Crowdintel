# Protocolo de Coordinación del Proyecto

> **Bot_Crowdintel — Proyecto de Código Abierto**

---

## 1. Regla de Oro

> **Quien propone, otro implementa; quien implementa, otro prueba; quien prueba,
> otro critica; y un supervisor independiente decide si se acepta.**

Ningún entregable se autocertifica. Cada uno requiere validación cruzada.

---

## 2. Supervisor (Inamovible)

**NO acepta** frases como:
- "parece correcto"
- "compila en mi entorno"
- "mejoró el promedio"
- "el benchmark dice X ciclos"
- "el revisor anterior lo revisó"

**Exige siempre:**
- comando ejecutado
- commit o patch
- resultado reproducible
- logs
- métricas
- fallos conocidos
- riesgos restantes

---

## 3. Estructura del Equipo

```
Supervisor
    |
    +-- Arquitectura          — Contratos, ADR
    +-- Paralelización         — Grafo de dependencias
    +-- Implementación         — C++/CMake
    +-- Build & CI            — Build, CI, Docker, sanitizers
    +-- Seguridad             — Criptografía, seguridad adversarial
    +-- Benchmarking          — Benchmarks, profiling, TSC
    +-- Tests                 — Tests, refactors
    +-- Investigación         — Alternativas, análisis
    +-- Integración           — Integración, secretos, operación
    +-- Documentación         — Runbooks, documentación
    +-- Red Team             — Crítica independiente
```

---

## 4. Workflow por Fases

### Fase 0 — Baseline
**Salida:** BASELINE.md, RISK_REGISTER.md

### Fase 1 — Build reproducible
**Gate:** cmake configure PASS, build limpio PASS, ctest PASS, ASan PASS, UBSan PASS

### Fase 2 — Seguridad y criptografía
**Gate:** vectores conocidos PASS, verificación cruzada PASS, secrets scan PASS

### Fase 3 — Motor de riesgo y estado
**Gate:** replay normal/corrupto PASS, reconexión PASS, reinicio PASS, duplicados PASS

### Fase 4 — Cliente CLOB
**Gate:** no órdenes duplicadas, no payloads inconsistentes, no bloqueo indefinido

### Fase 5 — Replay y paper trading
**Salida:** resultados de validación en ambiente de prueba

### Fase 6 — Benchmark
**Gate:** benchmark reproducible, mismo resultado estadístico, sin errores ignorados

### Fase 7 — Optimización
**Targets bloqueados:** EIP-712, Keccak, riesgo, nonce, reconciliación

### Fase 8 — Revisión adversarial
**Salida:** FINAL_RED_TEAM_REPORT.md

### Fase 9 — Certificación y despliegue
**Salida:** release candidate, SBOM, hash de binario, configuración versionada

---

## 5. Reglas de Coordinación

### 5.1 No editar sobre cambios no validados
Cada tarea debe tener:
- entrada
- salida esperada
- archivos permitidos
- tests obligatorios
- criterios de rechazo

### 5.2 Separar cambios funcionales y de rendimiento
```
fix: correct EIP-712 domain separator
test: add EIP-712 known-answer vector
perf: optimize order book lookup
infra: add AF_XDP backend
```

### 5.3 No aceptar afirmaciones sin artefacto
Cada claim debe apuntar a: test, log, benchmark, commit, dataset, configuración, o hash.

### 5.4 Dos revisores para cambios críticos
Áreas: criptografía, riesgo, cliente de órdenes, reconciliación, secretos, transporte, apagado de emergencia.

Requiere: implementador + Supervisor independiente.

### 5.5 Branches aisladas
- Cada fase trabaja en su propia rama
- Supervisor controla merge a `main`
- Revisión de PRs antes de merge
