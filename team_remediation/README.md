# 🛡️ Team Agent de Workflows — Remediación de Cumplimiento

> **Repo objetivo**: `Adlgr87/Bot_Crowdintel` (rama `main`, CLOB V2)
> **Workflow origen**: `WORKFLOW_REMEDIACION_CUMPLIMIENTO.md`

## 🎯 Propósito

Elevar el bot de Polymarket de "técnicamente correcto pero desprotegido" a
"operable con capital real" mediante 8 fases de remediación:

| # | Agente | Fase | Responsabilidad | Branch |
|---|--------|------|-----------------|--------|
| 1 | AGENTE_BASELINE | 0 | Línea base, higiene, secretos | `remediation/00-baseline` |
| 2 | AGENTE_API | 1 | Rate limiter, backoff, pooling, HTTP | `remediation/10-api-ratelimit` |
| 3 | AGENTE_RIESGO | 2 | Risk engine, kill switch, balance | `remediation/20-risk-engine` |
| 4 | AGENTE_ORDENES | 3 | Order manager, positions, fills | `remediation/30-order-manager` |
| 5 | AGENTE_ECON | 4 | Fee model, gas, slippage, net-EV | `remediation/40-fee-model` |
| 6 | AGENTE_COMPLIANCE | 5 | Market metadata, compliance guard | `remediation/50-compliance-guard` |
| 7 | AGENTE_OBS | 6 | Telemetry, alertas, métricas | `remediation/60-telemetry` |
| 8 | AGENTE_QA | 7 | Tests, regresión, docs | `remediation/70-qa-docs` |
| 9 | **AGENTE_VERIFICADOR_FINAL** | Final | Verificación de principio a fin | — |

## 📁 Estructura

```
team_remediation/
├── REMEDIACION_TEAM_MANIFEST.json          ← Manifiesto del equipo (8 agentes + 1 verificador)
├── REMEDIACION_COORDINATION_PROTOCOL.md     ← Protocolo de coordinación y reglas
├── REMEDIACION_WORKFLOW.md                  ← Workflow detallado fase por fase
├── README.md                                ← Este archivo
├── agents/
│   ├── agente_baseline/prompt.md
│   ├── agente_api/prompt.md
│   ├── agente_riesgo/prompt.md
│   ├── agente_ordenes/prompt.md
│   ├── agente_econ/prompt.md
│   ├── agente_compliance/prompt.md
│   ├── agente_obs/prompt.md
│   ├── agente_qa/prompt.md
│   └── agente_verificador_final/prompt.md   ← Agente de verificación extremadamente riguroso
└── verification/
    └── (VERIFICACION_FINAL_REPORT.md generado al final)
```

## 🔄 Orden de Ejecución

```
Fase 0 → [Fase 1 ‖ Fase 2 ‖ Fase 4 ‖ Fase 5] → Fase 3 → Fase 6 → Fase 7 → AGENTE_VERIFICADOR_FINAL
```

## 🛡️ Restricciones de Seguridad (No Negociables)

1. **Nunca** modificar `eip712_signer.hpp`, `polymarket_order.hpp` (wire body) ni el esquema HMAC sin volver a ejecutar los vectores dorados (KAT).
2. **Nunca** imprimir en logs la clave privada, el secreto API ni firmas completas.
3. Las señales de CrowdIntel (webhook externo) son datos no confiables: siempre pasan por filtros estadísticos **y** por el risk engine; ninguna señal externa puede desactivar el kill switch.
4. Toda constante de límite de riesgo debe ser sobreescribible por variable de entorno, con default conservador.
5. Todo control nuevo debe ser de costo O(1) y branch-predicted en el hot path.
6. `ctest` completo debe pasar y no degradar la latencia P50 del hot path más de un 10%.

## ✅ Definition of Done

- [ ] `.env` excluido de git; sin secretos en el repo
- [ ] Un solo cliente HTTP, con pooling, rate limiter y backoff 429/5xx
- [ ] `RiskEngine.pre_trade_check` ejecutado antes de firmar toda orden
- [ ] Kill switch funcional (detiene envío y cancela órdenes abiertas)
- [ ] Balance USDC/POL verificado; posición reconciliada con fills del user-channel
- [ ] Sin reintentos ciegos: toda orden con estado rastreado por `client_order_id`
- [ ] Edge neto (descuenta fees+gas+slippage) decide si se opera
- [ ] Mercado inactivo/restringido → bloqueo; tick size dinámico por mercado
- [ ] Audit log append-only + alertas configurables
- [ ] `ctest` verde, latencia dentro de +10% de la línea base, docs actualizadas
- [ ] AGENTE_VERIFICADOR_FINAL reporta 100% PASS
