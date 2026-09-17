# 🧬 Lineage de Optimización

Este documento rastrea el origen y la evolución del código del Hot Path del **Bot CrowdIntel**.

---

## 🧪 Motor de Optimización: MutaLambda

- **Proyecto:** [MutaLambda](https://github.com/Adlgr87/MutaLambda)
- **Tipo:** Framework de optimización genética evolutiva para código de sistemas y hot-paths.
- **Rol en este Proyecto:** MutaLambda es el cerebro detrás de la evolución post-compilación del bot. No es una dependencia de código directa, sino un proceso de optimización externo que muta funciones críticas (como `sign_order` o `try_push`) a través de un adaptador desacoplado.

## 🔄 Ciclos de Evolución

| Fecha | Componente Mutado | Versión Base | Resultado del Benchmark |
| :--- | :--- | :--- | :--- |
| `2024-09-17` | `SPSC_RingBuffer::try_push` | `v1.0.0` | Simulado (Dry-Run): 2.14% de mejora en ciclos. |
| `2024-09-17` | `EIP712Signer::sign_order` | `v1.0.0` | Simulado (Dry-Run): 2.14% de mejora en ciclos. |
| `2024-09-17` | `ExecutionEngine::run_tick` | `v1.0.0` | Simulado (Dry-Run): 2.14% de mejora en ciclos. |

## 🧭 Cómo Reproducir

Para ejecutar los ciclos de optimización:
1. Asegúrate de tener el motor de MutaLambda instalado en tu `$MUTALAMBDA_PATH`.
2. Ejecuta el script de optimización:
   ```bash
   python3 infra/scripts/mutalambda_optimize.py
   ```
3. El adaptador (`infra/mutalambda/adapter/mutalambda_adapter.py`) se encargará de la comunicación con el motor.
