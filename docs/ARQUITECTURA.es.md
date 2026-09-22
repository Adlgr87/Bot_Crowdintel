# 🏗️ Arquitectura — Bot CrowdIntel (Polymarket CLOB V2)

> Este documento sustituye al antiguo `WORKFLOW_MAESTRO_POLYMARKET.md`
> (tablas duplicadas, cifras de benchmarks inválidos y referencias a commits
> obsoletos). La versión extendida en inglés está en
> [`ARCHITECTURE.md`](ARCHITECTURE.md); aquí va el resumen operativo.

## Estado real (medido, 22-sep-2026)

| Métrica | Objetivo | Estado | Evidencia |
|---------|----------|--------|-----------|
| Firma EIP-712 **V2** | esquema oficial | ✅ verificada byte a byte vs referencia Python | `test_signer` + `cross_check_v2.py` |
| Cuerpo JSON de `POST /order` | esquema oficial | ✅ idéntico byte a byte | `test_core::wire_body` |
| Auth L2 (POLY_* + HMAC) | esquema oficial | ✅ implementada (midstates ~0.3 µs) | KAT RFC 4231 |
| Tick del hot path (pool pre-firmado) | mínimo posible | ✅ **~90 ns P50** | `latency_bench` (TSC calibrado) |
| Tick del hot path (firma inline) | — | ~24 µs P50 en vCPU compartida 2.1 GHz | `latency_bench` |
| Feed de mercado | real | ✅ WSS RFC 6455 propio + parser probado con mensajes de la doc | `test_core::ws_market_parsing` |
| Backtester | real | ✅ replay CSV con PnL/slippage | `l2_backtester` |

**Importante**: las cifras antiguas de este repo ("8 ns", "24 ciclos",
"MutaLambda 3 mutaciones", "APPROVED FOR PRODUCTION") eran artefactos de
benchmarks inválidos o documentos ficticios; fueron eliminadas. El historial
completo de defectos y su arreglo está en
[`REMEDIATION_2026-09-22.md`](REMEDIATION_2026-09-22.md).

## Flujo (tick-to-wire)

```
WSS mercado ──seqlock──► OrderBookL2 ──┐
Señales CrowdIntel ──SPSC──┐           │
Presign (±8 ticks) ──flip──┼───────────┤
                           ▼           ▼
                    ExecutionEngine (core aislado, spin/park)
                    filtros económicos → Kelly exacto →
                    pool (~90 ns) | firma inline (~24 µs) →
                    cuerpo JSON (buffers fijos) → HMAC midstates
                           ▼
        TLS persistente → POST /order (POLY_*) → clob.polymarket.com
```

## Puntos clave

1. **Pool de órdenes pre-firmadas**: la ECDSA (el paso caro) se hace EN FRÍO
   sobre la grilla de precios plausible; el hot path solo escanea 16 slots.
2. **V2**: sin nonces — unicidad por `timestamp(ms)` + salt aleatorio (RDRAND).
   La dirección del Exchange depende de `neg_risk` (`BOT_NEG_RISK`).
3. **Cero alloc / zero syscalls** en el hot path; un solo mercado por proceso.
4. **Seguridad**: llaves solo por entorno, buffers de secretos wiped, pin TLS
   opcional (`BOT_TLS_PIN`). Pendientes para dinero real: ver `docs/STATUS.md`.

## Requisitos de despliegue

- Servidor bare-metal ideal (con `isolcpus=2,3`, desactivar C-states);
  en VPS aplicar solo la fase 1 de `infra/scripts/kernel_tuning.sh`.
- `BOT_PRIVATE_KEY_HEX`, `CLOB_API_KEY/SECRET/PASSPHRASE`, `BOT_TOKEN_ID`
  por entorno; nunca en código.
- Primera prueba con fondos: orden GTC de papel en un mercado barato y
  observar `submitted:` / `ws:` en el resumen de apagado.
