# 🧬 Lineage de Optimización

Este documento rastrea el origen y la evolución del código del Hot Path del **Bot CrowdIntel**.

---

## 🧪 Motor de Optimización: MutaLambda

- **Proyecto:** [MutaLambda](https://github.com/Adlgr87/MutaLambda)
- **Tipo:** Framework de optimización genética evolutiva para código de sistemas y hot-paths.
- **Rol en este Proyecto:** MutaLambda es el cerebro detrás de la evolución post-compilación del bot. No es una dependencia de código directa, sino un proceso de optimización externo que muta funciones críticas (como `sign_order` o `try_push`) a través de un adaptador desacoplado.

## 🔄 Ciclos de Evolución

> **Status**: MutaLambda integration is wired but **not yet executed**.
> The adapter is configured to use the in-repo `MutaLambda/` engine via
> `$MUTALAMBDA_PATH`. When the engine is not present, the adapter reports
> `status: "not_configured"` with 0% improvement (no fabricated numbers).

| Fecha | Componente Mutado | Versión Base | Generaciones | Resultado | Mutación Aplicada |
| :--- | :--- | :--- | :--- | :--- | :--- |
| _(pendiente)_ | `EIP712Signer::sign_order` | `v1.0.0` | — | — | Keccak-256 chain-based ρ+π (aplicado manualmente, pendiente de evolución) |
| _(pendiente)_ | `SPSC_RingBuffer::try_push` | `v1.0.0` | — | — | memory_order_relaxed en head_ (aplicado manualmente) |
| _(pendiente)_ | `OrderBookL2::update_bid` | `v1.0.0` | — | — | Pack de datos de nivel para localidad de caché (aplicado manualmente) |

### 📈 Métricas Post-Evolución (RDTSC Benchmark, 20K ticks, 5K warmup)

Medidas reales con `latency_bench` (calibrado TSC → ns vía `clock_gettime`):

| Métrica | Valor (ciclos) | Valor (ns) |
| :--- | :--- | :--- |
| Min | ~2070K | ~770 μs |
| P50 | ~3.2M | ~1.19 ms |
| P99 | ~6.99M | ~2.60 ms |

> **Nota**: El hot path incluye ECDSA signing (Keccak-256 + OpenSSL ECDSA_sign).
> Las latencias anteriores (24 ciclos / 7.8ns) eran de la ruta vacía del queue
> y **no representan** el costo real de signing. El adaptador de MutaLambda genera
> equivalentes Python usando **Keccak-256** (no SHA-256) para validación de
> corrección durante la evolución.

### Optimizaciones aplicadas manualmente (pre-MutaLambda)
1. **Keccak-256**: Implementación chain-based ρ+π (XKCP reference), no tabla ROT.
2. **SPSC_RingBuffer**: Capacidad 4096, `placement new` zero-alloc, `memory_order_release/acquire`.
3. **OrderBookL2**: Bounds check con sentinel, `alignas(64)` para evitar false sharing.
4. **NonceManager**: `clock_gettime(CLOCK_REALTIME)` + contador atómico.
5. **ExecutionEngine**: Sin `std::cout` en `run_tick`, private key desde env var.

## 🔬 Motor de Optimización Real

- **Framework**: [MutaLambda](https://github.com/Adlgr87/MutaLambda)
- **Arquitectura**: NSGA-II multi-isla
- **Runner**: `SubprocessRunner` con escaneo AST
- **Validación**: Test cases por función (correctness + performance)

## 🧭 Cómo Reproducir

1. Clona el motor MutaLambda:
   ```bash
   git clone https://github.com/Adlgr87/MutaLambda MutaLambda
   ```
2. Instala dependencias Python:
   ```bash
   pip install numpy scipy click rich pyyaml pydantic msgpack requests pycryptodome
   ```
3. Ejecuta el adaptador:
   ```bash
   export MUTALAMBDA_PATH="$PWD/MutaLambda"
   export PYTHONPATH="$PWD/MutaLambda:$PYTHONPATH"
   python infra/mutalambda/adapter/mutalambda_adapter.py
   ```
4. El adaptador generará equivalentes Python con **Keccak-256** y ejecutará
   la evolución genética sobre las funciones objetivo.
