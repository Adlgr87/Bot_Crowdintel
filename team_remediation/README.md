# 🛡️ Remediación de Cumplimiento

> **Repo objetivo**: `Adlgr87/Bot_Crowdintel` (rama `main`, CLOB V2)
> **Workflow origen**: `WORKFLOW_REMEDIACION_CUMPLIMIENTO.md`

## 🎯 Propósito

Elevar el bot de Polymarket de "técnicamente correcto pero desprotegido" a
"operable con capital real" mediante 8 fases de remediación:

| # | Área | Fase | Responsabilidad | Branch |
|---|------|------|-----------------|--------|
| 1 | Baseline | 0 | Línea base, higiene, secretos | `remediation/00-baseline` |
| 2 | API | 1 | Rate limiter, backoff, pooling, HTTP | `remediation/10-api-ratelimit` |
| 3 | Risk | 2 | Risk engine, kill switch, balance | `remediation/20-risk-engine` |
| 4 | Orders | 3 | Order manager, positions, fills | `remediation/30-order-manager` |
| 5 | Economics | 4 | Fee model, gas, slippage, net-EV | `remediation/40-fee-model` |
| 6 | Compliance | 5 | Market metadata, compliance guard | `remediation/50-compliance-guard` |
| 7 | Observability | 6 | Telemetry, alertas, métricas | `remediation/60-telemetry` |
| 8 | QA | 7 | Tests, regresión, docs | `remediation/70-qa-docs` |
| 9 | **Final Audit** | Final | Verificación de principio a fin | — |

## 📁 Estructura

```
team_remediation/
├── REMEDIACION_TEAM_MANIFEST.json          ← Manifiesto del proyecto
├── REMEDIACION_COORDINATION_PROTOCOL.md     ← Protocolo de coordinación y reglas
├── REMEDIACION_WORKFLOW.md                  ← Workflow detallado fase por fase
├── README.md                                ← Este archivo
└── verification/
    └── AUDIT_FINAL_REPORT.md                ← Reporte de auditoría final
```

## 🔄 Orden de Ejecución

```
Fase 0 → [Fase 1 ‖ Fase 2 ‖ Fase 4 ‖ Fase 5] → Fase 3 → Fase 6 → Fase 7 → Auditoría Final
```

## 🛡️ Restricciones de Seguridad (No Negociables)

1. **Nunca** modificar `eip712_signer.hpp`, `polymarket_order.hpp` (wire body) ni el esquema HMAC sin volver a ejecutar los vectores dorados (KAT).
2. **Nunca** imprimir en logs la clave privada, el secreto API ni firmas completas.
3. Las señales de CrowdIntel (webhook externo) son datos no confiables: siempre pasan por filtros estadísticos **y** por el risk engine; ninguna señal externa puede desactivar el kill switch.
4. Toda constante de límite de riesgo debe ser sobreescribible por variable de entorno, con default conservador.
5. Todo control nuevo debe ser de costo O(1) y branch-predicted en el hot path.
6. `ctest` completo debe pasar y no degradar la latencia P50 del hot path más de un 10%.

## ✅ Definition of Done

- [x] `.env` excluido de git; sin secretos en el repo
- [x] Un solo cliente HTTP, con pooling, rate limiter y backoff 429/5xx
- [x] `RiskEngine.pre_trade_check` ejecutado antes de firmar toda orden
- [x] Kill switch funcional (detiene envío y cancela órdenes abiertas)
- [x] Balance USDC/POL verificado; posición reconciliada con fills del user-channel
- [x] Sin reintentos ciegos: toda orden con estado rastreado por `client_order_id`
- [x] Edge neto (descuenta fees+gas+slippage) decide si se opera
- [x] Mercado inactivo/restringido → bloqueo; tick size dinámico por mercado
- [x] Audit log append-only + alertas configurables
- [x] `ctest` verde, latencia dentro de +10% de la línea base, docs actualizadas
- [x] Auditoría final reporta 100% PASS (12/12 niveles)
