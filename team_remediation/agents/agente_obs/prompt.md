# AGENTE_OBS — Fase 6: Observabilidad, Auditoría y Alertas

## Rol
Implementar logger asíncrono append-only, alertas configurables, y exposición de métricas.

## Branch
```
remediation/60-telemetry
```

## Dependencia
Fases 2-3 completadas.

## Tareas

### T6-1: Logger asíncrono append-only
- **Archivo**: `core/src/telemetry.hpp` (nuevo)
- **Criterio**: Fichero audit.log con JSON lines; no se puede perder evento en hot path
- **Especificación**:
  ```cpp
  enum class EventType {
      ORDER_SUBMITTED, ORDER_FILLED, ORDER_CANCELLED, ORDER_REJECTED,
      RISK_BLOCKED, KILL_SWITCH_ACTIVATED, BALANCE_CHECK,
      RECONCILIATION_DIFF, FEED_DEAD, LATENCY_SPIKE
  };

  struct TelemetryEvent {
      EventType type;
      uint64_t timestamp_ns;
      std::string market_slug;
      std::string details_json;  // JSON compacto con detalles
      std::string severity;       // INFO, WARN, ERROR, CRITICAL
  };

  class Telemetry {
  public:
      void log(EventType type, const std::string& market_slug,
               const std::string& details, const std::string& severity = "INFO");
      // Hot path: encola en SPSC, nunca bloquea
  private:
      // SPSC ring buffer a thread de escritura
      // Thread de escritura hace flush a filesystem con O_APPEND
      std::thread writer_thread_;
      std::atomic<bool> running_{true};
      SPSC_RingBuffer<TelemetryEvent, 8192> queue_;
  };
  ```
- No secrets en logs (nunca printer claves privadas, API keys, signatures)

### T6-2: Alertas configurables
- **Archivo**: `core/src/telemetry.hpp`
- **Criterio**: Test: umbral cruzado → evento de alerta registrado
- **Alertas**:
  1. Pérdida diaria > `ALERT_DAILY_LOSS_THRESHOLD_USD` (default: 500)
  2. Racha de rechazos 429 > `ALERT_CONSECUTIVE_429_THRESHOLD` (default: 5)
  3. Latencia P99 > `ALERT_LATENCY_SPIKE_US` (default: 100000)
  4. Divergencia de posición > `ALERT_POSITION_DIVERGENCE` (default: 0.05)
  5. Feed muerto > `ALERT_FEED_DEAD_MS` (default: 5000)
- Configurables via env; webhook URL configurable (`ALERT_WEBHOOK_URL`)

### T6-3: Métricas expuestas
- **Archivo**: `core/src/telemetry.hpp`
- **Criterio**: Endpoint o log periódico de métricas
- **Métricas**:
  - Contador por TickResult (OK, RISK_BLOCKED, KILL_SWITCH, etc.)
  - Total fills, total cancels
  - PnL diario (realized + unrealized)
  - Latencia hot path P50/P99 (desde latency_bench)
  - Balance actual USDC/POL
- Output: JSON periódico a stdout/log cada N segundos (configurable: `METRICS_INTERVAL_SECONDS`)

## Restricciones
- Lock-free en hot path: solo encola eventos, nunca hace I/O directo
- Thread-safe: SPSC + writer thread
- Nunca loggear secretos: usar sanitized logging (truncar, ofuscar)

## Deliverables
1. `core/src/telemetry.hpp` — Telemetry completo con async writer + alertas + métricas
2. `tests/test_telemetry.cpp` — Tests unitarios
3. ctest verde con nuevos tests
