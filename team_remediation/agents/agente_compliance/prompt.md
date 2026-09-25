# Compliance — Fase 5: Compliance Guard y Metadata de Mercado

## Rol
Implementar fetch de metadata de mercado (tick size, estado, resolución), compliance guard con listas restringidas y verificación de jurisdicción.

## Branch
```
remediation/50-compliance-guard
```

## Dependencia
Fase 0 completada.

## Tareas

### T5-1: Fetch de metadata en arranque (cold path)
- **Archivo**: `core/include/market_metadata.hpp` (nuevo)
- **Criterio**: Test mock: tick size se aplica; mercado cerrado → pre_trade_check bloquea
- **Especificación**:
  ```cpp
  struct MarketMetadata {
      std::string token_id;
      std::string question;
      std::string outcome_token;  // "Yes"/"No"
      int tick_size;              // en micro-unidades (e.g., 100 = 0.0001)
      std::string status;          // "active", "closed", "resolved"
      std::chrono::system_clock::time_point resolution_time;
      std::optional<std::string> resolution_src;  // fuente de resolución
      bool enable_dynamic_fees;    // flag para comisión dinámica (T4)
  };

  class MarketMetadataCache {
  public:
      void fetch_metadata(const std::string& token_id);  // cold path RPC
      const MarketMetadata* get(const std::string& token_id) const;
      bool is_market_tradable(const std::string& token_id) const;
      uint64_t apply_tick_size(uint64_t raw_price, int tick_size) const;
  private:
      std::unordered_map<std::string, MarketMetadata> cache_;
      std::shared_mutex mutex_;
  };
  ```

### T5-2: ComplianceGuard
- **Archivo**: `core/include/compliance_guard.hpp` (nuevo)
- **Criterio**: token_id en blocklist → rechazo; flag de jurisdicción desactivado → arranque bloqueado
- **Especificación**:
  ```cpp
  struct ComplianceConfig {
      bool jurisdiction_check_enabled = true;   // COMPLIANCE_JURISDICTION_ENABLED
      std::string allowed_jurisdiction;         // COMPLIANCE_ALLOWED_JURISDICTION
      std::set<std::string> restricted_tokens;  // COMPLIANCE_RESTRICTED_TOKENS (comma-separated)
  };

  class ComplianceGuard {
  public:
      bool is_token_allowed(const std::string& token_id) const;
      bool verify_jurisdiction(const std::string& country_code) const;
      bool verify_market_active(const std::string& token_id, const MarketMetadataCache& cache) const;
      TickResult check_all(const std::string& token_id, const std::string& country_code);
  };
  ```

### T5-3: Rechazar trading si mercado no activo o resolución próxima
- **Archivo**: `compliance_guard.hpp`, `execution_engine.cpp`
- **Criterio**: Nuevo `TickResult::MARKET_NOT_TRADABLE`
- **Acción**: Si mercado no está activo, o resolución está dentro de `COMPLIANCE_RESOLUTION_WARNING_HOURS` (default: 24h), bloquear

### T5-4: (Opcional) Almacenar criterio/fuente de resolución
- **Archivo**: `market_metadata.hpp`
- **Acción**: Persistir `resolution_src` y `resolution_criteria` en frío (config file)

## Restricciones
- NO tocar el hot path loop (los checks de compliance se hacen en pre_trade_check, que debe ser O(1))
- Nunca hardcodear "permitido todo" para jurisdicción — configurado por el operador
- Jurisdicción viene del entorno/operador, no del código fuente

## Deliverables
1. `core/include/market_metadata.hpp` — MarketMetadataCache con tick size y estado
2. `core/include/compliance_guard.hpp` — ComplianceGuard con blocklist y jurisdicción
3. `core/src/execution_engine.cpp` — Integrar compliance checks
4. `tests/test_compliance_guard.cpp` — Tests unitarios
5. ctest verde con nuevos tests
