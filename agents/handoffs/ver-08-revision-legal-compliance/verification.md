# VERIFICATION — VER-08: Revisión legal/compliance

**Verificador:** AUD-SEC
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** GCC 16.2, secp256k1 @ pin

---

## Veredicto: VERIFIED_FAIL — ComplianceGuard documentado pero NO implementado

### Hallazgo

La observación **CONFIRMA** que ComplianceGuard está **documentado en README.md (línea 100) y ARCHITECTURE_MAP.md (línea 20)** como paso 1 del hot path, pero **NO EXISTE como clase ni función en el código**.

### Evidencia de código

**Grep por "Compliance" o "compliance" en core/src/:**

```
$ grep -r "Compliance\|compliance" core/src/
# Resultado: 0 matches
```

**README.md línea 100:**
```
1. ComplianceGuard — jurisdiction + restricted tokens (fail-closed)
```

**ARCHITECTURE_MAP.md línea 20:**
```
├─ 1. ComplianceGuard (jurisdiction + restricted tokens, fail-closed)
```

**execution_engine.hpp — hot path (`run_tick()` y `housekeeping()`):**

El hot path comienza con `housekeeping()` (línea 178) que drena eventos de usuario y ejecuta risk actions. **NO hay compliance check antes de las órdenes.**

```cpp
TickResult run_tick() {
    const TickResult housework = housekeeping();  // ← Step 0: NO ComplianceGuard
    if (housework == TickResult::RISK_KILL_SWITCH || ...)
        return housework;
    // Step 1: VolatilityGate observe (adverse selection)
    // Step 2: Sizing
    // Step 3: RiskManager authorize
    // Step 4: Sign + submit
}
```

### ¿Qué debería hacer ComplianceGuard (según docs)?

| Función | Implementación | Estado |
|---------|----------------|--------|
| jurisdiction check | ❌ No existe | FAIL |
| restricted tokens | ❌ No existe lista | FAIL |
| fail-closed | ❌ No existe código | FAIL |
| ERC-7739 handling | ⚠️ Solo rechaza type 3 (market_config.hpp:295) | PARCIAL |

### ERC-7739 (tipo 3)

**Archivo:** `core/src/market_config.hpp`, línea 295:
```cpp
return "POLY_1271 requires ERC-7739 wrapping; this build fails closed. Use type 0/1/2 or the official V2 signer sidecar";
```

✅ Type 3 falla cerrado (fail-closed) está implementado.
❌ Pero no hay el wrapper ERC-7739 completo, solo un error.

### Configuración de compliance

**Archivo:** `core/src/market_config.hpp`

```cpp
// NO existe BOT_JURISDICTION, BOT_RESTRICTED_TOKENS, BOT_COMPLIANCE_MODE
// Solo:
char alpha_bearer_token[128]{};  // auth del alpha HTTP receiver
bool live_armed = false;
char kill_switch_file[192] = "/tmp/crowdintel.kill";
```

### Alpha HTTP receiver (loopback sin TLS)

```cpp
char alpha_bind[64] = "127.0.0.1";  // ← Loopback sin TLS
uint16_t alpha_port = 8088;
```

El alpha HTTP receiver escucha en `127.0.0.1` sin TLS. Si el alpha producer está en la misma máquina, los Bearer tokens viajan en texto plano dentro del host. Si hay un proxy externo, depende de la configuración del proxy.

### Legal scope

- **No hay ADR de compliance** — `adr/` está vacío.
- **No hay checklist de compliance firmada.**
- **No hay tests de fail-closed para compliance.**

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| Jurisdiction guard fail-closed | ❌ | No existe ComplianceGuard |
| Restricted tokens list + fail-closed | ❌ | No existe lista de tokens restringidos |
| Fail-closed tests | ❌ | No existen tests de compliance |
| ADR compliance | ❌ | No existe adr/0002-compliance.md |
| ERC-7739 | ⚠️ | Type 3 falla cerrado, pero no implementado |

## Recomendación para IMP

1. **ComplianceGuard clase**: Implementar `compliance_guard.hpp` con:
   - Jurisdiction whitelist (config: `BOT_JURISDICTION`)
   - Restricted tokens list (config: `BOT_RESTRICTED_TOKENS`)
   - Fail-closed por defecto
   - Tests fail-closed
2. **ADR**: `adr/0002-compliance.md` documentando:
   - Alcance legal
   - Jurisdicciones soportadas/prohibidas
   - Token restriction list (mantenida actualizada)
   - Operator identity y licensing
3. **TLS para alpha receiver**: Si hay proxy, mTLS; si no, documentar limitation.
4. **Canary checklist H1-H15**: Vincular a compliance items.

## Artefactos

- `agents/handoffs/ver-08-revision-legal-compliance/artifacts/compliance_gap.md`
- `reports/sec/ver-08-compliance.md`

## Signoff