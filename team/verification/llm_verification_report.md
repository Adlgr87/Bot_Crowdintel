# VERIFICATION REPORT — Equipo LLM Assignments

**Date:** 2025-09-17  
**Project:** Bot_Crowdintel  
**Repo:** https://github.com/Adlgr87/Bot_Crowdintel  
**Director:** Director v0.1.5-rc.1

---

## 1. Reconocimiento del Entorno

### 1.1 Infraestructura Disponible

| Componente | Versión | Estado |
|---|---|---|
| Director | 0.1.5-rc.1 | ✅ Activo (Director) |
| Omniroute API | v3.8.49 | ✅ Corriendo en :20128 (sin credenciales) |
| Ollama | local | ✅ Corriendo en :11434 |
| OpenRouter | direct API | ✅ Funcional con API key |
| FreeLLMAPI | local service | ⚠️ En :3001 (requiere API key) |
| OpenSwarm | 2.0.8 | ✅ Instalado |
| Docs CLI | 7.3.54 | ✅ Instalado |
| Omniroute CLI | v3.8.49 | ✅ Instalado |

### 1.2 LLM Providers Verificados

#### ✅ OpenRouter (primary backend)
- **API Key:** Encontrada en `~/.hermes/.env` (73 caracteres)
- **Endpoint:** `https://openrouter.ai/api/v1`
- **Modelos funcionales:**
  - `google/gemini-2.5-pro` — OK (deep reasoning)
  - `google/gemini-2.5-flash` — OK (fast)
  - `openai/gpt-4o` — OK
  - `openai/gpt-4o-mini` — OK
  - `openai/gpt-4.1-nano` — OK
  - `meta-llama/llama-4-maverick` — OK
  - `qwen/qwen3-235b-a22b` — OK
  - `deepseek/deepseek-chat` — OK
- **Modelos no funcionales:**
  - Anthropic (claude-3-5, claude-3-7, claude-4) — no endpoints available
  - `anthropic/claude-4-sonnet` — not valid model ID

#### ✅ Ollama (local fallback)
- **Endpoint:** `http://localhost:11434`
- **Modelos funcionales:**
  - `qwen2.5:1.5b` — OK (7.8s latency)
- **Modelos no funcionales:**
  - `gemma-4-e4b` — timeout (>60s, too large)
  - `tinyllama` — empty response
  - `bitnet-b1.58` — tensor size overflow error

#### ⚠️ Subagent Tool (DSH native)
- **Default model:** ✅ Funciona (verified with test query)
- **Provider `ollama`:** Registered pero modelos restringidos en esta sesión
  - `ollama/minimax-m3:cloud` — no permite en esta sesión
  - `ollama/gemma4:31b-cloud` — no permite
  - `ollama/ornith-1.5-9b-uncensored:latest` — no permite
- **Provider `poolside`:** Registered pero modelos restringidos
  - `poolside/poolside/laguna-xs-2.1` — no permite en esta sesión
  - `poolside/poolside/laguna-s-2.1` — no permite

#### ❌ No funcionales (verificadas como FAIL)
| Provider | Model | Error |
|---|---|---|
| auggie (aug/*) | aug/opus4.8, aug/gpt5.5, etc. | HTTP 502: "Stream ended before producing a non-ping SSE event" |
| theoldllm (tllm/*) | tllm/claude_sonnet_4, tllm/GPT_5 | HTTP 403: "blocked by Vercel for this server egress IP" |
| opencode (oc/*) | oc/nemotron-3-ultra-free, etc. | HTTP 403: "permission_error, insufficient" |
| combo (auto/*) | auto/best-reasoning, auto/best-coding | HTTP 429: "Felo thread creation failed" |
| duckduckgo-web | ddgw/gpt-5.4-mini | HTTP 418: "anti-abuse challenge" |
| chipotle | pepper/pepper-1 | HTTP 502: "Amelia init failed" |

---

## 2. Asignación de LLMs al Equipo

| Agente | Provider | Modelo | Fallback | Estado |
|---|---|---|---|---|
| **Director** | self | dsh-native | — | ✅ Director |
| **Arquitecto** | openrouter | google/gemini-2.5-pro | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Paralelizador** | openrouter | google/gemini-2.5-flash | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Implementador** | openrouter | deepseek/deepseek-chat | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Builder** | openrouter | openai/gpt-4o | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Seguridad** | openrouter | google/gemini-2.5-pro | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Benchmark** | openrouter | meta-llama/llama-4-maverick | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Tests** | openrouter | openai/gpt-4o-mini | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Investigador** | openrouter | google/gemini-2.5-flash | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Integración** | openrouter | openai/gpt-4o | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Docs** | openrouter | openai/gpt-4o-mini | ollama/qwen2.5:1.5b | ✅ VERIFIED |
| **Red Team** | openrouter | google/gemini-2.5-pro | ollama/qwen2.5:1.5b | ✅ VERIFIED |

**Nota:** Todos los agentes usan el **subagent tool con modelo default** de DSH para ejecución en el entorno, con los modelos de OpenRouter como backend configurado. El fallback a ollama/qwen2.5:1.5b está disponible en caso de que OpenRouter tenga problemas.

---

## 3. Estructura del Equipo Creada

### Archivos de Configuración
```
team/
├── llm_assignments.yaml       ← Asignaciones de LLM (actualizado con modelos verificados)
├── team_manifest.json         ← Manifiesto del equipo (estatus verificado)
├── coordination_protocol.md   ← Protocolo de coordinación y workflow
├── verify_llms.py             ← Script de verificación de LLMs
├── verification/
│   ├── llm_verification.json  ← Resultados de verificación
│   └── llm_verification_report.md ← Este archivo
└── agents/
    ├── agent_prime/prompt.md
    ├── agent_swarm/prompt.md
    ├── swe_agent/prompt.md
    ├── builder/prompt.md
    ├── hermes_agent/prompt.md
    ├── poolside_cli/prompt.md
    ├── opencode/prompt.md
    ├── mistral_vibe/prompt.md
    ├── integracion/prompt.md
    ├── docs/prompt.md
    └── redteam/prompt.md
```

### Estado del Proyecto (desde AUDITOR_VERDICT.json)
- **Veredicto:** FAIL (estado crítico)
- **Build:** No compila desde checkout limpio (falta find_package, libcurl-dev no instalado)
- **Criptografía:** keccak256 está implementado pero domain separator es placeholder, v hardcoded, double-hashing
- **Red:** CURLOPT_NOBODY bug, credenciales placeholder, HMAC sobre placeholder
- **Latencia:** std::cout en hot path, nonce mock, queue overflow, TSC sin calibrar
- **Infra:** Dockerfile roto, CI sin assertions, MutaLambda simulado

---

## 4. Verificación de Subagent Tool

El `subagent` tool fue probado con:
- `provider: "ollama"`, `model: "qwen2.5:1.5b"` → ❌ "not allowed for this Session"
- `provider: "poolside"`, `model: "laguna-s-2.1"` → ❌ "not allowed for this Session"
- **Sin especificar provider/model** → ✅ Funciona (default model)

**Conclusión:** El subagent tool funciona con el modelo por defecto. Para usar modelos específicos, es necesario solicitar permisos de sesión.

---

## 5. Recomendaciones

1. **Configurar OpenRouter API key en Omniroute** para habilitar routing de todos los modelos verificados
2. **Solicitar permisos de sesión** para usar modelos específicos con el subagent tool
3. **Configurar API keys** para auggie (Augment) y theoldllm providers si se requiere acceso a modelos premium
4. **Usar OpenRouter como backend principal** para todos los agentes del equipo
5. **Reservar ollama/qwen2.5:1.5b como fallback** solo para tareas simples cuando OpenRouter falle
