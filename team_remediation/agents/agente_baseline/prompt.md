# AGENTE_BASELINE — Fase 0: Línea Base e Higiene

## Rol
Establecer la línea base de CI, limpiar higiene de secretos, y crear la rama `remediation/compliance`.

## Branch
```
remediation/00-baseline
```

## Tareas (del WORKFLOW_REMEDIACION_CUMPLIMIENTO.md)

### T0-1: Añadir `.env`, `secrets/` a `.gitignore`
- **Archivo**: `.gitignore` (repo raíz)
- **Criterio**: `git check-ignore .env` devuelve match
- **Acción**: Añadir `*.env`, `.env*`, `secrets/`, `core/build/` si no está

### T0-2: Eliminar main_hot_path.cpp del target de producción
- **Archivo**: `core/CMakeLists.txt`, `core/src/main_hot_path.cpp`
- **Criterio**: Build determinista sin referencias rotas; `grep -r "YOUR_API_KEY_HERE"` vacío
- **Acción**: 
  1. Remover `YOUR_API_KEY_HERE` de `main_hot_path.cpp` (si existe) — usar env vars
  2. Excluir `main_hot_path.cpp` del target `crowdintel_bot` en CMakeLists (solo para demo)
  3. Verificar `grep -r "YOUR_API_KEY_HERE" core/ alpha/` devuelve vacío

### T0-3: Unificar cliente HTTP
- **Archivo**: `core/src/lightweight_client.hpp`
- **Criterio**: Solo un cliente HTTP en el código de producción
- **Acción**:
  1. Confirmar que `lightweight_client.hpp` es el cliente de producción
  2. Documentar que `mock_client.hpp` es solo para benchmarks
  3. Si existe cliente obsoleto con Bearer/X-Nonce, borrarlo o marcar como legacy
  4. Añadir comentario de claridad en CMakeLists

### T0-4: Registrar línea base
- **Archivo**: `docs/PERF_METRICS.md`
- **Criterio**: Números base registrados antes de tocar nada
- **Acción**:
  1. Ejecutar `cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)` en limpio
  2. Ejecutar `ctest --output-on-failure` y registrar resultado
  3. Ejecutar `./bin/latency_bench` y registrar P50/P99
  4. Actualizar `docs/PERF_METRICS.md` con la línea base REAL

## Restricciones
- NO tocar lógica de trading
- NO tocar `eip712_signer.hpp` (criptografía)
- Preservar todos los vectores KAT existentes
- Branch-predicted y O(1) en hot path

## Deliverables
1. Branch `remediation/00-baseline` creada
2. `.gitignore` actualizado
3. `CMakeLists.txt` actualizado (main_hot_path excluido del target producción)
4. `docs/PERF_METRICS.md` actualizado con baseline real
5. CTest verde (2/2 tests)
6. Comprobante de `git check-ignore .env` y `grep -r "YOUR_API_KEY_HERE"`

## Protocolo de Verificación
```bash
cd core
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
ctest --output-on-failure
./bin/latency_bench
cd ../.. && git check-ignore .env
grep -r "YOUR_API_KEY_HERE" core/ alpha/ || echo "CLEAN"
```
