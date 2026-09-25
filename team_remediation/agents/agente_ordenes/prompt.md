# AGENTE_ORDENES — Fase 3: Order Manager, Reconciliación y Positions

## Rol
Implementar order manager con client_order_id tracking, position tracker, user-channel fills feed, anti-retry, y self-trade detection.

## Branch
```
remediation/30-order-manager
```

## Dependencia
Fase 1 completada (necesita `order_id` real del cliente HTTP).

## Tareas

### T3-1: Registro de client_order_id y estados
- **Archivo**: `core/include/order_manager.hpp` (nuevo)
- **Criterio**: Toda orden enviada queda registrada con id y estado inicial
- **Especificación**:
  ```cpp
  enum class OrderStatus {
      PENDING, OPEN, PARTIAL, FILLED, CANCELLED, REJECTED, EXPIRED
  };

  struct ManagedOrder {
      std::string client_order_id;    // salt + timestamp hex
      uint64_t nonce;
      OrderParams params;
      OrderStatus status;
      uint64_t fills_quantity;        // en units * 1e6
      uint64_t fills_cash_value;      // en USD * 1e6
      std::chrono::steady_clock::time_point created_at;
      std::chrono::steady_clock::time_point last_update;
  };

  class OrderManager {
  public:
      std::string generate_client_order_id(uint64_t salt);
      void register_order(const OrderParams& params, uint64_t nonce);
      void update_status(const std::string& id, OrderStatus status);
      ManagedOrder* find_order(const std::string& id);
      std::vector<ManagedOrder*> get_open_orders();
      void cancel_order(const std::string& id);
  private:
      std::unordered_map<std::string, ManagedOrder> orders_;
      std::shared_mutex mutex_;
  };
  ```

### T3-2: User-channel fills feed
- **Archivo**: `core/src/ws_market_listener.hpp` (extender), `core/include/position_tracker.hpp` (nuevo)
- **Criterio**: Test con mensajes de fill mock: posición interna se actualiza y coincide
- **Especificación**:
  ```cpp
  struct FillEvent {
      std::string order_id;
      std::string market_slug;
      bool is_buy;
      uint64_t price;       // * 1e6
      uint64_t size;        // * 1e6
      std::chrono::steady_clock::time_point timestamp;
  };

  class PositionTracker {
  public:
      void apply_fill(const FillEvent& fill);
      double get_net_position(const std::string& market_slug) const;
      double get_realized_pnl(const std::string& market_slug) const;
      double get_unrealized_pnl(const std::string& market_slug, double mark_price) const;
      bool reconcile(double api_position, const std::string& market_slug, double tolerance);
  private:
      struct Position {
          double quantity = 0.0;
          double cash = 0.0;
          double realized_pnl = 0.0;
      };
      std::unordered_map<std::string, Position> positions_;
      std::shared_mutex mutex_;
  };
  ```

### T3-3: Reconciliación periódica (cold path)
- **Archivo**: `position_tracker.hpp`
- **Criterio**: Divergencia inyectada → kill switch o alerta
- **Acción**: Comparar posición interna vs API, si divergen > max_position_divergence → trigger kill switch

### T3-4: Anti-reintento ciego
- **Archivo**: `order_manager.hpp`, `execution_engine.cpp`
- **Criterio**: Timeout → se consulta estado; si ya está filled, no se reenvía
- **Especificación**:
  ```cpp
  // En OrderManager:
  bool should_retry(const std::string& client_order_id);  // Consulta estado antes de reenviar
  // En LightweightCLOBClient:
  OrderStatus query_order_status(const std::string& client_order_id);
  ```

### T3-5: Cancelación de órdenes obsoletas
- **Archivo**: `core/src/presigned_pool.hpp` (nuevo si no existe), `order_manager.hpp`
- **Criterio**: Salto de precio → pool marcado inválido, órdenes pendientes canceladas
- **Acción**: Si el mejor bid/ask se mueve más de `max_price_deviation_bps` desde que se creó la orden, marcar como obsoleta

### T3-6: Detección de self-trade
- **Archivo**: `order_manager.hpp`
- **Criterio**: Orden propia en el book → orden entrante bloqueada o cancelada primero
- **Especificación**:
  ```cpp
  bool check_self_trade(const OrderParams& params);  // Verifica contra órdenes propias abiertas
  ```

## Restricciones
- NO tocar la fórmula de Kelly
- El order lifecycle tracking NO debe añadir latencia al hot path (usar cold path para RPC queries)
- client_order_id debe ser único (salt + timestamp + counter)

## Deliverables
1. `core/include/order_manager.hpp` — OrderManager completo
2. `core/include/position_tracker.hpp` — PositionTracker con reconciliación
3. `core/src/ws_market_listener.hpp` — Extendido con user-channel fills
4. `core/src/presigned_pool.hpp` — Pool de órdenes con invalidación
5. `core/src/execution_engine.cpp` — Integrado con OrderManager
6. `tests/test_order_manager.cpp` — Tests unitarios
7. `tests/test_position_tracker.cpp` — Tests unitarios
8. ctest verde con nuevos tests

## Integration Point
```cpp
// En ExecutionEngine:
std::string order_id = order_mgr_.generate_client_order_id(params.salt);
order_mgr_.register_order(params, params.nonce);

// Antes de submitir, anti-retry:
if (order_mgr_.should_retry(order_id)) {
    submit_and_track(order_id);
}
```
