# AGENTE_API — Fase 1: Rate Limiting y Cliente HTTP

## Rol
Implementar RateLimiter por endpoint, backoff exponencial con jitter, connection pooling, y parseo real de respuestas HTTP.

## Branch
```
remediation/10-api-ratelimit
```

## Dependencia
Fase 0 completada (baseline registrada).

## Tareas

### T1-1: RateLimiter por endpoint (token bucket)
- **Archivo**: `core/include/rate_limiter.hpp` (nuevo)
- **Criterio**: Test unitario: >X req/s bloquea en el cliente antes de salir a red
- **Especificación**:
  ```cpp
  class RateLimiter {
  public:
      RateLimiter(double rate_per_sec, double burst);
      bool try_acquire();                              // O(1), branch-predicted, thread-safe (atomic)
      std::chrono::microseconds next_available() const;
  private:
      std::atomic<double> tokens_;
      const double rate_per_sec_;
      const double burst_;
      std::atomic<uint64_t> last_refill_ns_;
  };
  ```
- Configurable por env: `CLOB_RATE_LIMIT_PER_SEC` (default: 1.0), `CLOB_BURST` (default: 2.0)

### T1-2: Backoff exponencial con jitter
- **Archivo**: `core/src/lightweight_client.hpp` (extender)
- **Criterio**: Test: secuencia de 429 → retardos crecientes; no hay retry ciego
- **Especificación**:
  - Retry solo en 429 y 5xx (503, 502, 504)
  - Backoff: `delay = base × 2^attempt + jitter` (jitter uniforme 0..1)
  - Leer `Retry-After` header si existe, usarlo como delay mínimo
  - Max retries: 5 (configurable via `CLOB_MAX_RETRIES`)
  - No retry en 4xx (excepto 429)

### T1-3: Connection pooling / keep-alive
- **Archivo**: `core/src/lightweight_client.hpp`
- **Criterio**: Una sola conexión TLS reutilizada; benchmark sin overhead de handshake
- **Acción**:
  - Inicializar el handle `CURL*` persistente una sola vez en el constructor (`curl_easy_init()`) y liberarlo en el destructor; aplicar `CURLOPT_CONNECTTIMEOUT` junto con el resto de opciones del handle
  - Usar `CURL* easy` persistente como miembro de la clase
  - `CURLOPT_FRESH_CONNECT = 0`, `CURLOPT_FORBID_REUSE = 0`
  - `Connection: keep-alive` (default de libcurl)

### T1-4: Parseo real de respuesta HTTP
- **Archivo**: `core/src/lightweight_client.hpp`
- **Criterio**: 429 no se trata como éxito; order_id se captura
- **Especificación**:
  ```cpp
  struct HttpResponse {
      HttpStatus status;
      std::string body;
      std::optional<std::string> retry_after;
      std::optional<std::string> order_id;
  };

  std::optional<HttpResponse> submit_order(const SignedOrder& order);
  ```
  - Parsear JSON body para extraer `order_id` (usar parser minimal o regex)
  - 429 no es éxito → retry con backoff
  - 4xx (excepto 429) → no retry, reportar error

### T1-5: Eliminar claim de 10,000 órdenes/s
- **Archivo**: `README.md`, `docs/WORKFLOW_MAESTRO_POLYMARKET.md` (si existe)
- **Criterio**: Sin cifras de throughput que incentiven el abuso
- **Acción**: Reemplazar "10,000 órdenes/s" con "sujeto a rate limits publicados de Polymarket CLOB"

## Restricciones
- NO tocar `eip712_signer.hpp` (firma criptográfica)
- Rate limiter debe ser O(1) y thread-safe
- Hot path: el rate check debe ser atómico y barato (usar en `ExecutionEngine::run_tick` antes de `submit_order`)

## Deliverables
1. `core/include/rate_limiter.hpp` — implementación + test unitario
2. `core/src/lightweight_client.hpp` — rate limiter integrado, backoff, pooling, parseo HTTP
3. `tests/test_rate_limiter.cpp` — test unitario
4. `core/CMakeLists.txt` — registrar `rate_limiter` test en ctest
5. Tests pasan en ctest

## Código de test esperado
```cpp
// Rate limiter: 10 req/s, burst 5
RateLimiter rl(10.0, 5.0);
for (int i = 0; i < 5; i++) assert(rl.try_acquire());  // 5 burst OK
assert(!rl.try_acquire());  // 6th in same instant → bloqueado
auto wait_time = rl.next_available();
// Esperar y verificar que after wait_time, try_acquire() devuelve true
```
