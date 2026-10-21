# Binance Feed Test Analysis — OBS-07

## File: core/src/binance_ws_client.hpp

### Network guard pattern

```cpp
#if defined(CROWDINDEL_HAVE_NETWORK)
// Real network code with OpenSSL, sockets, etc.
// Lines 42-53: TLS/SSL includes, socket includes
// Lines 64-92: Real BinanceWSClient class with connection management
#else
// Synthetic feed (lines 920+): deterministic mock data
// "When CROWDINTEL_HAVE_NETWORK is not defined (offline/mock build),
//  start() runs a deterministic synthetic feed"
#endif
```

### Build configuration

**core/CMakeLists.txt lines 136-141:**
```cmake
if(HAVE_NETWORK)
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(crowdintel_bot PRIVATE CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
else()
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_FORCE_MOCK=1)
endif()
```

### CI configuration

**Grep por CROWDINTEL_HAVE_NETWORK en CI:**
```
$ grep -r "HAVE_NETWORK\|CROWDINTEL_NETWORK" .github/ infra/
# Resultado: No CI config uses network builds
```

CI uses the **offline/mock build** (`-DCROWDINTEL_NETWORK=OFF`), meaning:
- ✅ All tests compile and run
- ❌ No tests connect to real `wss://stream.binance.com:9443`
- ❌ No reconnect tests with real network failures
- ❌ No gap/latency tests with real feed

### SpikeDetector testing

**File: core/src/binance_spike_detector.hpp**

```cpp
// Exist test: tests/acceptance/test_accept_shield_lifecycle.cpp
// But: uses mock spikes, not real Binance spikes
```

### BinanceWSClient reconnect logic

```cpp
// Lines 67-71:
static constexpr uint32_t MAX_RECONNECT_ATTEMPTS = 10;
static constexpr uint64_t RECONNECT_BASE_DELAY_NS = 1'000'000'000ULL;  // 1s
static constexpr uint64_t RECONNECT_MAX_DELAY_NS = 30'000'000'000ULL;  // 30s
```

**No tests for:** reconnect logic, gap detection, message ordering, book reconstruction integrity.

### Multi-feed manager

**File: core/src/multi_feed_manager.hpp**

```cpp
// Has reconcile_circuit() method (line 168)
// But: no integration tests for multi-feed behavior
```

## Conclusion

The Binance feed has real implementation but **zero real-feed tests**. All CI uses the synthetic/offline build. The 24h uptime requirement cannot be verified through automated testing — it requires manual paper trading.
