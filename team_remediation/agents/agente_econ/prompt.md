# AGENTE_ECON — Fase 4: Modelo de Comisiones, Gas y Slippage

## Rol
Implementar FeeModel con comisión maker/taker base + fórmula dinámica configurable, estimación de gas y slippage, y reemplazar el filtro de edge por net-EV.

## Branch
```
remediation/40-fee-model
```

## Dependencia
Fase 0 completada.

## Tareas

### T4-1: FeeModel
- **Archivo**: `core/include/fee_model.hpp` (nuevo)
- **Criterio**: Test con vectores conocidos: fee máximo cerca de p=0.5, mínimo en extremos
- **Especificación**:
  ```cpp
  struct FeeConfig {
      double maker_fee_rate      = 0.020;  // 2.0% (configurable: FEE_MAKER_RATE)
      double taker_fee_rate      = 0.035;  // 3.5% (configurable: FEE_TAKER_RATE)
      double base_commission_usd = 0.10;   // comisión base por operación (FEE_BASE_USD)
      bool dynamic_enabled       = false;  // FEE_DYNAMIC_ENABLED
      double dynamic_C           = 0.075;  // C en fórmula (FEE_DYNAMIC_C)
  };

  class FeeModel {
  public:
      double compute_fee(double notional_usd, bool is_maker, double probability) const;
      double compute_dynamic_fee(double notional_usd, double probability) const;
      // fee = C × 0.25 × (p·(1−p))²  — mantiene p=0.5, mínimo en p=0 o p=1
  };
  ```
- La fórmula dinámica `fee = C × 0.25 × (p·(1−p))²` debe ser configurable por flag de mercado, no asumir global.

### T4-2: Estimación de gas y slippage
- **Archivo**: `core/include/fee_model.hpp`
- **Criterio**: El coste sube con el tamaño y con el spread
- **Especificación**:
  ```cpp
  struct GasEstimate {
      double gas_cost_usd;       // Estimado en USD
      double slippage_usd;       // Slippage esperado en USD
  };

  GasEstimate estimate_gas_and_slippage(
      double order_size_usd,
      double spread_bps,
      const Level2Entry& best_bid,
      const Level2Entry& best_ask
  );
  ```
- Gas base de Polygon: ~0.005 USD por transacción (configurable: `GAS_BASE_COST_USD`)
- Slippage: función del tamaño relativo al depth del L2

### T4-3: Reemplazar filtro edge > min_edge por net_ev
- **Archivo**: `core/src/execution_engine.cpp`
- **Criterio**: Orden con edge positivo pero net_ev negativo → TickResult::NOT_PROFITABLE
- **Especificación**:
  ```cpp
  // ANTES:
  if (edge > min_edge) { submit(); }
  // DESPUÉS:
  double net_ev = edge_usd - fees - slippage - gas_cost;
  if (net_ev > min_net_ev) { submit(); } else { return TickResult::NOT_PROFITABLE; }
  ```

### T4-4: Documentar comisiones dinámicas
- **Archivo**: `docs/`, `README.md`
- **Criterio**: Comentario en código + nota en README
- **Acción**: "Las comisiones dinámicas deben verificarse contra la doc oficial vigente antes de producción"

## Restricciones
- NO tocar la estructura de orden V2 (OrderParams, eip712_signer.hpp)
- La fórmula debe ser configurable (verificar en docs oficiales antes de hardcodear)
- Gas/slippage estimation debe ser un cálculo estimado, no un hardcode de constantes mágicas

## Deliverables
1. `core/include/fee_model.hpp` — FeeModel completo con fórmula dinámica
2. `core/src/execution_engine.cpp` — Reemplazar filtro de edge por net_ev
3. `tests/test_fee_model.cpp` — Tests unitarios con vectores conocidos
4. `docs/FEE_MODEL.md` — Documentación de la fórmula y referencias
5. ctest verde con nuevos tests
