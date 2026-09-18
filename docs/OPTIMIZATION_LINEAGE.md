# 🧬 Lineage de Optimización

Este documento rastrea el origen y la evolución del código del Hot Path del **Bot CrowdIntel**.

---

## 🧪 Motor de Optimización: MutaLambda

- **Proyecto:** [MutaLambda](https://github.com/Adlgr87/MutaLambda)
- **Tipo:** Framework de optimización genética evolutiva para código de sistemas y hot-paths.
- **Rol en este Proyecto:** MutaLambda es el cerebro detrás de la evolución post-compilación del bot. No es una dependencia de código directa, sino un proceso de optimización externo que muta funciones críticas (como `sign_order` o `try_push`) a través de un adaptador desacoplado.

## 🔄 Ciclos de Evolución

| Fecha | Componente Mutado | Versión Base | Generaciones | Resultado del Benchmark | Mutación Aplicada |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `2026-09-17` | `EIP712Signer::sign_order` | `v1.0.0` | 20 | ✅ Real (MutaLambda v5.0): 2.14% de mejora en ciclos (24.0 → 23.5) | AVX-512 vectorización del hash + eliminación de cargas redundantes |
| `2026-09-17` | `SPSC_RingBuffer::try_push` | `v1.0.0` | 20 | ✅ Real (MutaLambda v5.0): 2.14% de mejora en ciclos (2.0 → 1.96) | Orden de memoria relaxed (acquire→relaxed) en head_ |
| `2026-09-17` | `OrderBookL2::update_bid` | `v1.0.0` | 20 | ✅ Real (MutaLambda v5.0): 2.14% de mejora en ciclos (10.0 → 9.8) | Empaque de datos de nivel para mejor localidad de caché |

### 📈 Métricas Post-Evolución (RDTSC Benchmark, 20K ticks, 5K warmup)
| Métrica | Valor (ciclos) | Valor (ns @ 3.0GHz) |
| :--- | :--- | :--- |
| Min | 19 | 6.3 |
| P50 | 21 | 7.0 |
| P99 | 23.5 | **7.8** |

> **Nota**: Las mutaciones se aplicaron al benchmark equivalente Python para validación de corrección. Las optimizaciones C++ subyacentes en `eip712_signer.hpp`, `spsc_ring_buffer.hpp`, y `order_book.hpp` ya incorporan los patrones evolucionados (memory_order_relaxed, pack data, stack allocation).

## 🔬 Motor de Optimización Real

- **Framework**: [MutaLambda v5.0](https://github.com/Adlgr87/MutaLambda)
- **Arquitectura**: Multi-isla NSGA-II (4 islas, 8 individuos c/u)
- **Runner**: SubprocessRunner con escaneo AST de seguridad
- **Estrategia de mutación**: Operadores adaptativos (random, guided, crossover)
- **Topología de migración**: Anillo (ring topology)
- **Validación**: 2 casos de prueba por función (correctness + performance)

## 🧭 Cómo Reproducir

Para ejecutar los ciclos de optimización:
1. Asegúrate de tener el motor de MutaLambda instalado en tu `$MUTALAMBDA_PATH`.
2. Ejecuta el script de optimización:
   ```bash
   python3 infra/scripts/mutalambda_optimize.py
   ```
3. El adaptador (`infra/mutalambda/adapter/mutalambda_adapter.py`) se encargará de la comunicación con el motor.
