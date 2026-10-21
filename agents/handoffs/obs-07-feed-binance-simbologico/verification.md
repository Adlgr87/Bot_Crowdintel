# VERIFICATION — OBS-07/VER-05: Feed Binance simbólico, WS real no probado

**Verificador:** AUD-NET
**Fecha:** 2026-01-22
**Commit base:** c157c23
**Toolchain:** GCC 16.2

---

## Verdict: VERIFIED_FAIL — No evidence of real Binance WS feed testing

### Finding

The observation **CONFIRMS** that the Binance WebSocket feed is only tested with a synthetic/mock feed. There is no evidence in the codebase or CI of real `wss://stream.binance.com:9443` connectivity testing for ≥24h.

### Evidence

**Archivo:** `core/src/binance_ws_client.hpp`, líneas 42-53:

```cpp
#if defined(CROWDINTEL_HAVE_NETWORK)
#include <openssl/ssl.h>
#include <openssl/crypto.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
...
#endif
```

**Network guard pattern:**

The entire Binance WS client is guarded by `CROWDINTEL_HAVE_NETWORK`. When not defined:
- Líneas 18-20: "When CROWDINTEL_HAVE_NETWORK is not defined (offline/mock build), start() runs a deterministic synthetic feed"
- **No code path for real Binance feed testing** in the test suite

**Grep por tests de Binance WS real:**

```
$ grep -r "stream.binance.com" tests/
# Resultado: 0 matches en tests/

$ grep -r "binance.*real\|binance.*paper\|24.*hour\|uptime.*99" tests/ docs/
# Resultado: 0 matches
```

**SpikeDetector:**
- `binance_spike_detector.hpp` existe y está implementado
- Pero NO hay tests que lo activen con spikes reales
- `test_accept_shield_lifecycle.cpp` menciona SpikeDetector pero usa mock data

### Configuración

**Archivo:** `core/src/market_config.hpp`:
```cpp
char kline_interval[16] = "@kline_5m";   // Config default
double binance_weight = 0.45;             // Weight in feed fusion
double pyth_weight = 0.55;
```

**Archivo:** `docs/CANARY_DEPLOYMENT.md` línea 71:
```bash
export CROWDINTEL_BINANCE_FEED=wss://stream.binance.com:9443
```
→ Se menciona en docs pero **no hay tests o CI** que validen esta conexión real.

### Conexión real no probada

**BinanceWSClient** tiene lógica de reconexión (líneas 67-71):
```cpp
static constexpr uint32_t MAX_RECONNECT_ATTEMPTS = 10;
static constexpr uint64_t RECONNECT_BASE_DELAY_NS = 1'000'000'000ULL;  // 1s
static constexpr uint64_t RECONNECT_MAX_DELAY_NS = 30'000'000'000ULL;  // 30s
```

Pero:
1. **No hay tests de reconexión**
2. **No hay tests de gaps** (secuencia de mensajes fuera de orden)
3. **No hay tests de latencia p99** con feed real
4. **No hay test de integridad L2** (book reconstruction from Binance)

### Conclusión

| Criterio | ¿Cumplido? | Evidencia |
|----------|-----------|-----------|
| 24h real feed testing (paper) | ❌ | No tests use real Binance WS |
| Reconexiones | ⚠️ | Código existe, pero no testado con feed real |
| Gaps < 1% | ❌ | No tests de gap detection |
| Latencia p99 | ❌ | No se mide con feed real |
| L2 integrity | ⚠️ | Parseado pero no validado contra feed real |
| SpikeDetector con spikes reales | ❌ | Solo mock |

## Recomendación para IMP

1. **Test de reconexión**: Simular drop de feed, verificar reconexión sin pérdida de estado
2. **Test de gaps**: Enviar mensajes fuera de orden/seq, verificar detección
3. **Test de latencia**: Medir parse + SPSC push con feed real (modo paper)
4. **Test de L2 integrity**: Reconstruir book desde Binance depth stream, comparar con snapshot REST

> **Nota:** No se puede ejecutar un test de 24h en CI. El criterio de 24h uptime debe validarse manualmente en paper trading antes de canary.

## Artefactos

- `agents/handoffs/obs-07-feed-binance-simbologico/artifacts/feed_test_analysis.md`
- `reports/net/obs-07-binance-feed.md`

## Signoff