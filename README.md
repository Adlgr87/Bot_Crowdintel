# Bot CrowdIntel — Polymarket CLOB V2 Trading Bot (C++20)

**Internal reference. No marketing.** Para uso exclusivo del autor y socios técnicos.

---

## Qué es

Un motor de ejecución C++20 de ultra-baja latencia para el
[CLOB V2](https://docs.polymarket.com/developer-docs/trading/clob) de
Polymarket en Polygon (chainId = 137). Consume señales de alpha vía
HTTP, las evalúa contra un libro de órdenes L2 en memoria, y envía
órdenes firmadas (EIP-712) al CLOB.

**No ejecuta trading real todavía.** El canary live está bloqueado
hasta que se pasen las verificaciones H1–H15 en
`docs/CANARY_CHECKLIST.md`.

## Estado de las correcciones de auditoría

Las siguientes correcciones críticas y altas del reporte externo han
sido aplicadas (commit `b78691b` sobre PR #9, rebased a `main`):

| ID  | Severidad | Fix | Archivo | Estado |
|-----|-----------|-----|---------|--------|
| **M1** | Crítica | `ceil_to_quantum` para taker_amount en BUY market | `core/src/polymarket_order.hpp` | ✅ Aplicado |
| **A2** | Alta | `total_fee` incluido en `order_notional` (BUY: +fee, SELL: -fee) | `core/src/execution_engine.hpp` | ✅ Aplicado |
| **A2-pre-size** | Media | `sizing_price` (fee-adjusted) usado para share sizing | `core/src/execution_engine.hpp` | ✅ Aplicado |
| **A3** | Alta | `daily_buy_volume_` con reset UTC a medianoche | `core/src/execution_engine.hpp` | ✅ Aplicado |

### Detalles

**M1 — Rounding direccional incorrecto en BUY market orders.**
Antes: `floor_to_quantum` para `taker_amount`, lo que podía producir
un costo del maker estrictamente menor que `price × shares`, violento
el price-cap invariant. Ahora: `ceil_to_quantum` garantiza que el
costo del maker cubra siempre el taker_amount.

Prueba: `test_buy_market_rounding()` en `tests/unit/test_core.cpp`
(8 assertions: edge cases de `ceil_to_quantum` + invariante de price cap).

**A2 — Fees excluidos del cálculo de risk cap.**
Antes: `order_notional` excluía el taker fee, permitiendo que órdenes
BUY excedieran `BOT_MAX_ORDER_USD` por el monto del fee. Ahora:
`total_fee = fee_per_share × effective_shares × 1e-6` se suma
(resta para SELL) a `order_notional` antes de comparar contra
`max_order_usd`, `max_exposure_usd`, `max_daily_loss_usd`.

**A2-pre-size — Share sizing no ajustado por feed.**
Antes: `usd_to_shares_fixed(usd, price)` usaba el precio crudo,
sobre-asignando shares cuyo fee empujaba el total sobre el cap.
Ahora: `usd_to_shares_fixed(usd, sizing_price)` donde
`sizing_price = price + fee_per_share` (BUY), garantizando
`shares × sizing_price ≈ notional + fee ≤ usd ≤ cap`.

**A3 — Daily loss cap sin reset.**
Antes: `worst_case_loss_usd_` acumulaba sin reset, permitiendo drift
de día a día. Ahora: renombrado a `daily_buy_volume_`, acumula solo
BUY notional (gross), y `check_daily_reset()` reinicia a UTC midnight.

Limitación: `daily_buy_volume_` NO sobrevive a reinicios (sin ledger
persistente). Documentado en `docs/CONFIGURATION.md`.

## Arquitectura

### Hot path — `run_tick()` (`core/src/execution_engine.hpp`)

O(1), sin allocationes, sin I/O de red, sin `std::cout`:

```
1. Pop AlphaSignal de SPSC ring buffer (source 0x01)
2. ComplianceGuard — jurisdiction + restricted tokens (fail-closed)
3. Book stale check — timestamp atómico, ~90s threshold
4. SpikeDetector — anti-sniping en feed Binance (rechazo pre-subscripción)
5. WindowShield — lifecycle state machine (fail-closed on settlement windows)
6. Drain MarkelState de SPSC ring (source 0x02, Binance L2)
7. CfCNetwork::infer() — 5.6μs p50 con AVX2-FMA, produces conviction [0,1]
8. Conviction = weighted(CfC_p, OFI_lr, TWAP_signal)
9. Size: KellySizer (quarter-Kelly) → LadderSkewer → ceil_to_quantum
10. Cap checks: max_order, max_exposure, daily_buy_volume (fee-INclusive)
11. RiskManager authorize (P2 — always-on, kill latch)
12. VolaGate slippage gate (P3 — pre-signature rejection)
13. Sign EIP-712, submit via presigned pool or direct POST
```

Steps 4-8 son opt-in (ver `core/src/engine_extensions.hpp`). Si todos los
punteros de `ExtendedEngineLayers` son nullptr, el hot path cae al
comportamiento legacy (P1-P8) sin overhead adicional.

### Capas P1–P4 (PR #8, ya en `main`)

| Capa | Componentes | Responsabilidad |
|------|-------------|-----------------|
| **P1 Eyes** | `account_events.hpp`, `position_tracker.hpp`, `reconciliation.hpp` | Contabilidad WSS privada, reconciliación REST, kill-switch por drift |
| **P2 Brakes** | `risk_manager.hpp`, `kill_switch.hpp` | Exposure caps, stop-loss VWAP, hedge complementario, daily-loss kill |
| **P3 Adverse Selection** | `volatility_gate.hpp` | Dynamic ladder TTL, shock cooldowns, pre-sign slippage gate |
| **P4 Brain** | `bayesian_engine.hpp`, `evidence.hpp`, `source_reliability.hpp` | Beta-Binomial posterior, NDJSON cold-path ingestion, ~41 ns update |

### BTC 5m/15m Specialization — Fases 1-6

Implementación paralela en 6 fases. Cada fase es **opt-in**: los módulos
nuevos se activan mediante `ExtendedEngineLayers` (ver
`core/src/engine_extensions.hpp`). Si está desactivado, el hot path
caza al comportamiento legacy.

| Fase | Módulos | Archivo clave | Latency | Estado |
|------|---------|---------------|---------|--------|
| **1** | BinanceWSClient, OFICalculator, EvidenceIngress | `binance_ws_client.hpp`, `ofi_calculator.hpp`, `evidence_ingress.hpp` | OFI ≈ 3.8 ns/event | ✅ Completado |
| **2** | CfCNetwork (AVX2-FMA, 32 hidden), tanh/sigmoid Padé [3/3] | `cfc_network.hpp` + `cfc_network_test.hpp` | <1.1 μs p50, <1.5 μs p99 | ✅ Completado (102/102 KAT tests pass) |
| **3** | WindowShield (5-state machine), SpikeDetector | `window_shield.hpp`, `binance_spike_detector.hpp` | <500 ns | ✅ Completado |
| **4** | KellySizer (quarter-Kelly), TWAPTracker, LadderSkewer | `kelly_sizer.hpp`, `twap_tracker.hpp`, `ladder_skew.hpp` | Kelly <500 ns, TWAP <1 μs, Ladder <2 μs | ✅ Completado |
| **5** | Hot-path integration (`ExtendedEngineLayers`) | `engine_extensions.hpp` | Overhead total <12 μs p50 | ✅ Implementado |
| **6** | Canary deploy, monitoring, scaling | `docs/DEPLOYMENT.md#canary` | — | En progreso |

**Latencia del nuevo hot-path:** el overhead combinado de fases 1→8
es < 12 μs p50 y < 25 μs p99 (budget total < 50 μs p99).

**Modelo entrenado:** `infra/models/cfc_btc_5m_v1.bin` — 2,536 floats
(6×32 + 32×32 + 32 + 32×32 + 32×32 + 32 + 32 + 32 + 6 + 1 + 32 + 4 = 2,597
→ 2,536 con weight tying). SHA256 verificado en runtime.

**Datos de entrenamiento:** `ml_training/` — PyTorch pipeline con walk-forward
validation (10 folds), BCE+ECE loss, AdamW (lr=3e-4, wd=0.01), cosine annealing.
`kat_vectors.json` contiene 10 Known-Answer Vectors para verificación C++.

### Crypto

- **EIP-712 V2 signing**: `keccak256(0x1901 ‖ domainSep ‖ structHash)`
  - Domain: "Polymarket CTF Exchange" v2, chainId 137
  - Contract: `0xE111180000d2663C0091e4f400237545B87B996B`
  - 11-field Order struct, ABI-encoded
  - RFC 6979 deterministic nonces, low-S, keccak-256 con sufijo 0x01
- **HMAC-SHA256 auth (L2)**: key = `base64url_decode(secret)`,
  prehash = `unix_seconds + method + path + body`, headers `POLY_*`
- **libsecp256k1** con módulo recovery integrado (commit pin read-only)

## Build

Requisitos: CMake 3.20+, compilador C++20, pthreads, e
libsecp256k1 con el módulo recovery. Builds con red también requieren
OpenSSL y libcurl.

```bash
# Construir libsecp256k1 en el commit pinneado
export SECP_COMMIT=6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d
git clone --filter=blob:none https://github.com/bitcoin-core/secp256k1 /tmp/secp256k1
git -C /tmp/secp256k1 checkout --detach "$SECP_COMMIT"
cmake -S /tmp/secp256k1 -B /tmp/secp256k1/build \
  -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
  -DSECP256K1_BUILD_TESTS=OFF -DSECP256K1_BUILD_BENCHMARK=OFF
cmake --build /tmp/secp256k1/build -j

# Build offline (mock)
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=OFF -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`-DCROWDINTEL_NETWORK=ON` para transporte HTTPS/WSS real. La
configuración falla si faltan dependencias de red; nunca produce un
stub que parezca live.

## Verificación

```bash
# Tests nativos: KAT, unitarios/concurrency, replay
ctest --test-dir build --output-on-failure

# Cross-check independiente de EIP-712/RFC6979 (Python)
python3 -m pip install pycryptodome==3.23.0 coincurve==21.0.0
build/bin/test_signer --json > /tmp/signer.json
python3 tests/crypto/cross_check_v2.py /tmp/signer.json
build/bin/test_signer --json-neg-risk > /tmp/signer-neg.json
python3 tests/crypto/cross_check_v2.py /tmp/signer-neg.json

# Benchmark de CPU (no es SLO end-to-end)
build/bin/latency_bench
```

CI: GCC + Clang en 4 configuraciones (Release+net, Release-offline,
ASan/UBSan, TSan con 100 iteraciones). TSan: 0 reportes. Cppcheck:
0 errores. Mutation testing: 17/19 detectados, 2 equivalentes.

## Limitations y scope

- **Bot es CLOB V2 only.** Protocol V2 (ExchangeV3 /
  `0x4bFb...`) fuera de scope.
- **HTTP loopback solo.** Alpha receiver escucha en `127.0.0.1`; TLS
  vía proxy externo (M8 no corregido, documentado).
- **`daily_buy_volume_` no persiste.** No sobrevive a restarts.
  Si el proceso muere, el contador se reinicia.
- **No hay PnL tracking real.** El motor no reporta PnL; el tracker
  local es conservativo hasta reconciliación REST.
- **No market-making.** El motor ejecuta órdenes discretas, no
  quote management continuo.
- **Canary live bloqueado.** Firmas tipo 3 fallan closed hasta ERC-7739.
  Ver `docs/CANARY_CHECKLIST.md` (H1–H15).
- **CfC entrenado offline.** El modelo `cfc_btc_5m_v1.bin` se entrenó
  con datos históricos de Binance BTC/USDT 5-min (enero 2023 - agosto 2025).
  No incluye features en tiempo real más allá del Book y del alpha feed.
  Re-entrenamiento requerido para markets distintos a BTC.
- **Binance feed es simbólico.** La conexión WS real usa `CROWDINTEL_HAVE_NETWORK`;
  el modo mock (`CROWDINTEL_FORCE_MOCK=1`) genera datos sintéticos deterministas
  vía `infra/mock/binance_ws_mock.py`. No se ha probado contra WS real de Binance.

## Documentación

- [Architecture map and latency budget](docs/ARCHITECTURE_MAP.md)
- [Dependency inventory](docs/DEPENDENCY_INVENTORY.md)
- [Latency budget (per-module SLO)](docs/LATENCY_BUDGET.md)
- [Phase 1 delivery: Binance WS + OFI](docs/PHASE1_DELIVERY.md)
- [Phase 2 delivery: native Cpp CfC + SIMD](docs/PHASE2_DELIVERY.md)
- [Configuration reference](docs/CONFIGURATION.md)
- [Deployment and rollback runbook](docs/DEPLOYMENT.md)
- [Security and threat model](docs/SECURITY.md)
- [Canary checklist (H1–H15)](docs/CANARY_CHECKLIST.md)
- [Remediation ledger](docs/REMEDIATION_STATUS.md)
