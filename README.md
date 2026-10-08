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

## Enfoque de señal: DSH (Deterministic Signal)

El motor principal de señalación ha migrado de **CfC (neural network)** a **matemática determinista**
según el workflow DSH.md. No hay ML en el hot-path. La decisión de trading se basa
exclusivamente en:

- **Brownian Bridge TWAP**: P(TWAP_final > K) = Φ(d), d = (A_t·t + S_t·τ − K·T) / (σ·√(τ³/3))
  — implementado en `twap_brownian_bridge.hpp` usando la aproximación de Abramowitz-Stegun 7.1.26
  (error < 1.5×10⁻⁷).
- **Volatilidad EWMA multi-escala**: λ_fast=0.94, λ_slow=0.98, detección de régimen NORMAL/SHOCK/CALM.
- **OFI lineal (Cont et al. 2014)**: Order Flow Imbalance con EMA y z-score thresholds.
- **Rate limiting + Circuit Breaker**: TokenBucket con exponiexponential backoff.
- **Kelly sizing**: quarter-Kelly, cap al 3% del bankroll.
- **Ladder + TimeStrategy**: construcción de book de liquidez con TTL.
- **WindowShield**: state machine 5-estados (MAKER_PASSIVE → MAKER_SKEWED → DIRECTIONAL → CLOSE_ONLY → HALTED).

El CfC (`cfc_network.hpp`) sigue presente como componente secundario opcional en el
código legacy, pero **no participa en el hot-path DSH**. Su entrenamiento offline y
modelo `.bin` se conservan para referencia histórica.

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

### Hot path — DSH tick loop (`core/src/engine_extensions.hpp`)

O(1), sin allocations, sin I/O de red, sin `std::cout`. 13 steps:

```
1.  Pop AlphaSignal de SPSC ring buffer (source 0x01)
2.  ComplianceGuard — jurisdiction + restricted tokens (fail-closed)
3.  Book stale check — timestamp atómico, ~90s threshold
4.  SpikeDetector — anti-sniping en feed Binance (rechazo pre-subscripción)
5.  WindowShield — state machine (fail-closed on Close-Only windows)
6.  Drain MarketState de SPSC ring (source 0x02, Binance L2)
7.  VolatilityEstimator.on_log_return() — actualiza σ con EWMA multi-escala
8.  TwapBrownianBridge.on_price() + compute() — P(Up) via Brownian Bridge
9.  OfiLinearFilter.on_book_update() — OFI + z-score (pressure level)
10. RateLimiter.can_send() + CircuitBreaker.allow() — fail-closed
11. KellySizer.compute() — quarter-Kelly, edge ≥ 0.5% requerido
12. LadderBuilder.build() + TimeStrategy — quotes con TTL
13. Sign EIP-712, submit via HTTPS (2s timeout)
```

El hot path DSH reemplaza al legacy CfC. `ExtendedEngineLayers` expone
punteros a los módulos P1-P4; si todos son nullptr, cae al comportamiento
legacy sin overhead. El nuevo hot-path overhead combinado es < 12μs p50.

### Capas P1–P4 (PR #8, ya en `main`)

| Capa | Componentes | Responsabilidad |
|------|-------------|-----------------|
| **P1 Eyes** | `account_events.hpp`, `position_tracker.hpp`, `reconciliation.hpp` | Contabilidad WSS privada, reconciliación REST, kill-switch por drift |
| **P2 Brakes** | `risk_manager.hpp`, `kill_switch.hpp` | Exposure caps, stop-loss VWAP, hedge complementario, daily-loss kill |
| **P3 Adverse Selection** | `volatility_gate.hpp` | Dynamic ladder TTL, shock cooldowns, pre-sign slippage gate |
| **P4 Brain** | `bayesian_engine.hpp`, `evidence.hpp`, `source_reliability.hpp` | Beta-Binomial posterior, NDJSON cold-path ingestion, ~41 ns update |

### DSH Specialization — Fases 1-6

Implementación determinista en 6 fases (6-phase checklist en workflow DSH.md
líneas 1363-1419). Cada fase es **opt-in**: los módulos nuevos se activan
mediante `ExtendedEngineLayers` (ver `core/src/engine_extensions.hpp`). Si está
desactivado, el hot path cae al comportamiento legacy sin overhead.

| Fase | Componentes | Archivo clave | Latency (p50) | Tests | Estado |
|------|-------------|---------------|--------------|-------|--------|
| **P1** | TwapBrownianBridge + VolatilityEstimator | `twap_brownian_bridge.hpp`, `volatility_estimator.hpp` | BB compute: 18 ns<br>vol_on_log_return: 8 ns | 24/24 + 11/11 | ✅ Completado |
| **P2** | OfiLinearFilter + WindowShield (mod) | `ofi_linear_filter.hpp`, `window_shield.hpp` | OFI eval: <100 ns<br>WindowShield: <200 ns | 13/13 + 12/12 | ✅ Completado |
| **P3** | RateLimiter + CircuitBreaker + RequestPrioritizer | `rate_limiter.hpp`, `circuit_breaker.hpp`, `request_prioritizer.hpp` | TokenBucket: O(1) | 14 tests | ✅ Completado |
| **P4** | FeeCalculator + KellySizer + LadderBuilder + TimeStrategy | `fee_calculator.hpp`, `kelly_sizer.hpp`, `ladder_builder.hpp`, `time_strategy.hpp` | Fee: <50 ns<br>Kelly: <200 ns | 56/56 | ✅ Completado |
| **P5** | Integration + paper trading | `engine_extensions.hpp` v2 | Pipeline <15μs p99 | 8 acceptance tests + 84 integration checks | ✅ Completado |
| **P6** | Canary deploy ($50) + monitoring | `docs/DEPLOYMENT.md#canary` | — | H1-H15 checklist | 🔄 En despliegue |

**Total de tests unitarios: 120 checks, todos pass.** ASan/UBSan limpios.

**Brownian Bridge formula:**
```
P(TWAP_final > K) = Φ(d)
d = (A_t·t + S_t·τ − K·T) / (σ·√(τ³/3))
```
donde `A_t` = acumulado de precios, `S_t` = TWAP acumulado, `τ` = tiempo restante,
`K` = strike, `T` = duración total de la ventana. Φ(x) via A&S 7.1.26 (error < 1.5×10⁻⁷).

**CfC legacy:** `cfc_network.hpp` y `infra/models/cfc_btc_5m_v1.bin` se conservan
para referencia histórica. No participa en el hot-path DSH. El modelo entrenado
offline (2,536 floats) no se usa en la decisión actual.

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
- **CfC legacy.** El modelo `cfc_btc_5m_v1.bin` (2,536 floats) se entrenó
  con datos históricos de Binance BTC/USDT 5-min. Se mantiene para referencia
  pero no se usa en el hot-path DSH actual. Re-entrenamiento requerido solo si
  se reintrodujera CfC.
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
