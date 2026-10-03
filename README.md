# Bot CrowdIntel — motor de ejecución para el CLOB de Polymarket

Bot de trading de uso propio (mío y de unos socios). No es un producto, ni una
librería, ni un servicio multi-usuario: es un único proceso que recibe señales
`alpha` por HTTP en loopback, mantiene el libro L2 por WebSocket, decide precio y
tamaño con Kelly fraccionario bajo límites de riesgo, firma órdenes EIP-712 y las
envía al CLOB (*Central Limit Order Book*) de Polymarket por HTTPS con
autenticación L2 (HMAC-SHA256).

Alcance real: una wallet EOA (*Externally Owned Account*), un mercado a la vez,
órdenes mínimas, capital propio y un operador humano delante. No hay soporte de
multi-cuenta, ni de cartera, ni de type 3 (ERC-7739), ni de posiciones de
protocolo v2.

---

## Etapa actual — leer antes de tocar nada

**Fecha del estado: 2026-10-03. Etapa: código auditado y listo para *preparar* el
canary. El bot TODAVÍA NO ha operado con dinero real: no se ha observado ni una
orden, ni un fill, ni un heartbeat, ni una reconciliación contra el venue real.**

| Área | Estado |
|---|---|
| Auditoría de código | Cerrada. 27 hallazgos corregidos: 1 bloqueante, 2 altos, 12 medios, 12 bajos. El bloqueante era que `crowdintel-preflight` no ligaba la wallet, así que su puerta no podía aprobarse nunca y tres comprobaciones de identidad pasaban en vacío. Resumen en [CHANGELOG.md](CHANGELOG.md) y detalle en [docs/LIVE_SAFETY_2026-10-03.md](docs/LIVE_SAFETY_2026-10-03.md) |
| Builds y pruebas | 4 configuraciones desde cero sin un solo warning (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`): Release+red, Release offline, ASan+UBSan, TSan. ctest 7/7 · 6/6 · 7/7 · 7/7. 100 iteraciones repetidas bajo TSan sin ningún informe. Cppcheck 2.17.1: 0 errores, 0 warnings |
| Verificado sin red real | Firma EIP-712 de orden (vector independiente en Python: pycryptodome + coincurve) y firma L1 `ClobAuth` (idéntica byte a byte al vector publicado por el SDK oficial `py-clob-client`). Protocolo del canal user, heartbeat y REST contra un *venue loopback* propio y fixtures derivados de los SDK oficiales. Ledger WAL, reconciliación, kill switch, gates de egreso y fail-closed |
| **NO verificado** (sólo el venue real lo resuelve) | Path y body exactos del heartbeat (`/v1/heartbeats` vs `/heartbeats`; `null` vs `""`); cadencia real de cancelación (umbral 10 s confirmado en los dos SDKs, peor caso ~15 s); si el `orderID` del venue es igual al digesto EIP-712 local; semántica de `fd.e≠1` y unidades de `base_fee`; latencias y margen real de *rate limit*. Todo marcado `[NO VERIFICADO]` con su paso de verificación en la checklist |
| Siguiente paso | Ejecutar **H1–H15** de [docs/CANARY_CHECKLIST.md](docs/CANARY_CHECKLIST.md) en el host de producción, con una wallet financiada con el mínimo y credenciales exclusivas del bot |
| Cerrado a propósito | Signature type 3 (POLY_1271 / ERC-7739) y posiciones de protocolo v2 (`BOT_ALLOW_PROTOCOL_V2=0`): se rechazan en el arranque con mensaje explícito, no se ignoran |

Regla para los socios: **nadie pone capital hasta que H1–H15 estén ejecutados y
registrados**, y el primer canary es mínimo, FAK, supervisado y con reconcile
externo tras cada orden.

---

## Qué hay en el repositorio

### Binarios

| Binario | Fuente | Función |
|---|---|---|
| `crowdintel_bot` | `core/src/main_hot_path.cpp` | Proceso principal: carga y valida configuración, liga la identidad de la wallet, arranca los hilos (libro, canal user, heartbeat, reconciliación, supervisor de metadata, kill switch), aplica los gates y al apagar imprime un post-mortem (por qué quedó `READY`/`BLOCKED`, estado del contrato de heartbeat) |
| `crowdintel-preflight` | `core/src/preflight_main.cpp`, `core/src/preflight.hpp` | Comprueba antes de operar: chain id por RPC, reloj, credenciales L2 y (opcional) L1, identidad wallet/maker/POLY_ADDRESS, metadata del mercado, saldo/allowance/posiciones/aprobaciones, durabilidad del ledger, canal user y heartbeat. Si todo pasa escribe un token (0600, atómico) que autoriza `BOT_ENABLE_LIVE_TRADING=1` sólo para ese fingerprint de configuración |
| `crowdintel-config` | `core/src/config_tool_main.cpp` | `validate` / `render-env` / `fingerprint` / `keys` sobre un fichero de entorno. `keys` lista las 80 claves reconocidas; con `BOT_STRICT_ENV=1` cualquier variable con prefijo conocido que el binario no lea es un error de arranque |

### Partes del bot (por función)

| Función | Ficheros | Qué hace |
|---|---|---|
| Ingesta de señales | `alpha/crowdintel/alpha_parser.hpp`, `alpha_receiver.hpp`, `core/src/alpha_http_receiver.hpp` | `POST /signal` en loopback con bearer token, HTTP/JSON estricto y acotado, plazo absoluto de 250 ms, rechazo de campos duplicados, normalización de mercado |
| Libro de mercado | `core/src/ws_market_listener.hpp`, `core/include/order_book.hpp` | WSS de mercado (`book`, `price_change(s)`), top publicado atómicamente y profundidad protegida; un tick no reconocido invalida el libro |
| Sesión WSS genérica | `core/src/ws_session.hpp`, `ws_url.hpp` | Handshake RFC 6455, `PING`/`PONG` textuales, framing acotado, reconexión con backoff; la usa el canal user (el listener de mercado aún lleva su propia fontanería: deuda documentada) |
| Metadata del venue | `core/src/metadata_pipeline.hpp`, `core/include/venue_metadata.hpp` | Gamma + CLOB: tick size, tamaño mínimo, `neg_risk`, fee, estado del mercado y conjunto de tokens. Se publica con el instante de observación y el supervisor bloquea el egreso si envejece (`BOT_METADATA_MAX_AGE_MS`) |
| Decisión y riesgo | `alpha/strategy/kelly_engine.hpp`, `core/src/execution_engine.hpp` | Filtros (confianza, q, edge mínimo, TTL, deduplicación, mercado), edge neto de fee, Kelly fraccionario, caps de orden/exposición/pérdida diaria, BUY reserva el peor coste y SELL exige inventario confirmado |
| Firmas pregeneradas | `core/src/presigned_pool.hpp`, `core/src/order_gateway.hpp` | Escalera consumible de órdenes presignadas por lado/precio/tamaño (un slot se consume una sola vez por CAS) y cola SPSC de egreso con reservas |
| Cripto | `core/crypto/eip712_signer.hpp`, `keccak256.hpp`, `sha256_engine.hpp`, `fast_random.hpp`, `order_digest.hpp`, `clob_auth.hpp` | EIP-712 (dominio estándar y negative-risk), Keccak-256, HMAC-SHA256 para L2, CSPRNG ChaCha20 sembrado por el SO para sales, digesto de orden y firma L1 `ClobAuth` |
| Órdenes | `core/src/polymarket_order.hpp` | Aritmética de punto fijo exacta (precio, shares, amounts), cuantización por tick, cuerpo JSON canónico y acotado, expiración GTD |
| Transporte | `core/src/curl_transport.hpp`, `clob_rest_client.hpp`, `rpc_client.hpp`, `lightweight_client.hpp` | HTTPS/WSS con libcurl+OpenSSL, pin TLS opcional, `http://` sólo a loopback y con opt-in explícito de test; REST del CLOB con cabeceras L2, heartbeat con fallback de path/body, cancelaciones; JSON-RPC a Polygon (chainId, ERC-20/ERC-1155, allowances) |
| Canal privado | `core/src/user_ws_client.hpp`, `user_ws_protocol.hpp`, `user_event.hpp` | Suscripción autenticada al canal user, parsing de frames `order`/`trade`, aplicación idempotente y monótona al ledger (el stream no repite lo perdido: tras reconectar se lee REST) |
| Estado durable | `core/include/event_ledger.hpp`, `core/src/order_recorder.hpp`, `core/include/order_state.hpp`, `ledger_order_observer.hpp`, `session_report.hpp` | Ledger write-ahead con fsync y checkpoints, replay y rechazo de journal corrupto, ticket durable **antes** de enviar, `UNKNOWN` (nunca reintento a ciegas) ante transporte ambiguo, máquina de estados de orden y formateadores puros del post-mortem |
| Seguridad operativa | `core/src/order_heartbeat.hpp`, `reconciliation.hpp`, `kill_switch.hpp`, `market_config.hpp` | Latido con warn/block/assume-cancelled y margen verificado frente al umbral del venue; reconciliación al arranque y tras cada desconexión (órdenes, fills, balances, allowances, inventario) → `READY`/`BLOCKED`; kill switch por fichero en **todos** los modos (ruta no comprobable = bloqueado); configuración ENV estricta con fingerprint |
| Infra | `infra/config/production.env.example`, `infra/scripts/deploy_production.sh`, `infra/scripts/kernel_tuning.sh`, `infra/docker/Dockerfile.prod` | Plantilla no-secreta de systemd `EnvironmentFile`, despliegue, ajuste conservador de kernel y contenedor de producción sin root |
| Pruebas | `tests/unit`, `tests/integration`, `tests/support`, `tests/replay`, `tests/benchmarks`, `tests/crypto` | Unitarios de núcleo y de seguridad en vivo, **venue loopback** (servidor HTTP/1.1 + WebSocket en 127.0.0.1 con fallos adversarios), transporte de fixtures, backtester de replay, benchmark de CPU y cross-check cripto independiente en Python |

---

## Modelo de seguridad (todo falla cerrado)

- Una orden **encolada no es una orden aceptada**: se exige HTTP 2xx + `success:true`
  + ausencia de error semántico. Un timeout ambiguo produce `UNKNOWN`, deshabilita el
  trading y **no** se reenvía.
- `BOT_ENABLE_LIVE_TRADING=1` sólo se honra con un token de preflight fresco
  (`BOT_PREFLIGHT_MAX_AGE_S`) cuyo `config_fingerprint` coincida con la configuración
  efectiva. El fingerprint cubre la configuración no secreta; la wallet se compara
  a mano (H2/H3): ambos binarios imprimen `wallet=`.
- El trading se habilita sólo tras pasar todos los gates; cualquier divergencia
  (metadata vieja, heartbeat tardío, desconexión, ledger corrupto, balance/allowance/
  inventario que no cuadra, chain id distinto de 137) → `trading_enabled=false` y
  reconciliación autoritativa. `assume-cancelled` **nunca** libera exposición.
- Nunca se aprueba allowance automáticamente y nunca se usa `max_uint256`: el objetivo
  es exposición + 10 % + 1 pUSD, y lo fija el operador.
- Secretos sólo por fichero (`/etc/crowdintel/credentials/*`, systemd
  `LoadCredential=`), regulares, no-symlink, 0600; se borran del entorno tras leerlos y
  no aparecen en logs, ledger ni fingerprint.
- Kill switch: crear el fichero bloquea egreso y provoca apagado; una ruta que no se
  puede comprobar se trata como activado.
- systemd no reinicia automáticamente tras un fallo: el estado de la cuenta puede ser
  desconocido.

## Build

Requisitos: CMake ≥ 3.16, compilador C++20 (verificado con GCC 12.2; CI también
clang), pthreads y **libsecp256k1 con el módulo recovery**. El build con red añade
OpenSSL y libcurl de desarrollo.

```bash
# libsecp256k1 en el commit fijado por CI
export SECP_COMMIT=6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d
git clone --filter=blob:none https://github.com/bitcoin-core/secp256k1 /tmp/secp256k1
git -C /tmp/secp256k1 checkout --detach "$SECP_COMMIT"
cmake -S /tmp/secp256k1 -B /tmp/secp256k1/build -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
  -DSECP256K1_BUILD_TESTS=OFF -DSECP256K1_BUILD_BENCHMARK=OFF
cmake --build /tmp/secp256k1/build -j

# Build sin dependencias de red (mock/paper determinista)
cmake -S core -B build -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=OFF -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`-DCROWDINTEL_NETWORK=ON` (por defecto) compila el transporte real HTTPS/WSS; si
faltan OpenSSL o libcurl la configuración **falla** en vez de generar un stub con
aspecto funcional. Opciones de sanitizadores: `-DCROWDINTEL_ASAN=ON
-DCROWDINTEL_UBSAN=ON`, `-DCROWDINTEL_TSAN=ON` (TSan no se puede combinar con
ASan/UBSan).

## Pruebas y verificación

```bash
ctest --test-dir build --output-on-failure     # crypto_kat, core_units, backtester_smoke,
                                               # live_safety_units, local_venue_integration,
                                               # preflight_rejects_empty_config,
                                               # preflight_binds_wallet_identity

# Cross-check cripto independiente (misma versión que CI)
python3 -m pip install pycryptodome==3.23.0 coincurve==21.0.0
build/bin/test_signer --json > /tmp/signer.json
python3 tests/crypto/cross_check_v2.py /tmp/signer.json
build/bin/test_signer --json-neg-risk > /tmp/signer-neg.json
python3 tests/crypto/cross_check_v2.py /tmp/signer-neg.json

# Benchmark de CPU (no es un SLO de extremo a extremo)
build/bin/latency_bench | tee /tmp/lat.txt
python3 tests/benchmarks/check_latency.py /tmp/lat.txt
```

`latency_bench` fija su propia identidad (`BOT_MODE=paper` + `BOT_TOKEN_ID`) porque
sólo mide la ruta de CPU. Si un bucle no produce ninguna muestra, imprime los motivos
de rechazo y termina con código 1, y `check_latency.py` rechaza un `p50=0`: un
benchmark que no mide nada no puede pasar por rápido. Cifras medidas el 2026-10-03 en
el sandbox de desarrollo (Release+red, 2 vCPU): decisión + pool presignado + submit
**p50 ≈ 0,44 µs**; decisión + firma inline + submit **p50 ≈ 34-38 µs**, de los que
~33 µs son Keccak+ECDSA. Es decir: el cuello es la firma, no la decisión.

CI (`.github/workflows/ci.yml`): gcc/clang con red y gcc offline, contenedor de
producción sin root, ASan+UBSan y TSan, humo de configuración y preflight, cross-check
cripto y presupuestos laxos de latencia.

## Corrida local (sin dinero, sin venue)

```bash
BOT_MODE=paper BOT_TICKS=50 \
BOT_MARKET_SLUG=demo \
BOT_TOKEN_ID=71321045679252212594626395510336467040167069592778062791519851593659551227755 \
BOT_LEDGER_DIR=/tmp/crowdintel-demo \
BOT_PRIVATE_KEY_HEX=23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b \
  build/bin/crowdintel_bot
```

Esa clave es el vector público de pruebas (KAT): **nunca** se usa con fondos. El
proceso imprime banner, `config_fingerprint`, `wallet=/maker=/api_address=`, termina
con `readiness=BLOCKED` (no hay venue) y cierra con el post-mortem. `BOT_MODE=mock`
sigue funcionando como sinónimo de `paper` pero avisa de que está obsoleto;
`BOT_TOKEN_ID` es obligatorio fuera de `replay`: paper con red resuelve la identidad
desde la metadata del venue, igual que live.

## Configuración

Referencia completa en [docs/CONFIGURATION.md](docs/CONFIGURATION.md) (80 claves,
defaults, rangos y reglas de carga) y plantilla no-secreta en
[`infra/config/production.env.example`](infra/config/production.env.example)
(`crowdintel-config validate` la acepta y rechaza cualquier variable que el binario no
lea). Los secretos van por `_FILE` desde `/etc/crowdintel/credentials/*`.

## Documentos

| Documento | Contenido |
|---|---|
| [docs/STATUS.md](docs/STATUS.md) | Estado verificado / no verificado / bloqueos, en inglés |
| [docs/ARQUITECTURA.es.md](docs/ARQUITECTURA.es.md) | Resumen operativo en español: flujo e invariantes |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Especificación técnica: concurrencia, firma, orden, fallos |
| [docs/CANARY_CHECKLIST.md](docs/CANARY_CHECKLIST.md) | Puerta de salida a real: gates automáticos, preflight, **H1–H15**, reglas operativas obligatorias, trabajo diferido y condiciones de parada |
| [docs/CONFIGURATION.md](docs/CONFIGURATION.md) | Todas las variables, validaciones y el guardián de edad de metadata |
| [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) | Runbook de despliegue, systemd, rollback |
| [docs/SECURITY.md](docs/SECURITY.md) | Modelo de amenazas y fronteras de confianza |
| [docs/BENCHMARKING.md](docs/BENCHMARKING.md) | Qué mide y qué **no** mide el benchmark; procedimiento reproducible |
| [docs/LIVE_SAFETY_2026-10-03.md](docs/LIVE_SAFETY_2026-10-03.md) | Auditoría de seguridad en vivo: hechos citados (SDK oficiales) frente a hipótesis |
| [docs/REMEDIATION_STATUS.md](docs/REMEDIATION_STATUS.md) | Registro histórico de la remediación de 2026-09-22 |
| [CHANGELOG.md](CHANGELOG.md) | Cambios, incluida la auditoría de 2026-10-03 (F-01..F-27) |

## Dependencias

Mínimas y fijadas: C++20 estándar, **libsecp256k1** (commit
`6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d`, módulo recovery) para ECDSA, **OpenSSL**
para TLS, **libcurl** para HTTP. Para la verificación cruzada de la firma, fuera del
binario: **pycryptodome** 3.23.0 y **coincurve** 21.0.0. Las referencias de contrato
se tomaron de los SDK oficiales de Polymarket (`py-clob-client`, `clob-client`,
`py-sdk`) y de docs.polymarket.com; las citas exactas están en
[docs/LIVE_SAFETY_2026-10-03.md](docs/LIVE_SAFETY_2026-10-03.md) y en las cabeceras de
cada fichero.

## Trabajo diferido (decidido, fuera del alcance del canary)

| Tema | Decisión |
|---|---|
| Signature type 3 (POLY_1271 / ERC-7739) | No se implementa ahora: el canary va con EOA (type 0). Falla cerrado con mensaje específico |
| Migrar `ws_market_listener.hpp` a `ws_session.hpp` | Diferida: deuda técnica con plan escrito; no se toca un hot path conocido-bueno justo antes del canary |
| `BOT_ALLOW_PROTOCOL_V2` | Se queda en 0 (las posiciones v2 liquidan por Combos/PositionManager, fuera de este conjunto de signer/allowance) |
| `BOT_RECON_MAX_PAGES` | Se queda en 4; sólo se sube si el canary llegara a dejar más órdenes en reposo que cuatro páginas |
| Ligar el token de preflight a la dirección de la wallet | Pendiente de decisión: hoy se mitiga imprimiendo `wallet=` en ambos binarios y comparándola en H2/H3, más la reconciliación de arranque |

## Licencia

Ver [LICENSE](LICENSE).
