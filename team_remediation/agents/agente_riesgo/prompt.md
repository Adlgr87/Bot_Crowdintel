# AGENTE_RIESGO — Fase 2: Risk Engine y Kill Switch

## Rol
Implementar risk engine con pre_trade_check, kill switch atómico, límites de exposición/pérdida, y consultas de balance.

## Branch
```
remediation/20-risk-engine
```

## Dependencia
Fase 0 completada.

## Tareas

### T2-1: Parámetros de riesgo en MarketConfig (env-driven)
- **Archivo**: `core/src/market_config.hpp` (nuevo)
- **Criterio**: Cargados desde env con defaults conservadores; test de parseo
- **Especificación**:
  ```cpp
  struct RiskConfig {
      double max_daily_loss_usd       = 500.0;
      double max_exposure_per_market   = 5000.0;
      double max_exposure_per_side     = 2000.0;
      double max_order_usd             = 500.0;
      int    max_open_orders           = 5;
      int    max_orders_per_min        = 10;
      int    max_cancels_per_min       = 20;
      int    max_price_deviation_bps   = 500;
      double min_usdc_balance          = 100.0;
      double min_pol_balance           = 10.0;
      double max_position_divergence   = 0.01;
      bool   enable_jurisdiction_check = true;

      static RiskConfig load_from_env();  // Lee getenv con defaults conservadores
  };
  ```

### T2-2: RiskEngine::pre_trade_check
- **Archivo**: `core/include/risk_engine.hpp` (nuevo), `core/src/execution_engine.cpp` (modificar)
- **Criterio**: Order que excede max_order_usd → TickResult::RISK_BLOCKED
- **Especificación**:
  ```cpp
  enum class TickResult {
      OK, NO_SIGNAL, NO_EDGE, NOT_PROFITABLE,
      RISK_BLOCKED, KILL_SWITCH, MARKET_NOT_TRADABLE,
      DUPLICATE_ORDER,
  };

  class RiskEngine {
  public:
      TickResult pre_trade_check(const OrderParams& params, double usdc_balance, double pol_balance);
      void record_order(double order_usd, bool is_buy);
      void record_cancel();
      void add_loss(double usd_loss);
      void set_kill_switch(bool active) { kill_switch_.store(active, std::memory_order_release); }
      bool is_kill_switch_active() const { return kill_switch_.load(std::memory_order_acquire); }
  private:
      std::atomic<bool> kill_switch_{false};
      // ... contadores atómicos y timestamps para ventanas
  };
  ```
- Costo: O(1), branch-predicted, atómico. NO aloca en hot path.

### T2-3: Kill switch atómico
- **Archivo**: `core/include/risk_engine.hpp`, `core/src/execution_engine.cpp`
- **Criterio**: Al activar el flag, `run_tick` devuelve KILL_SWITCH y no firma nada
- **Disparadores del kill switch**:
  - Pérdida diaria supera `max_daily_loss_usd`
  - Divergencia de posición supera `max_position_divergence`
  - Feed muerto > timeout (configurable: `FEED_DEAD_TIMEOUT_MS`)
  - Racha de rechazos > 5 consecutivos (configurable: `RISK_MAX_CONSECUTIVE_REJECTS`)

### T2-4: Consulta de balance USDC/POL (cold path)
- **Archivo**: `core/src/balance_checker.hpp` (nuevo)
- **Criterio**: Test con mock RPC: balance bajo → pre_trade_check bloquea
- **Especificación**:
  ```cpp
  class BalanceChecker {
  public:
      struct Balance {
          double usdc_balance;
          double pol_balance;
          std::chrono::steady_clock::time_point last_update;
      };
      Balance get_balances();  // Consulta fría a RPC (no en hot path)
      void check_min_balance(const RiskConfig& cfg);  // Lanza alerta si bajo
  private:
      LightweightCLOBClient& client_;
      Balance cached_balance_;
      std::mutex mutex_;
  };
  ```

### T2-5: Ventanas deslizantes órdenes/cancelaciones por minuto
- **Archivo**: `core/include/risk_engine.hpp`
- **Criterio**: Más de N órdenes en 60s → bloqueo hasta resetear ventana
- **Implementación**: Anillo circular de timestamps con `memory_order_relaxed`, limpieza en frío

## Restricciones
- NO tocar el esquema de auth (HMAC)
- Todos los checks deben ser O(1) y thread-safe (átomos + memory_order correcto)
- El kill switch debe ser atómico y visible entre threads
- Defaults conservadores: max_order_usd=500, max_open_orders=5

## Deliverables
1. `core/src/market_config.hpp` — RiskConfig env-driven
2. `core/include/risk_engine.hpp` — RiskEngine completo con kill switch y ventanas
3. `core/src/balance_checker.hpp` — BalanceChecker con mock RPC
4. `core/src/execution_engine.cpp` — Integrar pre_trade_check antes de firmar
5. `tests/test_risk_engine.cpp` — Tests unitarios (positivos y negativos)
6. ctest verde con nuevos tests

## Integration Point
```cpp
// En ExecutionEngine::run_tick, ANTES de firmar:
auto risk_result = risk_engine_.pre_trade_check(params, balances.usdc, balances.pol);
if (risk_result != TickResult::OK) {
    // Log (cold path), no sign, no submit
    telemetry_.log_risk_block(risk_result, params);
    return risk_result;
}
```
