# Agent Team Roster — Bot_Crowdintel

## Director
| Agente | Provider | Modelo | Estado |
|---|---|---|---|
| **Director** | self | dsh-native | ✅ Active |

## Equipo de Trabajo
| # | Agente | Rol | Provider | Modelo | Fallback | Estado |
|---|---|---|---|---|---|---|
| 1 | **Agent Prime** | Arquitectura, contratos, ADR | openrouter | google/gemini-2.5-pro | ollama/qwen2.5:1.5b | ✅ Verified |
| 2 | **Agent Swarm** | Paralelización, tracks A-H | openrouter | google/gemini-2.5-flash | ollama/qwen2.5:1.5b | ✅ Verified |
| 3 | **SWE Agent** | Implementación C++/CMake | openrouter | deepseek/deepseek-chat | ollama/qwen2.5:1.5b | ✅ Verified |
| 4 | **OpenHands** | Build, CI, Docker | openrouter | openai/gpt-4o | ollama/qwen2.5:1.5b | ✅ Verified |
| 5 | **Hermes Agent** | Seguridad, criptografía | openrouter | google/gemini-2.5-pro | ollama/qwen2.5:1.5b | ✅ Verified |
| 6 | **Poolside CLI** | Performance, benchmarks | openrouter | meta-llama/llama-4-maverick | ollama/qwen2.5:1.5b | ✅ Verified |
| 7 | **OpenCode** | Tests, refactors | openrouter | openai/gpt-4o-mini | ollama/qwen2.5:1.5b | ✅ Verified |
| 8 | **Mistral Vibe** | Investigación, alternativas | openrouter | google/gemini-2.5-flash | ollama/qwen2.5:1.5b | ✅ Verified |
| 9 | **OpenClaw** | Revisión de integraciones | openrouter | openai/gpt-4o | ollama/qwen2.5:1.5b | ✅ Verified |
| 10 | **KiloCode** | Documentación, runbooks | openrouter | openai/gpt-4o-mini | ollama/qwen2.5:1.5b | ✅ Verified |
| 11 | **ClaudeCode** | Crítica independiente, red team | openrouter | google/gemini-2.5-pro | ollama/qwen2.5:1.5b | ✅ Verified |

## Estructura de Archivos del Equipo
```
team/
├── TEAM_ROSTER.md                    ← Este archivo
├── llm_assignments.yaml              ← Asignaciones LLM detalladas
├── team_manifest.json                ← Manifiesto del equipo (JSON)
├── coordination_protocol.md          ← Protocolo de coordinación
├── verify_llms.py                    ← Script de verificación de LLMs
├── verification/
│   ├── llm_verification.json         ← Resultados automatizados
│   └── llm_verification_report.md    ← Reporte de verificación completo
└── agents/
    ├── agent_prime/
    │   └── prompt.md                 ← Instrucciones del Agente Prime
    ├── agent_swarm/
    │   └── prompt.md                 ← Instrucciones del Agent Swarm
    ├── swe_agent/
    │   └── prompt.md                 ← Instrucciones del SWE Agent
    ├── openhands/
    │   └── prompt.md                 ← Instrucciones de OpenHands
    ├── hermes_agent/
    │   └── prompt.md                 ← Instrucciones de Hermes Agent
    ├── poolside_cli/
    │   └── prompt.md                 ← Instrucciones de Poolside CLI
    ├── opencode/
    │   └── prompt.md                 ← Instrucciones de OpenCode
    ├── mistral_vibe/
    │   └── prompt.md                 ← Instrucciones de Mistral Vibe
    ├── openclaw/
    │   └── prompt.md                 ← Instrucciones de OpenClaw
    ├── kilocode/
    │   └── prompt.md                 ← Instrucciones de KiloCode
    └── claudecode/
        └── prompt.md                 ← Instrucciones de ClaudeCode
```

## Workflow Tool Assignment
The `workflow` tool is available for multi-agent orchestration (Agent Swarm role).
The `subagent` tool is used for individual agent invocations.
The `ralph` tool is available for iterative fresh-agent execution.

## Estado de Verificación
- **LLMs totales verificadas:** 8 modelos funcionales en OpenRouter + 1 en Ollama local
- **Agentes verificados:** 11/11 (todos con fallback funcional)
- **Proveedores rotos:** auggie (502), theoldllm (403), opencode (403), combo (429)
- **Estado de Phase 0:** En progreso
