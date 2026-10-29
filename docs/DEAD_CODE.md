# Dead code ledger

**Policy (enforced, not advisory):** a header under `core/` or `infra/` must be
reachable — transitively, through quoted includes — from
`core/src/main_hot_path.cpp`, the only translation unit the binary is built
from. Code kept for research lives under the `CROWDINTEL_BUILD_EXPERIMENTAL`
option, which is **OFF** by default. `scripts/check_dead_headers.sh` fails the
build when a *new* orphan appears.

This is not tidiness. It has already cost real work: `fee_calculator_v2.hpp`
carries an accepted ADR, its own tests, and a Python parity oracle for a fee
model the bot never runs. Anyone implementing "the fee phase" reaches for it
first and lands on a model that disagrees with the venue by up to 2.4x.

**Baseline: 27 orphan headers** out of 63 under `core/` and `infra/`
(`docs/dead_headers_baseline.txt`). The list may only shrink.

## Classification

| Header | Decision | Why |
|---|---|---|
| `core/src/binance_ws_client.hpp` | **WIRE-A** | Cliente WS real a Binance (947 lineas). Precio de entrada para el Motor A. |
| `core/src/ofi_linear_filter.hpp` | **WIRE-A** | z-score de OFI con EWMA. Complemento natural del Motor A. |
| `core/src/twap_brownian_bridge.hpp` | **WIRE-A** | Modelo P(TWAP>K) con tau^1.5. Es el corazon del Motor A (convergencia crypto). Cablearlo requiere el feed del oraculo. |
| `core/src/twap_tracker.hpp` | **WIRE-A** | Acumulador TWAP. Necesario para replicar el oraculo de 60s. |
| `core/src/volatility_estimator.hpp` | **WIRE-A** | Regimenes NORMAL/SHOCK/CALM por EWMA multi-escala. |
| `core/src/window_shield.hpp` | **WIRE-A** | FSM de 5 estados con CLOSE_ONLY/HALTED. Utilitario de riesgo para el Motor A. |
| `core/src/crowdintel_bias.hpp` | **WIRE-B** | Motor de sesgo de ballenas (tilt +-30bps). Sin referencias, ni un test. Util para el Motor B si se decide monetizar crowdintel. |
| `core/src/circuit_breaker.hpp` | **WIRE-C** | CLOSED/OPEN/HALF_OPEN con TODO explicito: wire into order_gateway. |
| `core/src/ladder_builder.hpp` | **WIRE-C** | Construccion de escalera de cotizaciones. Necesario para el Motor C (maker). |
| `core/src/rate_limiter.hpp` | **WIRE-C** | Token bucket entero con TODO explicito: integrate with order_gateway. |
| `core/src/network/socket_config.hpp` | **KEEP** | Utilidades de socket; revisar si conviene cablearlo en el cliente TLS propio. |
| `core/src/engine_extensions.hpp` | **EXPERIMENTAL** | Agrupa 11 modulos muertos. Su comentario afirma que el engine lo fusiona en EngineLayers: ese constructor no existe. |
| `core/src/fee_calculator.hpp` | **FROZEN** | Modelo de fees plano, incorrecto. Banner de deprecacion. Reemplazado por taker_fee_per_share(). |
| `core/src/fee_calculator_v2.hpp` | **FROZEN** | Igual que el anterior; su duplicado es la trampa mas cara del repo. |
| `core/src/kelly_sizer.hpp` | **FROZEN** | Paralelo a alpha/strategy/kelly_engine.hpp, que es el que usa el engine. min_edge 0.005 aqui fue citado como si fuera produccion. |
| `core/src/bench_engine.hpp` | **DELETE** | Benchmark de una segunda implementacion de estrategia que no existe. |
| `core/src/binance_spike_detector.hpp` | **DELETE** | update() declarado y nunca definido: solo enlaza por casualidad. |
| `core/src/cfc_network.hpp` | **DELETE** | Red neuronal CfC. El diseno del bot es deterministico, sin ML en el hot path. |
| `core/src/cfc_network_test.hpp` | **DELETE** | Soporte de test del anterior. |
| `core/src/market_context.hpp` | **DELETE** | Vacio de contenido util. |
| `core/src/mmap_state.hpp` | **DELETE** | Estado en mmap sin uso. |
| `core/src/multi_feed_manager.hpp` | **DELETE** | Fusion de feeds; el feed real (binance_ws_client) no lo usa. |
| `core/src/pyth_client.hpp` | **DELETE** | Feed Pyth sin uso; el oraculo de resolucion es Chainlink TWAP-60s. |
| `core/src/request_prioritizer.hpp` | **DELETE** | Priorizacion de requests sin consumidor. |
| `core/src/time_strategy.hpp` | **DELETE** | Sin uso. |
| `core/src/venue_state_machine.hpp` | **DELETE** | FSM de 425/429; el CircuitBreaker cubre lo mismo y tampoco esta cableado. |
| `infra/feeds/clob_websocket.hpp` | **DELETE** | Cliente WS duplicado; ws_market_listener.hpp es el real y esta cableado. |

### Decision key

| Key | Meaning |
|---|---|
| **WIRE-A** | Needed for Motor A (crypto TWAP convergence). Wire when the resolution-oracle feed is decided. |
| **WIRE-B** | Needed for Motor B (crowdintel insider mirroring). Wire only after the 2-week signal-quality experiment. |
| **WIRE-C** | Needed for Motor C (maker carry). Wire together with the GTC resting-order path. |
| **KEEP** | Reviewed and deliberately retained; revisit before wiring. |
| **EXPERIMENTAL** | Pulls in a dead dependency tree; compiled only with the opt-in flag. |
| **FROZEN** | Known-wrong duplicate of production logic. Deprecation banner in the file. Delete, never edit. |
| **DELETE** | No consumer, no plan to have one. Safe to remove. |
| **REVIEW** | Not yet classified. |

## Done in this pass

* `infra/feeds/binance_feed.hpp` — **deleted**. Zero references and it did not
  compile (malformed comment block at line 10, missing `<string>`, a
  non-existent `infra/include/` path).
* `core/src/async_state_store.hpp` — **deleted**. Its Redis and PostgreSQL
  writers were `(void)events;` stubs whose own comment said "For now, we stub it
  to ensure compilation". Fase 2 replaced its purpose with
  `core/include/daily_state.hpp`, which is real.
* Deprecation banners on `fee_calculator.hpp` and `fee_calculator_v2.hpp`
  pointing at `taker_fee_per_share()`.
* `scripts/verify_ci_build.sh` no longer requires dead files to exist; it now
  runs `check_dead_headers.sh` as step 4.
* Five targets that only exercise `engine_extensions.hpp` are gated behind
  `-DCROWDINTEL_BUILD_EXPERIMENTAL=ON`. `tests/unit/test_fees.cpp` was rewritten
  against the production model instead of being deleted.

## Next

1. Move the WIRE-A/WIRE-B/WIRE-C headers (and their tests) under
   `experimental/` so `core/` contains only what ships. Their tests move with
   them; the CMake gating already exists.
2. Delete the DELETE-marked headers together with their now-gated tests.
3. Re-run `./scripts/check_dead_headers.sh --update` and confirm the baseline
   shrinks to the WIRE set.
