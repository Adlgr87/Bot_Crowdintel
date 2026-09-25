# Tests Test Audit — Bot CrowdIntel

## Metadata
| Field | Value |
|---|---|
| **Auditor** | Tests (opencode/openai-gpt-4o-mini) |
| **Repository** | `/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel` |
| **Commit (HEAD)** | `6fbfb06` — "fix: resolve Adversary audit — real Keccak-256, real build system, real benchmarks, real MutaLambda integration" |
| **Date** | 2025-09-17 |
| **Overall Verdict** | ⚠️ **NO AUTOMATED TEST SUITE. Zero CTest. Coverage: 0% for all modules.** |

> **Nota clave:** El repositorio contiene archivos de test y benchmark, pero **ninguno está registrado con CTest**. No hay `enable_testing()`, `add_test()`, ni framework de testing (GoogleTest, Catch2, doctest). Los archivos `.cpp` de test son ejecutables `main()` con `return 0`/`return 1`, no tests automatizados.

---

## 1. Tests Existentes y Estado

### 1.1 CMakeLists.txt — Análisis de Test Infrastructure

**Archivo:** `core/CMakeLists.txt` (109 líneas)

#### `enable_testing()` — **AUSENTE**
- Nunca se llama `enable_testing()` en ningún lugar.
- No hay `include(CTest)` ni framework de testing importado.

#### `add_test()` — **AUSENTE**
- No hay ninguna llamada a `add_test()`.
- Cero pruebas registradas en CTest.

#### Targets de test definidos:
El `CMakeLists.txt` define 3 targets de ejecución (líneas 82–109), pero **NINGÚN uno está registrado con CTest**:

| Target | Source | Tipo | CTest registrado |
|---|---|---|---|
| `test_signer` | `crypto/test_signer.cpp` | Ejecutable (main) | ❌ No |
| `latency_bench` | `../tests/benchmarks/latency_bench.cpp` | Ejecutable (main) | ❌ No |
| `l2_backtester` | `../tests/replay/l2_backtester.cpp` | Ejecutable (main) | ❌ No |

#### Build de fuentes:
- `file(GLOB_RECURSE CORE_SOURCES "src/*.cpp" "../alpha/crowdintel/*.cpp")` — solo compila el hot path.
- `test_signer`, `latency_bench`, `l2_backtester` son targets independientes que **no comparten código con el hot path** (excepto includes).
- No hay GoogleTest, Catch2, o ningún framework importado vía `FetchContent` o `find_package`.

#### Flags de testing:
- `-DNDEBUG` está activo (línea 21) → **deshabilita todas las aserciones `assert()`**.
- No hay `-fsanitize=address,undefined` configurado para tests.
- No hay modo `Debug` disponible para tests en CI (solo `Release`/`RelWithDebInfo`).

#### Discrepancia con AUDITOR_VERDICT.json:
> "core/crypto/test_signer.cpp, tests/benchmarks/latency_bench.cpp and tests/replay/l2_backtester.cpp are NOT CMake targets and are never compiled by make"

**Este claim es INACURANTO PARA EL CÓDIGO ACTUAL.** El `CMakeLists.txt` actual (commit `6fbfb06`) SÍ define estos 3 como targets via `add_executable()`. El auditor estaba revisando una versión anterior. Sin embargo, el claim sigue siendo parcialmente válido en el sentido de que **no están registrados con CTest** — `make` compila los binarios pero `ctest` no los ejecuta.

#### Discrepancia con BASELINE.md:
BASELINE.md (líneas 29-32) afirma correctamente:
> "test_signer en CMake: ✅ Sí", "latency_bench en CMake: ✅ Sí", "l2_backtester en CMake: ✅ Sí"

Pero luego también afirma (línea 33):
> "ctest configurado: ❌ NO"

Esto es correcto.

---

### 1.2 `core/crypto/test_signer.cpp` — Análisis Detallado

**Archivo:** `core/crypto/test_signer.cpp` (40 líneas)

#### ¿Known-answer vectors? — **NO**
El test:
1. Crea una llave privada dummy: `std::vector<uint8_t> test_private_key(32, 0xAA)` (línea 9)
2. Construye un `EIP712Signer` con esa llave
3. Llena `OrderParams` con valores hardcodeados (salt=123456789, maker=0x11×20, etc.)
4. Llama `signer.sign_order(params, signature)`
5. **Imprime** los bytes r y s en hex
6. Retorna 0 si no lanzó excepción, 1 si lanzó

**No hay ninguna comparación con un valor esperado conocido (known-answer vector).** La firma se imprime pero nunca se verifica contra un valor canónico. No hay `assert()`, `memcmp()` contra expected, ni `if (result != expected) return 1`.

#### ¿Solo "no throw"? — **SÍ**
El test es **puramente un smoke test / "does it crash?"**:
```cpp
try {
    EIP712Signer signer(test_private_key);
    // ... fill params, sign ...
    return 0;
} catch (const std::exception& e) {
    return 1;
}
```
- No valida que la firma sea correcta criptográficamente.
- No valida que el hash Keccak sea correcto.
- No valida el recovery id `v`.
- No valida que el struct hash coincida con el EIP-712 domain.

#### Contradicción documental:
- **WORKFLOW_MAESTRO_POLYMARKET.md** (línea 13): `"Verificación: test_signer PASS"` — implica verificación de corrección, pero el test solo verifica "no se lanza excepción".
- **AUDITOR_VERDICT.json** (línea 28): `"test_signer.cpp ... performs no known-answer EIP-712 test-vector verification (docs claim 'test_signer PASS' but it is unverifiable)."` — Correcto, pero la parte sobre keccak sería ahora inexacta (ver sección 5).

#### Test files de Keccak conocidos (NO tracked by git):
Hay 3 archivos en el repo root que **NO están en git** (`.gitignore` no los excluye, simplemente no fueron `git add`):

| Archivo | Conocido-answer? | Git-tracked? | CTest? |
|---|---|---|---|
| `test_keccak.cpp` | ✅ Sí — verifica `keccak256("")` = `c5d24601...5d85a470` | ❌ No | ❌ No |
| `test_keccak_verify.cpp` | ✅ Sí — verifica keccak256("") usando `memcmp` contra bytes esperados | ❌ No | ❌ No |
| `test_keccak_debug.cpp` | ✅ Sí — compara `keccak256_hash` contra una implementación de referencia `keccak256_ref` | ❌ No | ❌ No |

Estos son **tests reales con known-answer vectors**, pero:
- No están integrados en CMake.
- No están en git (archivos "fantasmas" del filesystem).
- No están registrados con CTest.
- `test_keccak_debug.cpp` es particularmente interesante: incluye una **implementación de referencia independiente** de Keccak-256 para comparar contra la implementación en `eip712_signer.hpp`.

---

### 1.3 `tests/benchmarks/latency_bench.cpp` — Análisis Detallado

**Archivo:** `tests/benchmarks/latency_bench.cpp` (122 líneas)

#### Estado general:
- ✅ Es un target CMake (`add_executable(latency_bench ...)`)
- ❌ No está registrado con CTest
- ❌ No hay CI que lo ejecute como benchmark (CI ejecuta `./bin/crowdintel_bot`, no `latency_bench`)
- ✅ Tiene calibración TSC, warmup, y percentiles
- ❌ **Grave problema de metodología**: no filtra ticks vacíos (ver sección 3)

#### ¿Compila y ejecuta?
- El target compiló y existe en `core/build/bin/latency_bench` (artefacto de build previo)
- Pero **no existe en CI** — el workflow solo corre `./bin/crowdintel_bot`

#### Discrepancia con AUDITOR_VERDICT.json:
> "latency_bench.cpp is not a CMake target"

**INACCURANTO.** Es un target CMake (línea 92 del CMakeLists.txt). El auditor estaba revisando código anterior.

---

### 1.4 `tests/replay/l2_backtester.cpp` — Análisis Detallado

**Archivo:** `tests/replay/l2_backtester.cpp` (57 líneas)

#### Estado general:
- ✅ Es un target CMake (`add_executable(l2_backtester ...)`)
- ❌ No está registrado con CTest
- ❌ Es un **esqueleto / stub** — `main()` solo imprime "🧪 L2 Backtester initialized. Ready for tick replay." y retorna 0
- ❌ `run_replay()` tiene cuerpos `// ...` vacíos (líneas 43-45)
- ❌ `simulate_fill()` tiene un **riesgo de división por cero**: `total_cost / filled_size` cuando `filled_size == 0`

#### ¿Verifica algo?
No. Es un esqueleto con la interfaz definida pero sin implementación ni aserciones.

---

### 1.5 `tests/benchmarks/mem_audit.py` — Análisis Detallado

**Archivo:** `tests/benchmarks/mem_audit.py` (22 líneas)

#### Estado general:
- ❌ No es un script funcional — es un **stub**
- ❌ La llamada `subprocess.run(cmd, check=True)` está **comentada** (línea 16)
- ❌ Imprime: `"⚠️  Skipping actual execution: binary not yet compiled. Tooling logic is ready."`
- ❌ El binario hardcodeado `./bot_bin` no coincide con lo que produce CMake (`bin/crowdintel_bot`)
- ❌ No está registrado en CI
- ❌ No usa Python testing framework (pytest, unittest, etc.)

#### ¿Verifica algo?
No. No ejecuta Valgrind, no mide nada, no produce resultados.

---

## 2. Cobertura Actual

### Resumen de cobertura: **0% para todos los módulos**

| Módulo | Archivo | Tests dedicados | Cobertura | Status |
|---|---|---|---|---|
| **Crypto (EIP-712)** | `core/crypto/eip712_signer.hpp` | 0 (test_signer es smoke test) | 0% | ❌ |
| **SPSC Ring Buffer** | `core/include/spsc_ring_buffer.hpp` | 0 | 0% | ❌ |
| **OrderBookL2** | `core/include/order_book.hpp` | 0 | 0% | ❌ |
| **KellyEngine** | `alpha/strategy/kelly_engine.hpp` | 0 | 0% | ❌ |
| **AlphaParser** | `alpha/crowdintel/alpha_parser.cpp` | 0 | 0% | ❌ |

### 2.1 Crypto (EIP-712): 0% de cobertura verificable

**Código bajo test:** `core/crypto/eip712_signer.hpp` (262 líneas)
- Implementa `keccak256_hash()` — Keccak-f[1600] completo con 24 rounds
- Implementa `EIP712Signer` — carga de llave secp256k1, sign_order con OpenSSL EVP
- `OrderParams` struct con `__attribute__((packed))`

**Tests:**
- `test_signer.cpp`: Smoke test solo verifica "no throw". No valida:
  - Conocimiento de Keccak-256 contra vectores canónicos
  - Correctez de la firma ECDSA
  - Valor de `v` (recovery id)
  - Estructura EIP-712 (domain separator, struct hash)
- `test_keccak.cpp` / `test_keccak_verify.cpp` / `test_keccak_debug.cpp`: Existen en filesystem pero **no están en git** y **no están en CMake/CTest**. Verifican keccak256("") contra el vector canónico `c5d24601...5d85a470`.

**Estado del keccak en el código actual:**
El commit `6fbfb06` ("fix: resolve Adversary audit") reemplazó el stub SHA-256 por una implementación real de Keccak-f[1600]. La implementación en `eip712_signer.hpp` (líneas 36-122) incluye:
- ✅ Padding pad10*1 con `0x01` (correcto para Keccak, no SHA3-256)
- ✅ Constants de ronda correctas
- ✅ Rotation offsets `ROT[5][5]` correctos (coinciden con `test_keccak_debug.cpp`)
- ✅ Absorción/squeeze correctos (17 lanes × 8 bytes = 136 bytes rate)

**Sin embargo, persisten bugs criptográficos** (documentados en BASELINE.md):
- Domain separator es `{0xAB}` padded con zeros (placeholder)
- `v` hardcodeado a 27 (rompe ~50% de verificaciones de recuperación)
- Double-hashing: `EVP_sha256()` sobre el hash keccak resultante (línea 214) — OpenSSL hash-againea el digest
- Private key hardcodeada (`0x01×32`, `0xAA×32`)
- Salt hardcodeado (`12345`)

### 2.2 SPSC Ring Buffer: 0% cobertura

**Código bajo test:** `core/include/spsc_ring_buffer.hpp` (64 líneas)
- `SPSC_RingBuffer<T, Capacity>` — template lock-free SPSC queue
- Métodos: `try_push()`, `try_pop()`, `capacity()`
- Usa `memory_order_relaxed` para head/tail, `acquire`/`release` para sincronización

**Tests:** Cero tests dedicados. Solo se ejerce indirectamente en `latency_bench.cpp` (que no valida corrección).

**Riesgos no testeados:**
- `try_pop()` llama `buffer_[current_tail].~T()` (destructor explícito) — para tipos no-POD, el copy elision y el destructor manual pueden causar doble-free o use-after-free
- `try_push()` usa placement-new `new (&buffer_[current_head]) T(item)` — no hay test de que funcione con tipos non-trivially-constructible
- No hay test de race conditions, overflow, underflow, o comportamiento con `std::optional` en `try_pop()`

### 2.3 OrderBookL2: 0% cobertura

**Código bajo test:** `core/include/order_book.hpp` (54 líneas)
- `OrderBookL2` — 100 niveles de bids/asks, `std::array` estático, `std::atomic<uint64_t> sequence_`
- Métodos: `update_bid()`, `update_ask()`, `get_bid()`, `get_ask()`, `get_sequence()`

**Tests:** Cero tests. Solo se usa en benchmarks.

**Riesgos no testeados:**
- `get_bid(level)` / `get_ask(level)` **no verifican bounds** — acceso a `level >= MAX_LEVELS` causa UB (fuera de rango de `std::array`)
- No hay test de thread safety (aunque `sequence_` usa atomics, los arrays `bids_`/`asks_` no están protegidos)
- No hay test de que `update_bid`/`update_ask` incrementan correctamente `sequence_`

### 2.4 KellyEngine: 0% cobertura

**Código bajo test:** `alpha/strategy/kelly_engine.hpp` (31 líneas)
- `calculate_fractional_kelly(ev, confidence, fraction)` — fórmula simplificada
- `calculate_position_size(kelly_f, max_position_usd)`

**Tests:** Cero tests.

**Riesgos no testeados:**
- No hay test de casos edge: `ev <= 0` (debe devolver 0), `ev` muy grande (debe clamp a 1.0), `confidence` out of range
- La fórmula `optimal_f = ev * confidence` no es la fórmula real de Kelly — no hay validación matemática

### 2.5 AlphaParser: 0% cobertura

**Código bajo test:** `alpha/crowdintel/alpha_parser.cpp` (51 líneas)
- `AlphaParser` — filtra señales por q-value, confidence, EV; construye `AlphaSignal` POD; push a SPSC queue
- Filtros: `q_val > 0.05` → reject, `conf < 0.85` → reject, `ev < 0.02` → reject

**Tests:** Cero tests.

**Riesgos no testeados:**
- `strncpy(signal.market_slug, market.c_str(), sizeof(signal.market_slug) - 1)` — no garantiza null-termination si `market` tiene >= 31 chars
- No hay test de los tres filtros de rechazo
- No hay test de overflow del queue (cuando `try_push` retorna false)
- `timestamp_ns` es hardcodeado a `123456789` (mock) en lugar de usar RDTSC en tiempo real

---

## 3. Metodología del Benchmark (`latency_bench.cpp`)

### Aspectos positivos ✅

| Elemento | Implementado | Detalle |
|---|---|---|
| **Calibración TSC** | ✅ Sí | Líneas 27-33: `rdtscp()` + `chrono::high_resolution_clock` con `sleep(100ms)` para calcular `ns_per_cycle` |
| **Warmup** | ✅ Sí | `WARMUP_TICKS = 5000` (línea 21), fase de warmup con refill continuo (líneas 52-66) |
| **Percentiles** | ✅ Sí | Min, P50, P99 calculados (líneas 110-118) |
| **Refill de queue** | ✅ Sí | Refill periódico durante medición (líneas 85-95) |
| **rdtscp inline asm** | ✅ Sí | Usa `asm volatile("rdtscp")` para lectura directa de TSC |

### Aspectos negativos ❌

| Elemento | Problema | Detalle |
|---|---|---|
| **Filtrado de ticks vacíos** | ❌ NO filtra | `run_tick()` llama `try_pop()`; si retorna `nullopt`, no hace nada pero el latido se graba igual. ~7800 de 20000 ticks son vacíos (vértigo 3.2). |
| **Queue overflow** | ❌ Ignora `try_push` return | `QUEUE_CAPACITY = 4096`, pero se pre-fill con `4095` ítems y se refilled con `4095` ítems múltiples veces. `try_push` retorna false cuando está lleno, pero el return value es ignorado. |
| **Sin confidence intervals** | ❌ No reporta | No hay CIs, ni error estándar, ni múltiples repeticiones |
| **CPU isolation** | ❌ No usa taskset | No hay `taskset -c` ni `isolcpus` para aislar el benchmark del scheduler |
| **No register CTest** | ❌ No es test | No está en `add_test()` ni en CI |
| **Mock client imprime** | ❌ I/O en hot path | `MockCLOBClient::submit_order` es no-op (OK), pero `BenchExecutionEngine` crea un `EIP712Signer` con key hardcodeada `0xAA×32` |

### Análisis de queue overflow (problema crítico):

**Configuración:**
- `QUEUE_CAPACITY = 4096` (es decir, espacio útil = 4095, ya que SPSC usa un slot para distinguir full/empty)
- `TOTAL_TICKS = 20000`
- `WARMUP_TICKS = 5000`

**Durante pre-fill** (líneas 37-48): Se insertan `4095` ítems → queue está a capacidad límite.

**Durante warmup** (líneas 52-66):
- Cada tick consume 1 ítem via `run_tick()` → `try_pop()`
- Cada `QUEUE_CAPACITY/4 = 1024` ticks, se refilled con `4095` ítems
- Pero el queue tiene capacidad 4095 → la mayoría de los `try_push` en el refill **fallan silenciosamente**
- Después de 5000 ticks de warmup: ~5000 ítems consumidos, ~5 refills × 4095 = ~20475 intents, pero solo ~5000 espacios disponibles → ~15000 pushes fallidos

**Durante measurement** (líneas 83-101):
- Final refill: 4095 ítems (pero ~0 espacio disponible → casi todos fallan)
- 20000 ticks de medición, con refill cada `QUEUE_CAPACITY/2 = 2048` ticks
- Cada tick consume 1 ítem, pero el queue se vacía rápidamente después del initial pre-fill
- **Estimación:** ~12000 ticks vacíos medidos (60% de los ticks) — midiendo el fast-fail de `try_pop()` en queue vacío, NO el hot path real

**Resultado:** El benchmark reportado (P99 = 24 ciclos, línea 118) mide **principalmente el path de `try_pop()` en queue vacío** (un `return std::nullopt` sin casi trabajo), no el hot path real de signing/ordering. Las latencias reportadas son **inválidas** — no representan el costo real de `KellyEngine::calculate_position_size()` + `sign_order()` + `submit_order()`.

### Discrepancia con OPTIMIZATION_LINEAGE.md:

| Claim en docs | Realidad |
|---|---|
| "P99 = 23.5 ciclos (7.8 ns)" | Medido en queue vacío; hot path real no medido |
| "3.0 GHz" hardcoded (ns conversion) | `ns_per_cycle` se calcula en runtime (✅), pero el claim de "7.8 ns" deriva de ticks vacíos |
| "5K warmup" | ✅ Implementado, pero no resuelve el problema de ticks vacíos |
| "20K ticks" | ✅ 20000 TOTAL_TICKS, pero ~12K son vacíos |

---

## 4. Claims Falsos sobre Testing

### 4.1 Claim: "test_signer PASS"

**Fuente:** `WORKFLOW_MAESTRO_POLYMARKET.md` línea 13, `README.md` (implícito)

> "EIP-712 Firma | Real | ✅ OpenSSL/ECDSA | Verificación: `test_signer` PASS"

**Realidad:** `test_signer.cpp` no verifica corrección alguna. Solo comprueba que `sign_order()` no lanza excepción. No hay known-answer vector, no hay verificación de firma, no hay test de hash. El "PASS" es un claim no verificable.

### 4.2 Claim: "SPSC Lock-Free ... memory_order_acq_rel verificado"

**Fuente:** `WORKFLOW_MAESTRO_POLYMARKET.md` línea 15

> "SPSC Lock-Free | Zero Alloc | ✅ Funcional | `memory_order_acq_rel` verificado"

**Realidad:** El código usa `memory_order_relaxed` (para head/tail counters), `memory_order_acquire` (para cargar tail en `try_pop`), y `memory_order_release` (para almacenar head en `try_push`). **Nunca** usa `memory_order_acq_rel`. El claim es incorrecto respecto al orden de memoria (aunque el uso de `relaxed`/`acquire`/`release` es técnicamente correcto para SPSC).

### 4.3 Claim: "RDTSC Benchmark, 20K ticks" como Source of Truth

**Fuente:** `README.md` línea 64, `WORKFLOW_MAESTRO_POLYMARKET.md` línea 11

> "Results (RDTSC Benchmark, 20K iterations): P99 = 24 cycles (7.8ns)"

**Realidad:**
1. El benchmark existe y compila ✅
2. Tiene 20K ticks ✅
3. **Pero:** ~60% de los ticks miden queue vacío (try_pop rápido), no el hot path real ❌
4. El benchmark **no está registrado con CTest** ❌
5. **No se ejecuta en CI** ❌ (CI ejecuta `crowdintel_bot`, no `latency_bench`)
6. Las latencias reportadas son inválidas ❌

### 4.4 Claim: "CI/CD ... Run Latency Benchmark"

**Fuente:** `.github/workflows/ci-cd-and-optimize.yml` línea 30

```yaml
- name: Run Latency Benchmark (P99 must be < 30us)
  run: |
    cd core/build
    ./bin/crowdintel_bot || exit 1
```

**Realidad:** El step está etiquetado como "Run Latency Benchmark" pero ejecuta `./bin/crowdintel_bot` — que es el **demo main_hot_path.cpp** (un loop de 10 ticks con sleep de 100ms entre cada uno), **NO** `latency_bench`. El claim de "(P99 must be < 30us)" es falso — no se mide nada, y `|| exit 0` siempre pasa.

### 4.5 Claim de cobertura de memoria con Valgrind

**Fuente:** `docs/PERF_METRICS.md` línea 37-38

> "Verification Method: `valgrind --tool=massif`"
> "Status: ✅ Verified. `std::vector` and `new` are prohibited..."

**Realidad:** `mem_audit.py` (el script de auditoría de memoria) es un **stub**:
- La llamada `subprocess.run(cmd, check=True)` está **commentada** (línea 16)
- Imprime "⚠️  Skipping actual execution: binary not yet compiled"
- Nunca ejecuta Valgrind
- El binario hardcodeado `./bot_bin` no existe (CMake produce `bin/crowdintel_bot`)

### 4.6 AUDITOR_VERDICT.json: Claims desactualizados

El `AUDITOR_VERDICT.json` contiene varios claims que **eran verdaderos pero ya no lo son** tras el commit `6fbfb06`:

| Claim en AUDITOR_VERDICT.json | Estado actual |
|---|---|
| "keccak256_hash() is a STUB that calls SHA256()" | ❌ FALSO — la implementación actual es Keccak-f[1600] real (24 rounds, pad10*1, rotation offsets correctos) |
| "CI workflow installs only build-essential cmake clang — no curl/SSL dev headers" | ❌ FALSO — CI instala `libcurl4-openssl-dev libssl-dev` (línea 21 del workflow) |
| "test_signer.cpp, latency_bench.cpp and l2_backtester.cpp are NOT CMake targets" | ❌ FALSO — todos 3 están definidos como `add_executable()` en CMakeLists.txt |
| "Dockerfile.prod COPYs /app/build/bot_bin which does not exist" | ✅ Sigue siendo FALSO — Dockerfile usa `/app/build/bin/crowdintel_bot` (corregido) |
| "core/build/* artifacts are stale and non-reproducible" | ⚠️ Parcialmente válido — los artefactos de build están en `.gitignore` y no deberían estar en git, pero el `test_signer` binary SÍ está tracked |

### 4.7 Documentación vs. Realidad

| Documento | Claim | Realidad |
|---|---|---|
| `README.md` línea 7 | "It compiles, signs orders using OpenSSL's ECDSA" | ✅ Compila (Docker), pero keccak double-hashing bug; CI puede fallar sin deps sistema |
| `README.md` línea 66-69 | "Results: Min 20, P50 22, P99 24 cycles" | ❌ Derivado de benchmark con ~60% ticks vacíos |
| `README.md` línea 60-62 | "MutaLambda: memory_order_acquire → memory_order_relaxed" | ✅ El código usa `memory_order_relaxed` en SPSC (correcto) |
| `README.md` línea 61 | "std::vector → std::array in EIP-712 struct hashing" | ⚠️ Parcial — el struct uses `std::vector<uint8_t>` para `struct_data` aún (línea 184) |
| `docs/FINAL_AUDIT.md` línea 8 | "P99 ≈ 26µs" | ❌ Claim sin base en benchmark válido |
| `docs/FINAL_AUDIT.md` línea 24 | "APPROVED FOR PRODUCTION" | ❌ CRÍTICAMENTE FALSO — keccak double-hashing, v hardcodeado, keys placeholder, nonce mock |
| `docs/PERF_METRICS.md` línea 52 | `make pgo-build` | ❌ No existe target `pgo-build` en Makefile/CMake |
| `docs/PERF_METRICS.md` línea 55 | `./bin/benchmark_latency` | ❌ No existe target `benchmark_latency` (el binary es `latency_bench`) |
| `docs/OPTIMIZATION_LINEAGE.md` línea 17 | Fechas `2026-09-17` | ❌ Fechas futuras (el repo es de 2025) |
| `docs/OPTIMIZATION_LINEAGE.md` línea 28 | "mutaciones se aplicaron al benchmark equivalente Python" | ⚠️ Las mutaciones validan Python, NO el C++ hot path — metodológicamente inválido |

---

## 5. Análisis del Keccak-256: Discrepancia entre AUDITOR_VERDICT.json y Código Actual

### Estado en el código actual (commit 6fbfb06):

El `keccak256_hash()` en `core/crypto/eip712_signer.hpp` (líneas 36-122) **NO es un stub de SHA-256**. Es una implementación real de Keccak-f[1600]:

- ✅ **Round constants (RC[24])**: Correctas, coinciden con la especificación Keccak
- ✅ **Rotation offsets (ROT[5][5])**: Correctas, coinciden con `test_keccak_debug.cpp`:
  ```
  {  0, 36,   3,  41, 18 },  // x=0
  {  1, 32,   4,  43, 19 },  // x=1
  { 62,  6,  44,  23, 13 },  // x=2
  { 28, 55,  25,  21, 56 },  // x=3
  { 27, 20,  39,  56,  0 },  // x=4
  ```
- ✅ **Padding**: `padded[len] = 0x01` + `padded[RSIZ-1] |= 0x80` → pad10*1 (correcto para Keccak, NO SHA3-256)
- ✅ **Rate = 136 bytes**: Correcto para Keccak-256 (1088 bits)
- ✅ **24 rounds**: Correcto
- ✅ **Squeeze**: Extrae 32 bytes del state[0..3] en orden little-endian (correcto)

### Bugs persistentes (confirmados por BASELINE.md):

A pesar de que el Keccak está implementado, persisten problemas:

1. **Double-hashing en ECDSA**: `EVP_PKEY_CTX_set_signature_md(EVP_sha256())` + `EVP_PKEY_sign(..., eip712_hash, 32)` — OpenSSL SHA-256's el `eip712_hash` antes de firmar. Un verificador EIP-712 espera una firma directa sobre el hash Keccak, no sobre `SHA256(keccak_hash)`.

2. **`v` hardcodeado a 27**: No se calcula el recovery id de la firma ECDSA real.

3. **Domain separator `{0xAB}`**: No es un domain separator válido de EIP-712.

4. **`test_keccak*.cpp` no están en git ni CMake**: Los archivos que verifican el hash contra vectores canónicos (`c5d24601...`) existen pero no están integrados.

---

## 6. Estado de los Artefactos de Build

### Archivos binarios tracked en git (MALA PRÁCTICA):

| Archivo | Size | Razon de uso |
|---|---|---|
| `core/crypto/test_signer` | 68,984 bytes | ❌ Binary tracked en git — debería estar en `.gitignore` |
| `test_keccak` | 23,264 bytes | ❌ Binary compiled, no tracked |
| `test_keccak_bin` | 23,264 bytes | ❌ Binary compiled, no tracked |
| `test_keccak_debug` | 27,568 bytes | ❌ Binary compiled, no tracked |
| `test_keccak_verify` | 23,224 bytes | ❌ Binary compiled, no tracked |
| `core/build/bin/*` | — | ✅ En `.gitignore` |

### CTestTestfile: **NO EXISTE**

```bash
find core/build -name "CTestTestfile.cmake"  # → (no output)
find core/build -name "CTestConfig.cmake"   # → (no output)
```

No hay configuración de CTest en el árbol de build. El `make test` no está disponible.

---

## 7. Estado de CI/CD

### Workflow: `.github/workflows/ci-cd-and-optimize.yml`

#### Job 1: `build-and-test`
```yaml
- name: Build with LTO/PGO Flags
  run: cmake -DCMAKE_CXX_FLAGS="-O3 -march=native -flto" .. && make -j$(nproc)
- name: Run Latency Benchmark (P99 must be < 30us)
  run: ./bin/crowdintel_bot || exit 1
```

**Problemas:**
1. ❌ Etiqueta dice "Latency Benchmark" pero ejecuta `crowdintel_bot` (demo main, no benchmark)
2. ❌ No hay assertion de latencia — `|| exit 1` siempre pasa porque `crowdintel_bot` retorna 0
3. ❌ No ejecuta `test_signer`, `latency_bench`, o `l2_backtester`
4. ❌ No ejecuta `ctest`
5. ✅ Instala `libcurl4-openssl-dev` y `libssl-dev` (contrario al claim del AUDITOR_VERDICT.json)
6. ✅ Usa Clang (no GCC)

#### Job 2: `mutate-and-optimize`
```yaml
- name: Execute MutaLambda Evolution on Hot Path Functions
  run: python infra/mutalambda/adapter/mutualambda_adapter.py
```

**Problemas:**
- El `mutualambda_optimize.py` es un stub (líneas 9-20: solo imprime mensajes mock de "AVX-512 optimization")
- No ejecuta MutaLambda real

---

## 8. Resumen Ejecutivo: Tests Faltantes

### Prioritario (debe hacerse ya):

| # | Action | Componentes afectados |
|---|---|---|
| 1 | Agregar `enable_testing()` + `add_test()` en CMakeLists.txt | Todo el proyecto |
| 2 | Registrar `test_signer`, `latency_bench` como CTest targets | crypto, latency |
| 3 | Escribir known-answer vector test para `keccak256_hash("")` = `c5d24601...` y más vectores | crypto |
| 4 | Test de EIP-712 signing: verify signature con lib externa o known-answer | crypto |
| 5 | Test de SPSC: push/pop correctness, overflow, underflow, multi-thread | SPSC |
| 6 | Test de OrderBookL2: bounds check, update/get, sequence increment | OrderBook |
| 7 | Test de KellyEngine: edge cases (ev<=0, ev>>1, confidence boundaries) | Kelly |
| 8 | Test de AlphaParser: filter thresholds, strncpy null-termination, queue full | AlphaParser |

### Benchmark methodology fixes:
| # | Action | Detalle |
|---|---|---|
| 1 | Filtrar ticks vacíos | Solo medir ticks donde `signal` es no-nullo |
| 2 | Boundar pre-fill a Capacity | No intentar llenar más de `Capacity-1` |
| 3 | Multiple runs + confidence intervals | N ≥ 5 repeticiones, reportar P50 ± CI |
| 4 | CPU pinning | `taskset -c` + `isolcpus` en CI |
| 5 | No hardcoded 3.0GHz | Calibrar TSC-to-ns en cada run (ya hecho, pero usar para ns conversion) |

---

## 9. Matriz de Compliance de Testing

| Criterio | Estado | Evidencia |
|---|---|---|
| `enable_testing()` en CMakeLists.txt | ❌ FAIL | No existe |
| `add_test()` en CMakeLists.txt | ❌ FAIL | No existe |
| `ctest` funciona | ❌ FAIL | No hay CTestTestfile.cmake |
| test_signer con known-answer vectors | ❌ FAIL | Solo "no throw" |
| test_signer es CMake target | ✅ PASS | `add_executable(test_signer ...)` línea 82 |
| latency_bench es CMake target | ✅ PASS | `add_executable(latency_bench ...)` línea 92 |
| latency_bench registrado en CTest | ❌ FAIL | No `add_test()` |
| latency_bench filtra ticks vacíos | ❌ FAIL | No filtra try_pop() nullo |
| latency_bench en CI | ❌ FAIL | CI ejecuta crowdintel_bot, no latency_bench |
| l2_backtester es CMake target | ✅ PASS | `add_executable(l2_backtester ...)` línea 102 |
| l2_backtester registrado en CTest | ❌ FAIL | No `add_test()` |
| mem_audit.py ejecuta Valgrind | ❌ FAIL | subprocess.run() está comentado |
| Coverage de crypto | 0% | test_signer no valida firma/hash |
| Coverage de SPSC | 0% | Sin test dedicado |
| Coverage de OrderBook | 0% | Sin test dedicado |
| Coverage de Kelly | 0% | Sin test dedicado |
| Coverage de AlphaParser | 0% | Sin test dedicado |
| CI instala dev dependencies | ✅ PASS | `libcurl4-openssl-dev libssl-dev` en línea 21 |
| CI ejecuta tests automatizados | ❌ FAIL | Solo `./bin/crowdintel_bot \|\| exit 1` |
| CI valida latencia P99 < 30µs | ❌ FAIL | No ejecuta benchmark, no parsea output |

---

## 10. Discrepancias entre Documentación y Código Real

| Documento | Sección | Claim | Estado en código actual |
|---|---|---|---|
| `WORKFLOW_MAESTRO_POLYMARKET.md` | línea 13 | "test_signer PASS" | ❌ Falso — solo smoke test, no known-answer |
| `WORKFLOW_MAESTRO_POLYMARKET.md` | línea 15 | "memory_order_acq_rel verificado" | ❌ Falso — código usa relaxed/acquire/release |
| `WORKFLOW_MAESTRO_POLYMARKET.md` | línea 11 | "P99 8.0 ns (24 ciclos)" | ❌ Inválido — ~60% ticks medidos son vacíos |
| `README.md` | línea 7 | "It compiles, signs orders" | ⚠️ Parcial — compila en Docker, pero keccak double-hashing bug |
| `README.md` | línea 66 | "Results RDTSC Benchmark 20K ticks" | ✅ Parcial — benchmark existe pero es inválido |
| `docs/FINAL_AUDIT.md` | línea 24 | "APPROVED FOR PRODUCTION" | ❌ CRÍTICAMENTE FALSO |
| `docs/PERF_METRICS.md` | línea 52 | `make pgo-build` | ❌ No existe target |
| `docs/PERF_METRICS.md` | línea 55 | `./bin/benchmark_latency` | ❌ No existe (es `latency_bench`) |
| `docs/PERF_METRICS.md` | línea 38 | "std::vector/new prohibidos en /include, /src, /crypto" | ⚠️ Parcial — `eip712_signer.hpp` usa `std::vector` (líneas 184, 225) |
| `docs/OPTIMIZATION_LINEAGE.md` | línea 17 | Fechas `2026-09-17` | ❌ Fechas futuras |
| `AUDITOR_VERDICT.json` | línea 22 | "keccak256_hash() is STUB that calls SHA256()" | ❌ FALSO — implementación Keccak real |
| `AUDITOR_VERDICT.json` | línea 10 | "CI installs only build-essential cmake clang" | ❌ FALSO — CI instala curl/SSL dev |
| `AUDITOR_VERDICT.json` | línea 12 | "test_signer/ latency_bench/ l2_backtester NOT CMake targets" | ❌ FALSO — todos son targets |
| `AUDITOR_VERDICT.json` | línea 13 | "Dockerfile.prod COPYs /app/build/bot_bin" | ✅ Sigue falso — Dockerfile usa `bin/crowdintel_bot` |

---

## 11. Conclusión

El repositorio tiene **potencial** de testing pero **falla completamente en ejecución**:

1. **No hay infraestructura de testing automatizado.** No hay `enable_testing()`, `add_test()`, ni `ctest`. Los scripts "test" son ejecutables `main()` con `return 0`.

2. **Los 3 targets CMake existentes no están registrados con CTest.** Aunque compilan, `ctest` no los descubre ni ejecuta.

3. **El "test" de crypto (`test_signer.cpp`) es un smoke test sin known-answer vectors.** Verifica que no lanza excepción, pero no valida la correctitud criptográfica.

4. **El benchmark (`latency_bench.cpp`) tiene calibración y warmup, pero un grave defecto de metodología: non filtra ticks vacíos.** ~60% de los 20K ticks miden el path de `try_pop()` en queue vacío, invalidando todos los números reportados (P99=24 ciclos).

5. **Hay archivos de test de keccak con known-answer vectors (`test_keccak*.cpp`) que NO están en git ni en CMake.** Estos son los únicos tests reales con verificación, pero son artefactos "fantasmas" del filesystem.

6. **`mem_audit.py` y `l2_backtester.cpp` son stubs/esqueletos** que no realizan testing alguno.

7. **La documentación miente sistemáticamente sobre testing:** "test_signer PASS", "8.0 ns P99", "APPROVED FOR PRODUCTION", latencias de "24 cycles" — todos basados en tests no registrados, no ejecutados, o inválidos.

8. **El `AUDITOR_VERDICT.json` contiene claims desactualizados** (keccak stub, no CMake targets, CI sin deps) que fueron corregidos en el commit `6fbfb06`, pero el auditor nunca fue actualizado. Las versiones más recientes (BASELINE.md) son más precisas.

### Recomendación inmediata:
**No hay testing automatizado funcional en este repositorio.** Antes de cualquier optimización o despliegue, se requiere:
1. `enable_testing()` + `add_test()` en CMakeLists.txt
2. Tests con known-answer vectors para keccak256_hash (usar los existentes `test_keccak*.cpp` como base)
3. Test de EIP-712 signing contra vectores canónicos
4. Tests de unidad para SPSC, OrderBook, Kelly, AlphaParser
5. Arreglo del benchmark: filtrar ticks vacíos, boundar queue, múltiples repeticiones
6. Registro de todos los tests en CTest y ejecución en CI

---

*Audit performed by Tests (opencode/openai-gpt-4o-mini) on 2025-09-17.*
*Repository: Bot_Crowdintel, HEAD commit 6fbfb06*
