# Phase 0 — Baseline Recognition Report
**Project:** Bot_Crowdintel — Ultra-Low Latency Polymarket Trader  
**Analyst:** Arquitecto (Arquitectura, contratos y decisiones técnicas)  
**Date:** 2026-09-17  
**Git HEAD:** `6fbfb06` — *"fix: resolve Adversary audit — real Keccak-256, real build system, real benchmarks, real MutaLambda integration"*  
**Working Tree:** 1 uncommitted modification to `core/crypto/eip712_signer.hpp` (second Keccak fix attempt, still broken)  
**Environment:** GCC 16.2.1, OpenSSL 3.5.8, CMake 3.x. libcurl-dev headers **NOT** installed system-wide (found via conda fallback at `/home/adlg/text-generation-webui/text-generation-webui/installer_files/conda`).

---

## 1. Árbol de Archivos — Revisión de core/, alpha/, tests/, infra/, docs/

### 1.1 core/ — Hot Path (C++20)

| File | Estado | Compila | Observaciones |
|------|--------|---------|---------------|
| `core/CMakeLists.txt` | **MODIFICADO (HEAD)** | ✅ Sí | `find_package(CURL/OpenSSL)` + `target_link_libraries` agregados en HEAD. CURL se resuelve via fallback *hardcodeado* a conda. |
| `core/include/order_book.hpp` | ✅ Estable | ✅ Sí | `OrderBookL2` con `std::array` estático. **`get_bid()`/`get_ask()` sin bounds check** — OOB posible. |
| `core/include/spsc_ring_buffer.hpp` | ✅ Estable | ✅ Sí | SPSC lock-free correcto. Usa `memory_order_relaxed`/`acquire`/`release`. |
| `core/crypto/eip712_signer.hpp` | **⚠️ MODIFICADO (uncommitted)** | ✅ Sí | Keccak-256 **BROKEN** (ver sección 3). Domain separator placeholder. v=27 hardcoded. Deprecación OpenSSL 3.0. |
| `core/crypto/test_signer.cpp` | ✅ Estable | ✅ Sí | CMake target. Solo verifica "no throw". **Sin known-answer test**. Key `0xAA*32` hardcodeada. |
| `core/src/execution_engine.cpp` | ✅ Estable | ✅ Sí | `ExecutionEngine`. Private key fallback `0x01*32`. Maker/taker `0x00*20`. Sin `std::cout` en `run_tick` (pero `submit_order` sí). |
| `core/src/main_hot_path.cpp` | ✅ Estable | ✅ Sí | Credenciales `"YOUR_API_KEY"/"YOUR_SECRET"/"YOUR_PASSPHRASE"` hardcoded. Base URL: `https://clob.polymarket.com`. |
| `core/src/lightweight_client.hpp` | ✅ Estable | ✅ Sí | `CURLOPT_NOBODY` con POSTFIELDS (bug). Sin timeout. `std::cout` en hot path. HMAC sobre payload real (no placeholder). |
| `core/src/mock_client.hpp` | ✅ Estable | ✅ Sí | `MockCLOBClient` para benchmarks. No I/O. |
| `core/src/nonce_manager.hpp` | ✅ Estable | ✅ Sí | **Timestamp mock** (`1600000000000` hardcoded). Comentario dice "in production use clock_gettime". |
| `core/src/ws_market_listener.hpp` | ✅ Estable | ✅ Sí | **Simulated** — no WebSocket real. Push de `AlphaSignal` simulado cada 100ms. |

### 1.2 alpha/ — Cold Path

| File | Estado | Compila | Observaciones |
|------|--------|---------|---------------|
| `alpha/crowdintel/alpha_receiver.hpp` | ✅ Estable | ✅ Sí | `AlphaSignal` POD struct. Campos: type, market_slug, confidence, ev_per_dollar, q_value, timestamp_ns. |
| `alpha/crowdintel/alpha_parser.cpp` | ✅ Estable | ✅ Sí | `AlphaParser`. Filtrado FDR/q-value, confidence, EV. `timestamp_ns = 123456789` mock. |
| `alpha/strategy/kelly_engine.hpp` | ✅ Estable | ✅ Sí | `KellyEngine`. Kelly simplificado (`ev * confidence * fraction`). |
| `alpha/strategy/market_making_engine.hpp` | ✅ Estable | ✅ Sí | `MarketMakingEngine`. **No usado en hot path**. Typo: `initial_twel`. |

### 1.3 tests/

| File | Estado | Compila | Ejecútable | Observaciones |
|------|--------|---------|------------|---------------|
| `tests/benchmarks/latency_bench.cpp` | ✅ Estable | ✅ Sí | ✅ Sí | CMake target. **P99 real: ~957µs** (NO 24 ciclos/7.8ns). Mide firma real de ECDSA. |
| `tests/benchmarks/mem_audit.py` | ✅ Estable | N/A | ❌ No | **Stub** — nunca ejecuta valgrind. Imprime "binary not yet compiled". |
| `tests/replay/l2_backtester.cpp` | ✅ Estable | ✅ Sí | ✅ Sí | CMake target. **Skeleton** — cuerpos `// ...`. Riesgo división por cero. |

### 1.4 Root-level (untracked — no están en git)

| File | Tipo | Observaciones |
|------|------|---------------|
| `test_keccak.cpp` | Test fuente | Verifica `keccak256("")`. **FALLA**. |
| `test_keccak_verify.cpp` | Test fuente | Verifica con `memcmp`. **FALLA**. |
| `test_keccak_debug.cpp` | Test fuente | Compara impl vs referencia. **Ambas FALLAN**. |
| `test_keccak` (binario) | Pre-built | Produce hash incorrecto: `dc00ace7...` |
| `test_keccak_bin` (binario) | Pre-built | Sin ejecutar. |
| `test_keccak_verify` (binario) | Pre-built | Sin ejecutar. |
| `test_keccak_debug` (binario) | Pre-built | Sin ejecutar. |

> **Nota:** Estos archivos root-level test fueron agregados al working tree (no commiteados). No están en `.gitignore` como ignorados, pero `git status` los muestra como "Untracked".

### 1.5 infra/

| File | Estado | Observaciones |
|------|--------|---------------|
| `infra/docker/Dockerfile.prod` | ✅ Estable (HEAD) | Path corregido a `bin/crowdintel_bot`. Instala dev deps. **Sin `.dockerignore`** → copia core/build/ completo. |
| `infra/scripts/deploy_production.sh` | ✅ Estable (HEAD) | Deploy via SSH. Usa `crowdintel_bot` (no stale `crowdintel_bot_test`). |
| `infra/scripts/kernel_tuning.sh` | ✅ Estable | Requiere root, modifica GRUB, requiere reboot. `tcp_congestion_control=bbr` puede no estar disponible. |
| `infra/mutalambda/adapter/mutalambda_adapter.py` | ✅ Estable (HEAD) | **Dry-run** — import falla, emite resultados fabricados (2.14% / AVX-512). |
| `infra/scripts/mutalambda_optimize.py` | ✅ Estable | **Mock** — imprime "AVX-512" y "1.2us" sin lógica real. |
| `infra/mutalambda/optimization_targets.json` | ✅ Estable | 3 targets: sign_order, try_push, update_bid. |

### 1.6 docs/

| File | Estado | Observaciones |
|------|--------|---------------|
| `docs/ARCHITECTURE.md` | ✅ Estable | Claims "AVX2/SIMD" para crypto. No hay código SIMD real. |
| `docs/FINAL_AUDIT.md` | ✅ Estable (HEAD) | **"APPROVED FOR PRODUCTION"**. Claims P99 ≈ 26µs, AVX2/SIMD, Circuit Breakers. **Contradictado por la realidad**. |
| `docs/OPTIMIZATION_LINEAGE.md` | ✅ Estable (HEAD) | Fechas futuras (2026-09-17). Claims "Real (MutaLambda v5.0)". MutaLambda en dry-run. |
| `docs/PERF_METRICS.md` | ✅ Estable | Claims "✅ Verified" con valgrind. `make pgo-build` y `./bin/benchmark_latency` **no existen**. |

### 1.7 Root-level

| File | Estado | Observaciones |
|------|--------|---------------|
| `README.md` | ✅ Estable (HEAD) | Claims "It compiles, signs orders using OpenSSL's ECDSA" y "P99 24 cycles / 7.8ns". |
| `AUDITOR_VERDICT.json` | ✅ Estable (HEAD) | Verdict: FAIL. 6 categorías CRITICAL/HIGH. Fue escrito contra el estado **pre-fix** (commit `56e8b02`). |
| `WORKFLOW_MAESTRO_POLYMARKET.md` | ✅ Estable | Claims "8.0 ns (24 ciclos)" y "AVX2/SIMD". Referencia commit `5da1443` (4 detrás de HEAD). |
| `LICENSE` | ✅ Estable (HEAD) | MIT License. |
| `.github/workflows/ci-cd-and-optimize.yml` | ✅ Estable (HEAD) | CI instala `libcurl4-openssl-dev libssl-dev`. Pero el "latency gate" solo corre `./bin/crowdintel_bot \|\| exit 1` (siempre exit 0). |

---

## 2. Estado del Build (Build System)

### CMakeLists.txt (working tree = HEAD)
```
cmake_minimum_required(VERSION 3.16)
project(CrowdIntelBot LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
add_compile_options(-O3 -march=native -flto -fno-rtti -funroll-loops -DNDEBUG)
find_package(CURL QUIET COMPONENTS libcurl)   ← con fallback hardcodeado a conda
find_package(OpenSSL REQUIRED)
add_executable(crowdintel_bot ${CORE_SOURCES})  ← src/*.cpp + ../alpha/crowdintel/*.cpp
target_link_libraries(crowdintel_bot ${CURL_LIBRARY} OpenSSL::Crypto OpenSSL::SSL)
add_executable(test_signer crypto/test_signer.cpp)
add_executable(latency_bench ../tests/benchmarks/latency_bench.cpp)
add_executable(l2_backtester ../tests/replay/l2_backtester.cpp)
```

### Resultado del build (verificado en este entorno)

```
$ cmake .. && make -j$(nproc)
-- Found OpenSSL: /usr/lib64/libcrypto.so (found version "3.5.8")
-- CURL found via conda fallback: /home/adlg/.../conda/lib/libcurl.so
[100%] Built target crowdintel_bot  (✅ COMPILA)
[100%] Built target test_signer     (✅ COMPILA)
[100%] Built target latency_bench   (✅ COMPILA)
[100%] Built target l2_backtester   (✅ COMPILA)
```

**Warnings de compilación:** 12 warnings de deprecación por uso de la API `EC_KEY_*` de OpenSSL 3.0 (`EVP_PKEY_get1_EC_KEY`, `EC_KEY_set_private_key`, etc.). No son errores, pero la API está deprecated desde OpenSSL 3.0.

### ✅ Estado: COMPILA (build limpio desde el working tree)

> Lo que el `AUDITOR_VERDICT.json` dice: "CMakeLists defines a single target and performs NO find_package" — esto era **verdadero en el pre-fix** (commit 56e8b02), pero el HEAD commit lo corrigió. El build system actual funciona.

---

## 3. Estado de la Criptografía (EIP-712 Signer)

### 3.1 Keccak-256 — **BROKEN** (verificado por ejecución)

| Versión | `keccak256("")` result | Correcto? |
|---------|----------------------|-----------|
| Pre-fix (56e8b02) | `SHA256("")` = `e3b0c442...` | ❌ Stub |
| HEAD (6fbfb06) | `756843b1...` | ❌ Bug |
| Working tree (uncommitted) | `a6088b47...` | ❌ Bug |
| Pre-built `test_keccak` binary | `dc00ace7...` | ❌ Bug |
| **Expected** | `c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470` | — |

**Root cause (working tree):** El paso π (pi) de Keccak-f[1600] tiene un bug de indexación. La línea:
```cpp
int dest = y * 5 + (2 * x + 3 * y) % 5;
```
debería ser:
```cpp
int dest = y + 5 * ((2 * x + 3 * y) % 5);
```

El HEAD version tenía bugs adicionales: ROT array con valores `{ 27, 20, 39, 0, 0 }` (última fila incompleta), Chi con índice `B[y + x*5]` (transpuesto), y la permutación dentro del loop de absorción.

### 3.2 EIP-712 Domain Separator — **PLACEHOLDER**

```cpp
static const std::array<uint8_t, 32> domain_sep = {{0xAB}}; // Placeholder domain
```
Solo el primer byte es `0xAB`; el resto son ceros. **No es un domain separator real de Polymarket CLOB V2**. Las firmas no están bound al dominio correcto → vulnerabilidad de replay/cross-domain.

### 3.3 ECDSA Signing — **Técnicamente funciona, pero con bugs de protocolo**

- ✅ ECDSA over secp256k1 funciona (OpenSSL EVP_PKEY). `test_signer` produce una firma válida de 65 bytes.
- ❌ **Double-hashing bug:** `EVP_PKEY_CTX_set_signature_md(sign_ctx, EVP_sha256())` — OpenSSL SHA-256 el hash ya calculado. Combinado con el Keccak roto, la firma es incorrecta en dos ejes.
- ❌ **v = 27 hardcoded:** `out_signature[64] = 27`. Ethereum requiere 27/28 por paridad de y. Con v fijo, la recuperación falla ~50% del tiempo.
- ❌ **Private key fallback:** `std::vector<uint8_t>(32, 0x01)` en ExecutionEngine, `0xAA * 32` en BenchExecutionEngine y test_signer.

### 3.4 Test de Criptografía — **INSUFICIENTE**

| Test | Tipo | Known-Answer? | Resultado |
|------|------|---------------|-----------|
| `core/crypto/test_signer.cpp` | CMake target | ❌ No (solo "no throw") | ✅ PASS (vacío) |
| `test_keccak.cpp` (root) | No CMake | ✅ Sí | ❌ FAIL |
| `test_keccak_verify.cpp` (root) | No CMake | ✅ Sí | ❌ FAIL |
| `test_keccak_debug.cpp` (root) | No CMake | ✅ Comparativo | ❌ Ambas impls fallan |

### ✅ Estado: REAL (OpenSSL ECDSA), pero **CRYPTO BROKEN**
- Hay código criptográfico real (OpenSSL EVP + custom Keccak)
- El Keccak-256 **no produce hashes correctos** → todas las firmas EIP-712 son inválidas
- Domain separator, v, salt y private key son placeholders/dummies
- **No hay known-answer verification en la suite de CI**

---

## 4. Estado de Credenciales

| Location | Credential | Estado |
|----------|-----------|--------|
| `main_hot_path.cpp:25` | `LightweightCLOBClient("YOUR_API_KEY", "YOUR_SECRET", "YOUR_PASSPHRASE", ...)` | **PLACEHOLDER** (strings literales) |
| `execution_engine.cpp:88` | `std::vector<uint8_t>(32, 0x01)` (fallback priv key) | **HARDCODED** (dummy 0x01) |
| `bench_engine.hpp:21` | `EIP712Signer(std::vector<uint8_t>(32, 0xAA))` | **HARDCODED** (dummy 0xAA) |
| `test_signer.cpp:9` | `std::vector<uint8_t>(32, 0xAA)` | **HARDCODED** (dummy) |
| `nonce_manager.hpp:24` | `return 1600000000000;` | **HARDCODED MOCK** (timestamp fijo) |
| `main_hot_path.cpp` | `env_key = std::getenv("BOT_PRIVATE_KEY_HEX")` | ✅ Mecanismo de env var existe, pero fallback es 0x01*32 |
| Repo | `.env`, `*.pem`, `*.key`, `config.json` | ❌ No existen en el repo |

### ✅ Estado: **PLACEHOLDER / HARDCODED / DUMMIES**
- No hay credenciales reales en el repo (lo cual es correcto)
- Pero TODOS los valores son placeholders o dummies (no "reales")
- No hay secret management (vault, KMS, etc.)

---

## 5. Estado de Tests

| Component | Estado | Evidence |
|-----------|--------|----------|
| **CMake test targets** | ❌ 0 `add_test`/`enable_testing()` | No hay ctest configurado |
| **Crypto KAT** | ❌ Faltan known-answer tests en CI | `test_signer.cpp` solo verifica no-throw |
| **Unit tests** | ❌ No hay tests unitarios para OrderBook/SPSC/Kelly/AlphaParser | No hay archivos de test unitario |
| **Integration tests** | ❌ No hay tests de integración | No hay framework de test (no Catch2, no GoogleTest) |
| **Keccak verification** | ❌ `keccak256("")` produce hash incorrecto | `a6088b47...` ≠ `c5d24601...` |
| **Latency benchmark** | ⚠️ Ejecutable pero metricas inválidas | P99 real: ~957µs (NO 24 ciclos/7.8ns) |
| **mem_audit.py** | ❌ Stub | Nunca ejecuta valgrind |
| **l2_backtester.cpp** | ⚠️ Skeleton | Cuerpos `// ...`, riesgo división por cero |

### Resultados de ejecución verificada:

```
$ ./bin/test_signer
✅ Signature successful!   (solo verifica no-throw, no KAT)
$ ./bin/crowdintel_bot
🌐 HTTPS Order Submitted | Code: 404   (placeholder creds → 404)
$ ./bin/latency_bench
Min: 452µs | P50: 468µs | P99: 957µs   (NO 7.8ns)
$ ./bin/l2_backtester
🧪 L2 Backtester initialized. Ready for tick replay.  (skeleton)
$ ./test_keccak
❌ Keccak-256 INCORRECT
```

### ✅ Estado: **INCOMPLETO / INSUFICIENTE**
- Hay 3 executables de test (test_signer, latency_bench, l2_backtester)
- No hay ctest, no hay assertions reales
- El benchmark de latencia funciona pero produce ~957µs (no 24 ciclos)
- El test de keccak falla
- mem_audit.py es un stub

---

## 6. Implemented vs Documented vs Simulated

| Capability | Implemented? | Evidence | Documentado como |
|-----------|-------------|----------|-----------------|
| **Keccak-256** | ❌ Partial (existente, BROKEN) | `keccak256("")` = `a6088b47...` (incorrecto) | "FIPS 202 compliant" |
| **EIP-712 signing** | ⚠️ Real ECDSA pero hash roto | OpenSSL signs incorrect hash | "Production-ready" |
| **Domain separator** | ❌ Placeholder `{{0xAB}}` | Código lo confirma | (no documentado) |
| **HMAC auth (CLOB)** | ⚠️ Código existe pero creds placeholder | `hmac_sha256_base64()` funciona | "Polymarket CLOB V2" |
| **WebSocket listener** | ❌ SIMULATED | Push de datos mock, no conexión real | "Persistent WebSocket" |
| **NonceManager** | ❌ MOCK | Timestamp hardcoded `1600000000000` | "current time + atomic counter" |
| **OrderBook L2** | ✅ REAL | Código completo, compila | "O(1) updates" |
| **SPSC Ring Buffer** | ✅ REAL | Código completo, compila | "Lock-free, zero-alloc" |
| **Kelly Engine** | ✅ REAL | Código completo | "Kelly Criterion" |
| **Alpha Parser** | ✅ REAL | FDR/Confidence/EV filters | "CrowdIntel Ingest" |
| **Order submission** | ⚠️ Real HTTP pero a endpoint 404 | `Code: 404` | "Wire Transmission" |
| **AVX2/SIMD crypto** | ❌ NO EXISTE | No hay intrinsics SIMD en el código | "AVX2/SIMD implementation" (FINAL_AUDIT, ARCHITECTURE) |
| **Rust hot path** | ❌ NO EXISTE | No hay código Rust en el repo | "C++/Rust" (ARCHITECTURE) |
| **Circuit Breakers** | ❌ NO EXISTE | No hay código de circuit breaker | "Kelly Sizing + Circuit Breakers" (FINAL_AUDIT) |
| **PGO build** | ❌ NO EXISTE | `make pgo-build` no existe | "LTO/PGO optimized" (PERF_METRICS) |
| **MutaLambda evolution** | ❌ SIMULATED (dry-run) | Adapter falla import, emite 2.14% fabricado | "Real (MutaLambda v5.0)" (OPTIMIZATION_LINEAGE) |
| **Valgrind/Massif** | ❌ NUNCA EJECUTADO | `mem_audit.py` es stub, nunca corre valgrind | "✅ Verified" (PERF_METRICS) |
| **Kernel tuning** | ⚠️ Script existe, no aplicado | `kernel_tuning.sh` requiere root + reboot | "`kernel_tuning.sh` validated" (FINAL_AUDIT) |

---

## 7. Claim Falsos Detectados en Documentación

### 7.1 FINAL_AUDIT.md — "APPROVED FOR PRODUCTION"
> **"Final Verdict: APPROVED FOR PRODUCTION."**

- ❌ **Contradicción directa:** El keccak256 está roto → firmas EIP-712 inválidas. No se puede operar.
- ❌ **Credenciales placeholder:** `"YOUR_API_KEY"` en main_hot_path.cpp.
- ❌ **Nonce mock:** Timestamp hardcoded.
- ❌ **Build binary no linka correctamente:** El `crowdintel_bot` retornado 404 en submit_order.

### 7.2 FINAL_AUDIT.md — "EIP-712 Signing ≤ 25µs — ✅ AVX2/SIMD implementation in C++/Rust"
- ❌ **No hay AVX2/SIMD:** El código usa OpenSSL EVP API, no intrinsics SIMD.
- ❌ **No hay Rust:** No hay código Rust en el repositorio.
- ❌ **Timing real:** ECDSA signing en el benchmark toma ~957µs, no ≤25µs.

### 7.3 FINAL_AUDIT.md — "Tick-to-Wire < 50µs ✅ RDTSC Benchmarks show P99 ≈ 26µs"
- ❌ **P99 real medido:** 957µs (1257524 ciclos @ 0.372 ns/cycle). No 26µs.

### 7.4 FINAL_AUDIT.md — "Risk Management: ✅ Kelly Sizing + Circuit Breakers"
- ❌ **No hay circuit breakers:** No hay código de circuit breaker en ningún archivo.

### 7.5 README.md — "It compiles, signs orders using OpenSSL's ECDSA"
- ⚠️ **Parcial:** Compila (sí). Firma órdenes (sí, pero con hash incorrecto → firmas inválidas).

### 7.6 README.md / OPTIMIZATION_LINEAGE.md — "P99: 24 cycles / 7.8ns"
- ❌ **Medido real:** P99 = 2,573,186 ciclos / 957.353µs. Off by factor ~100,000×.

### 7.7 OPTIMIZATION_LINEAGE.md — "Real (MutaLambda v5.0): 2.14% improvement, AVX-512"
- ❌ **Dry-run:** `mutualambda_adapter.py` no encuentra el engine → `dry_run=True` → emite "2.14%" fabricado.
- ❌ **Import falla:** `from runners import SubprocessRunner` → ImportError.
- ❌ **Fechas futuras:** `2026-09-17` (hoy es 2026-09-17, pero el commit fue etiquetado como "future").

### 7.8 PERF_METRICS.md — "✅ Verified. std::vector and new are prohibited in /include/, /src/, and /crypto"
- ❌ **Falso:** `SPSC_RingBuffer` usa `std::unique_ptr<T[]>` (heap alloc). `EIP712Signer` usa `std::vector<uint8_t>` para `der_signature` y `struct_data`. `LightCLOBClient` usa `std::string`.

### 7.9 PERF_METRICS.md / ARCHITECTURE.md — "memory_order_acq_rel verificado"
- ❌ **Inexacto:** El código usa `memory_order_relaxed` para el counter, `acquire` para load y `release` para store. NO usa `memory_order_acq_rel` en ninguna parte. (El patrón es correcto para SPSC, pero la descripción es inexacta.)

### 7.10 PERF_METRICS.md — "make pgo-build" y "./bin/benchmark_latency"
- ❌ **No existen:** No hay target `pgo-build` en CMakeLists. El ejecutable es `latency_bench` en `bin/`, no `benchmark_latency`.

### 7.11 CI/CD — "Run Latency Benchmark (P99 must be < 30us)"
- ❌ **Miente:** El step ejecuta `./bin/crowdintel_bot \|\| exit 1` (el demo, no el benchmark). `crowdintel_bot` siempre exit 0.

### 7.12 WORKFLOW_MAESTRO_POLYMARKET.md — Tabla duplicada
- ⚠️ La tabla de metrics está **duplicada** (líneas 9-17 y 25-33 son idénticas).

---

## 8. Git State

- **HEAD:** `6fbfb06` — "fix: resolve Adversary audit"
- **Branch:** main (limpio respecto a origin/main)
- **Uncommitted changes:** ✅ 1 file modified (`core/crypto/eip712_signer.hpp`) — segundo intento de fix de Keccak, aún broken
- **Untracked files:** `team/` directory (full), root-level `test_keccak*` sources + binaries, `core/build_test/` (creado durante este baseline)
- **AUDITOR_VERDICT.json:** Commiteado en HEAD (`6fbfb06`). Fue escrito contra el estado pre-fix (`56e8b02`).
- **MutaLambda/:** Gitignored (`MutaLambda/` está en `.gitignore`). Existe como subdirectorio pero no es parte del repo.

---

## 9. Resumen Ejecutivo

### Estado General: ⚠️ **PARCIALMENTE IMPLEMENTADO, CRÍTICAMENTE ROTO EN CRYPTO**

| Área | Estado |
|------|--------|
| **Build system** | ✅ Compila (después de fix de HEAD). Depende de conda fallback para CURL. |
| **Criptografía (Keccak-256)** | ❌ **BROKEN** — hash incorrecto. Todas las firmas EIP-712 son inválidas. |
| **Criptografía (ECDSA)** | ⚠️ Real (OpenSSL), pero firma hash incorrecto + double-hashing + v=27. |
| **Credenciales** | ❌ Placeholders ("YOUR_API_KEY") / Dummies (0x01, 0xAA). |
| **NonceManager** | ❌ Mock timestamp hardcoded. |
| **WebSocket listener** | ❌ Simulated (no conexión real). |
| **HMAC auth** | ✅ Código existe, pero sobre payload placeholder en versiones anteriores → ahora sobre JSON real. Creeds siguen siendo placeholders. |
| **Tests** | ⚠️ 3 executables (compilan + corren), pero: 0 ctest, 0 KAT, keccak FAIL, benchmark metricas invalidas. |
| **Benchmarks** | ⚠️ Ejecuta pero P99 real = 957µs (documentado 7.8ns — **falso por factor 100,000×**). |
| **MutaLambda** | ❌ Simulated (dry-run, import falla, resultados fabricados). |
| **Docs** | ❌ Múltiples claim falsos: "APPROVED FOR PRODUCTION", "AVX2/SIMD", "Circuit Breakers", "P99 24 cycles". |

### Prioridad de bloqueadores críticos:
1. **[CRITICAL]** Keccak-256 produce hashes incorrectos → firmas EIP-712 inválidas
2. **[CRITICAL]** Domain separator es `{{0xAB}}` placeholder
3. **[CRITICAL]** Credenciales `"YOUR_API_KEY"` en entrypoint de producción
4. **[CRITICAL]** v = 27 hardcoded (recovery falla 50% del tiempo)
5. **[HIGH]** Double-hashing (SHA-256 sobre hash ya calculado)
6. **[HIGH]** `CURLOPT_NOBODY` con POSTFIELDS (puede descartar el body)
7. **[HIGH]** No hay ctest/assertions en CI
8. **[HIGH]** Documentation claims falsos ("APPROVED FOR PRODUCTION", "AVX2/SIMD", "P99 7.8ns")
