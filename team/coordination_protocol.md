# Protocolo de Coordinacion del Agent Team

> **Bot_Crowdintel — Director Director**

---

## 1. Regla de Oro

> **Un agente propone, otro implementa, otro prueba, otro critica y un supervisor independiente decide si se acepta.**

Ningun agente se autocertifica. Cada entregable requiere validacion cruzada.

---

## 2. Director: Director (Inamovible)

**NO acepta** frases como:
- "parece correcto"
- "compila en mi entorno"
- "mejoró el promedio"
- "el benchmark dice 20 ciclos"
- "el agente anterior lo revisó"

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
Director (Director)
    |
    +-- Agent Prime       -- Arquitectura, contratos, ADR
    +-- Agent Swarm       -- Paralelizacion, grafo de dependencias
    +-- SWE Agent         -- Implementacion C++/CMake
    +-- OpenHands         -- Build, CI, Docker, sanitizers
    +-- Hermes Agent      -- Seguridad, criptografia, adversarial
    +-- Poolside CLI      -- Benchmarks, profiling, TSC
    +-- OpenCode          -- Tests, refactors
    +-- Mistral Vibe      -- Investigacion, alternativas
    +-- OpenClaw          -- Integracion, secretos, operacion nocturna
    +-- KiloCode          -- Runbooks, documentacion
    +-- ClaudeCode        -- Critica independiente, red team
```

---

## 4. Workflow por Fases

### Fase 0 — Baseline y congelacion
**Agentes:** Director, Agent Prime, ClaudeCode, OpenHands
**Salida:** BASELINE.md, RISK_REGISTER.md, ACCEPTANCE_GATES.md

### Fase 1 — Build reproducible
**Gate:** cmake configure PASS, build limpio PASS, ctest PASS, ASan PASS, UBSan PASS, Docker build PASS

### Fase 2 — Seguridad y criptografia
**Gate:** vectores conocidos PASS, verificacion cruzada PASS, fuzzing PASS, secrets scan PASS, security review PASS

### Fase 3 — Motor de riesgo y estado
**Gate:** replay normal/corrupto PASS, desconexion PASS, reinicio PASS, duplicados PASS, fuera de orden PASS

### Fase 4 — Cliente CLOB
**Gate:** no ordenes duplicadas, no payloads inconsistentes, no bloqueo indefinido, no secretos en logs

### Fase 5 — Replay y paper trading
**Agentes:** OpenCode, Mistral Vibe, Poolside CLI, Agent Prime

### Fase 6 — Benchmark serio
**Gate:** benchmark reproducible, mismo resultado estadistico, sin cola vacia dominante, sin errores ignorados

### Fase 7 — AF_XDP
**Agentes:** Agent Prime, SWE Agent, Poolside CLI, OpenHands, OpenClaw

### Fase 8 — MutaLambda
**Agentes:** Poolside CLI, Mistral Vibe, SWE Agent, ClaudeCode
**Targets bloqueados:** EIP-712, Keccak, riesgo, nonce, reconciliacion, cancelacion, secretos

### Fase 9 — Revision adversarial total
**Agentes:** ClaudeCode, Hermes Agent, OpenClaw
**Salida:** FINAL_RED_TEAM_REPORT.md

### Fase 10 — Certificacion y despliegue
**Salida:** release candidate, SBOM, hash de binario, configuracion versionada, runbook, plan de rollback

---

## 5. Reglas de Coordinacion

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
Cada claim debe apuntar a: test, log, benchmark, commit, dataset, configuracion, o hash.

### 5.4 Dos revisores para cambios criticos
Areas: criptografia, riesgo, cliente de ordenes, reconciliacion, secretos, transporte, apagado de emergencia.

Requiere: implementador + Hermes Agent + supervisor.

### 5.5 Branches aisladas
- Cada agente trabaja en su propia rama: `agents/<agent-name>/<task>`
- Director controla merge a `main`
- ClaudeCode hace review de PRs antes de merge
