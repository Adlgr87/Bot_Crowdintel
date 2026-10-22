# AUD-NET Report — OBS-07: Feed Binance simbólico, WS real no probado

**Date:** 2026-01-22 | **Auditor:** AUD-NET | **Commit:** c157c23

## Resumen

El Binance WebSocket Client tiene la infraestructura para conectar a `wss://stream.binance.com:9443`, pero **todas las pruebas CI usan el build offline/synthetic**. No hay evidence de 24h de testing con feed real, ni tests de reconexión, gaps, ni latencia de mensajes.

## Evidencia

### Build configuration

```cmake
# core/CMakeLists.txt líneas 136-141:
if(HAVE_NETWORK)
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_HAVE_NETWORK=1)
else()
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_FORCE_MOCK=1)
endif()
```

### CI uses offline build

```bash
$ grep -r "HAVE_NETWORK\|CROWDINTEL_NETWORK" .github/ infra/
# Resultado: No CI config usa network builds
```

### BinanceWSClient — network guard

```cpp
// binance_ws_client.hpp líneas 42-53:
#if defined(CROWDINTEL_HAVE_NETWORK)
#include <openssl/ssl.h>
#include <sys/socket.h>
...
#else
// Synthetic feed
#endif
```

### Tests

```bash
$ grep -r "stream.binance.com\|binance.*real\|24.*hour" tests/
# Resultado: 0 matches
```

### SpikeDetector

SpikeDetector existe en `binance_spike_detector.hpp` pero los tests usan datos mock, no spikes reales.

### Reconnection logic

```cpp
// binance_ws_client.hpp líneas 67-71:
static constexpr uint32_t MAX_RECONNECT_ATTEMPTS = 10;
static constexpr uint64_t RECONNECT_BASE_DELAY_NS = 1'000'000'000ULL;
static constexpr uint64_t RECONNECT_MAX_DELAY_NS = 30'000'000'000ULL;
```

No hay tests de reconexión. The SECURITY_AUDIT.md findings (MEDIUM-3) sobre WebSocket fixed backoff ya están documentados.

## Conclusión

| Criterio | Cumplido | Evidencia |
|----------|---------|-----------|
| 24h real feed (paper) | ❌ | No CI network tests |
| Reconexiones | ⚠️ | Código existe, no testeado |
| Gaps < 1% | ❌ | No tests |
| Latencia p99 | ❌ | No con feed real |
| L2 integrity | ⚠️ | Parsed pero no validado vs real |
| SpikeDetector real | ❌ | Solo mock |

## Recomendación

1. Test de reconexión: simular drop, verificar reconnect
2. Test de gaps: mensajes fuera de orden
3. Test de L2 integrity: book reconstruction
4. **24h paper trading test**: manual, required before canary
