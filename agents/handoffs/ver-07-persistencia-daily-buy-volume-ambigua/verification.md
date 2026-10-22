# VERIFICATION — VER-07: Ambigüedad persistencia daily_buy_volume_

**Verificador:** AUD-DOCS + AUD-RISK
**Fecha:** 2026-01-22
**Commit base:** c157c23

---

## Verdict: VERIFIED_PASS — Persistencia implementada y testeada, docs inconsistentes

### Finding

The observation **CONFIRMS** that the persistence implementation EXISTS and is tested (test_daily_volume_persist.cpp), but the documentation is inconsistent: README says "si el proceso muere, el contador se reinicia" (T1.3) while the code actually persists via atomic write (temp + rename).

### Evidence

**Implementation exists:** `core/src/execution_engine.hpp` líneas 817-857

```cpp
// T1.3: Persist daily buy-volume state to file (atomic write: temp + rename)
bool save_daily_volume_state(const char* path) const noexcept {
    char tmp_path[512];
    std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE* f = std::fopen(tmp_path, "wb");
    // ... write [vol: double][day: uint64_t] ...
    return std::rename(tmp_path, path) == 0;  // atomic
}

bool load_daily_volume_state(const char* path) noexcept {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;  // missing file → false
    // ... read + validate ...
}
```

**Formato de persistencia:**
- 8 bytes: `double vol` (daily buy volume)
- 8 bytes: `uint64_t day` (UTC day number)
- Total: 16 bytes, scritto atomicamente via temp + rename

**Tests existen:** `tests/unit/test_daily_volume_persist.cpp` (130 líneas, 3 tests)

| Test | Valida | Resultado |
|------|-------|-----------|
| Test 1: Save/load round-trip | vol=30.0, current day → load preserves | ✅ PASS |
| Test 2: New day resets | vol=45.0, old_day → load + check_daily_reset → vol=0 | ✅ PASS |
| Test 3: Missing file graceful | path doesn't exist → returns false | ✅ PASS |

**Documentación inconsistente:**

1. **README.md línea 86-90** (T1.3):
   > "daily_buy_volume_ sobrevive a reinicios mediante escritura atómica (temp + rename)..."
   
   ✅ Documenta la persistencia correctamente AQUÍ.

2. **Pero el observation document (workflow DSH.md)** dice:
   > "si el proceso muere, el contador se reinicia"
   
   Esto CONTRADICE el código. El observation se refiere a documentación OLD que decía que el contador se reiniciaba.

### Analisis de corrupción

The test file does NOT include a corruption test:

```
$ grep -i "truncat\|corrupt\|incomplete" tests/unit/test_daily_volume_persist.cpp
# Resultado: 0 matches
```

**Gap:** No hay test para archivo truncado (ej. escritura interrumpida a mitad del archivo). El formato binario de 16 bytes es frágil: si el proceso muere después de `fopen(tmp)` pero antes de `rename()`, el archivo `.tmp` queda truncado. En el próximo arranque, `load_daily_volume_state()` leería datos parciales.

**Análisis del load:**
```cpp
bool ok = true;
ok &= (std::fread(&vol, sizeof(vol), 1, f) == 1);
ok &= (std::fread(&day, sizeof(day), 1, f) == 1);
return ok;  // Returns false if fread fails → arranca en cero
```

✅ El código valida que fread retorna 1. Si el archivo está truncado:
- Si falta el `day`: vol se lee, day no → ok=false → returns false → arranca en cero ✅
- Si falta el `vol`: vol no se lee → ok=false → returns false → arranca en cero ✅

**El código handlea corrupción gracefulmente**, pero no hay test explícito.

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| Persistencia implementada | ✅ | execution_engine.hpp:817-857 |
| Test de persistencia | ✅ | test_daily_volume_persist.cpp (3 tests) |
| Test de corrupción | ❌ | No existe test para archivo truncado |
| Docs consistentes | ⚠️ | README dice persiste, workflow obs dice "se reinicia" |

**Veredicto: VERIFIED_PASS con observación menor** — La implementación es correcta, pero:
1. La documentación en el workflow DSH.md está desactualizada
2. Faltan tests de corrupción (archivo truncado)

## Recomendación para IMP

1. **Añadir test de corrupción**: `test_daily_volume_corruption.cpp` — archivo truncado → load returns false
2. **Actualizar docs**: Aclarar que la persistencia existe y funciona (corregir el workflow DSH.md obs)

## Artefactos

- `agents/handoffs/ver-07-persistencia-daily-buy-volume-ambigua/artifacts/persistence_analysis.md`
- `reports/docs/ver-07-persistence.md`

## Signoff