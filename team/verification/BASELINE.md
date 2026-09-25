# BASELINE.md — Phase 0 Baseline and Estado Actual

**Fecha:** 2025-09-17  
**Director:** Director  
**Repo:** https://github.com/Adlgr87/Bot_Crowdintel  
**Commit actual:** `5da1443` (latest in main)  

---

## 1. Estado General del Proyecto

### Veredicto del Auditoría
**⚠️ Estado actual: FAIL (CRITICAL)** — Basado en `AUDITOR_VERDICT.json`

El proyecto tiene **6 categorías en FAIL** con **8 issues críticos**. No está listo para producción.

---

## 2. Estado por Componente

### 2.1 Build System (CMake)
**Estado: BROKEN**

| Aspecto | Estado | Detalle |
|---|---|---|
| CMakeLists.txt | Parcialmente corregido | Tiene `find_package(OpenSSL REQUIRED)` pero el `find_package(CURL)` tiene fallback condicional |
| Dependencias instaladas | NO | `libssl-dev` no instalado, `libcurl4-openssl-dev` no instalado |
| Build desde checkout limpio | NO COMPILA | Error: `fatal error: curl/curl.h: No such file or directory` |
| Targets CMake | 3 definidos | `crowdintel_bot`, `test_signer`, `latency_bench`, `l2_backtester` |
| test_signer en CMake | ✅ Sí | `add_executable(test_signer crypto/test_signer.cpp)` |
| latency_bench en CMake | ✅ Sí | `add_executable(latency_bench ../tests/benchmarks/latency_bench.cpp)` |
| l2_backtester en CMake | ✅ Sí | `add_executable(l2_backtester ../tests/replay/l2_backtester.cpp)` |
| ctest configurado | ❌ NO | No hay `enable_testing()` / `add_test()` |
| Sanitizers | ❌ NO configurados | No ASan, no UBSan en CMake flags |

### 2.2 Criptografía (EIP-712)
**Estado: PARTIALLY FIXED (still CRITICAL)**

| Aspecto | Estado | Detalle |
|---|---|---|
| keccak256_hash() | ✅ Real | Implementación Keccak-f[1600] 24 rounds (líneas 36-122) |
| Domain separator | ❌ STUB | `{0xAB}` padded con zeros (línea 191) |
| Recovery id v | ❌ HARDCODED | `v = 27` (línea 240) |
| Double-hashing | ❌ BUG | `EVP_PKEY_CTX_set_signature_md(EVP_sha256())` + signs the eip712_hash (línea 214) |
| Private key | ❌ HARDCODED | `0x01*32` fallback (línea 88) |
| Salt | ❌ HARDCODED | `12345` en tests, nonce usado como salt (línea 43) |
| test_signer | ⚠️ No verifica conocido | Solo verifica "no throw", no known-answer vectors |

### 2.3 Cliente CLOB (HTTP)
**Estado: BROKEN**

| Aspecto | Estado | Detalle |
|---|---|---|
| CURLOPT_NOBODY | ❌ BUG | `CURLOPT_NOBODY 1L` con `CURLOPT_POSTFIELDS` descarta el POST body |
| CURLOPT_TIMEOUT | ❌ NO | Sin timeout, puede bloquear indefinidamente |
| Payload | ❌ PLACEHOLDER | `order.payload` es `'order_data_placeholder'` |
| HMAC sobre payload | ❌ BUG | HMAC calculado sobre placeholder, no sobre el JSON real |
| Credenciales | ❌ HARDCODED | `'YOUR_API_KEY'/'YOUR_SECRET'/'YOUR_PASSPHRASE'` en main_hot_path.cpp:25 |
| Cert pinning | ❌ NO | Sin pin de certificados |
| secret zeroization | ❌ NO | std::string sin zeroización |
| TLS | ⚠️ Parcial | libcurl gestiona TLS por defecto |

### 2.4 Hot Path / Latencia
**Estado: CLAIMS INVÁLIDOS**

| Aspecto | Estado | Detalle |
|---|---|---|
| std::cout en hot path | ❌ BUG | `submit_order` imprime a stdout (línea 87-88) |
| NonceManager | ❌ MOCK | `get_timestamp_ms()` retorna `1600000000000` constante |
| OrderBookL2 bounds check | ❌ UNSAFE | `get_bid()/get_ask()` sin bounds check en level |
| TSC calibration | ❌ BUG | `cycle_to_us = 1.0/3000.0` hardcodea 3.0GHz |
| latency_bench | ⚠️ Parcialmente fixed | Tiene calibration y warmup, pero... |
| latency_bench en CMake | ✅ Sí | Registrado como target |
| Queue overflow | ❌ BUG | `try_push` return value ignorado (en versiones anteriores) |

### 2.5 Infraestructura
**Estado: BROKEN**

| Aspecto | Estado | Detalle |
|---|---|---|
| Dockerfile.prod | ❌ BROKEN | `COPY /app/build/bot_bin` no existe (CMake emite `bin/crowdintel_bot`) |
| .dockerignore | ❌ NO | Copia todo el árbol stale de `core/build/` |
| CI/CD | ⚠️ Parcial | GitHub Actions existe pero no instala libcurl-dev / libssl-dev |
| kernel_tuning.sh | ⚠️ PELIGROSO | Modifica grub, requiere reboot, puede fallar en VPS |
| deploy_production.sh | ❌ BROKEN | Corre `crowdintel_bot_test` (stale artifact) |
| MutaLambda | ❌ SIMULADO | Adapter path wrong, `dry_run=True`, imports fallan |

### 2.6 Tests
**Estado: INSUFICIENTE**

| Aspecto | Estado | Detalle |
|---|---|---|
| enable_testing() | ❌ NO | No hay ctest |
| add_test() | ❌ NO | No hay tests registrados |
| test_signer.cpp | ⚠️ No verifica | Solo "no throw", no known-answer vectors |
| latency_bench.cpp | ⚠️ Metodología cuestionable | Pre-fill de 20000 en queue de 1024 (versiones anteriores) |
| l2_backtester.cpp | ⚠️ Skeleton | Bodies `// ...` y riesgo de división por cero |
| mem_audit.py | ❌ STUB | Imprime "binary not yet compiled" |
| Cobertura | ❌ CERO | No hay Cobertura de: crypto, SPSC, OrderBook, Kelly, AlphaParser |

### 2.7 Documentación
**Estado: MISALINEADA**

| Documento | Estado | Veredicto |
|---|---|---|
| README.md | ⚠️ Claims falsos | Dice "It compiles, signs orders using OpenSSL" — contradictorio |
| WORKFLOW_MAESTRO_POLYMARKET.md | ⚠️ Claims no verificables | "P99 8.0 ns (24 ciclos)" derivado de benchmark inválido |
| docs/FINAL_AUDIT.md | ❌ CRÍTICO | "APPROVED FOR PRODUCTION" contradice keccak stub, placeholder keys |
| docs/OPTIMIZATION_LINEAGE.md | ❌ FICTICIO | MutaLambda v5.0 "Real", fechas 2026-09-17 (futuro) |
| docs/PERF_METRICS.md | ⚠️ Referencias inexistentes | `make pgo-build`, `./bin/benchmark_latency` no existen |
| AUDITOR_VERDICT.json | ✅ PRECISO | Veredicto honesto: FAIL |

---

## 3. Inventario de Riesgos (Phase 0)

### CRITICAL
1. Build no compila desde checkout limpio
2. keccak256: domain separator es placeholder (0xAB)
3. v recovery id hardcoded a 27 (rompe ~50% de signatures)
4. Double-hashing: EVP_sha256 sobre eip712_hash ya keccak-haseado
5. Private key hardcoded: 0x01*32
6. CURLOPT_NOBODY descarta POST body en submit_order
7. Credenciales placeholder en main_hot_path.cpp
8. MutaLambda simulado (dry_run, imports rotos, fechas futuras)
9. docs/FINAL_AUDIT.md dice "APPROVED FOR PRODUCTION" — FALSO

### HIGH
10. std::cout en hot path (run_tick, submit_order)
11. NonceManager timestamp mock (constante 1600000000000)
12. OrderBookL2 sin bounds check (posible UB)
13. Dockerfile roto (bot_bin no existe)
14. CI sin instalar dependencias de desarrollo
15. No hay ctest, cero tests automatizados
16. latency_bench no filtra ticks vacíos (metodología inválida)

### MEDIUM
17. No hay cert pinning
18. Secrets en std::string sin zeroización
19. kernel_tuning.sh peligroso
20. deploy_production.sh usa binario stale

---

## 4. Criterios de Aceptación (Phase 0)

| Criterio | Estado |
|---|---|
| build limpio desde checkout = PASS | ❌ FAIL |
| cmake configure sin errores | ❌ FAIL |
| test_signer conocido-answer vetores = PASS | ❌ FAIL |
| secrets scan limpio = PASS | ❌ BLOCK |
| ctest con al menos 3 tests = PASS | ❌ FAIL |
| docs alineadas con código real = PASS | ❌ FAIL |

**Veredicto Phase 0:** BLOCKED — El proyecto requiere correcciones críticas antes de proceder a Phase 1.
