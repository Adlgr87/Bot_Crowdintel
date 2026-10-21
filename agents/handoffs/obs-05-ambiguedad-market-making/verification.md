# VERIFICATION — OBS-05: Ambigüedad "no market-making" vs LadderBuilder

**Verificador:** AUD-ARCH + AUD-DOCS
**Fecha:** 2026-01-22
**Commit base:** c157c23

---

## Verdict: VERIFIED_FAIL — Documentación contradictoria sobre market-making

### Finding

The observation **CONFIRMS** an ambiguity: the README and ARCHITECTURE_MAP describe LadderBuilder as constructing discrete quotes, while ARCHITECTURE_MAP.md line 53 mentions "LadderSkew" which implies market-making. The actual code in LadderBuilder does construct bid/ask pairs but does NOT do continuous cancel/replace quote management.

### Evidence

**Archivo:** `core/src/ladder_builder.hpp` (análisis completo)

- `LadderBuilder::build()` construye un **single bid/ask quote pair** (líneas 53-121)
- NO cancel/replace loop — quotes se construyen y se envían como órdenes discretas
- `Quote` struct (líneas 43-49): `{ bid_price, ask_price, bid_size, ask_size, is_active }`
- NO lógaja de "quote management" continuo — each invoke is standalone

**README.md línea 11:**
> "Consume señales de alpha vía HTTP, las evalúa contra un libro de órdenes L2 en memoria, y envía ódenes firmadas (EIP-712) al CLOB."

→ **No dice market-making.**

**ARCHITECTURE_MAP.md línea 53:**
> `| LadderSkew | Hot (before order construction) | Hot | < 3μs p50 | 16-order pool construction |`

→ Menciona "16-order pool" y "LadderSkew" que suena a market-making, pero LadderBuilder solo hace 1 pair.

**README.md línea 140:**
> "LadderBuilder + TimeStrategy: quotes con TTL"

→ Implica quotes con TTL, que es un patrón de market-making (cancel/replace).

### ¿Hay cancel/replace continuo?

**Archivo:** `core/src/presigned_pool.hpp` — El PresignedOrderPool mantiene slots pre-firmados que se consumen. Los slots se "replenisan hacia" (replenished periodically), pero esto es **presigning, no market-making activo**.

**Archivo:** `core/src/time_strategy.hpp` — Define TTL por hora del día, no cancel/replace continuo.

**Archivo:** `core/src/main_hot_path.cpp` — El hot loop procesa AlphaSignals y envía órdenes. NO hay un loop de cancel/replace de quotes.

### Re-quoting detection

**engine_extensions.hpp** — NO re-quoting logic. La función `defense_check()` y `compute_bb_probability()` no hacen re-quoting.

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| Definición clara de discrete execution | ⚠️ | README implica órdenes discretas, pero LadderBuilder construye quotes |
| Definición clara de NO quote management | ❌ | ARCHITECTURE_MAP menciona "LadderSkew" + "16-order pool" |
| Si hay re-quoting, documentar como scope real | ⚠️ | No hay re-quoting, pero la ambigüedad persiste |
| Revisión por AUD-SEC | ❌ | No se ha revisado por confusión de terminología |

## Recomendación para IMP

1. **ADR**: `adr/0001-scope-market-making.md` aclarando:
   - El bot hace **discrete execution** (tamaño + precio único por tick)
   - NO hace market-making continuo (cancel/replace a alta frecuencia)
   - LadderBuilder construye un quote pair basado en convicción, no un book completo
   - "Ladder" = posición de quoting única, no depth continuo
2. **Actualizar docs/ARCHITECTURE.md** para reflejar esto.
3. **Reconciliar "LadderSkew" vs "LadderBuilder"**: es el mismo concepto, naming inconsistente.

## Artefactos

- `agents/handoffs/obs-05-ambiguedad-market-making/artifacts/scope_analysis.md`
- `adr/0001-scope-market-making.md` (para crear)
- `reports/arch/obs-05-scope-ambiguity.md`

## Signoff