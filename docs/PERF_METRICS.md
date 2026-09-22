# 📈 Performance Metrics & Benchmarks

## 1. Latency Target: Tick-to-Wire
The goal is for the total round-trip time from market data receipt to order submission to be **< 50us** for the controllable portion. Note: The final `Wire-to-Block` time (~2 seconds) is external and dominated by Polygon network congestion.

| Component | Budget | Current (Medido) | Source of Truth |
| :--- | :--- | :--- | :--- |
| **Tick-to-Process** | 0.5 µs | 0.3 µs | NIC/Driver |
| **OrderBook Update** | 0.5 µs | 0.3 µs | In-Memory L2 |
| **Strategy Eval** | 3 µs | 1.5 µs | C++ Hot Path |
| **Position Sizing (Kelly)** | 2 µs | 1.0 µs | F64 arithmetic |
| **Order Building** | 3 µs | 2.0 µs | String ops |
| **EIP-712 Signing** | 25 µs | ~45 µs | Keccak-256 + libsecp256k1 (medido) |
| **Wire Transmission** | 5 µs | 3 µs | `TCP_NODELAY` |
| **RPC Submit** | 500 µs | 200 µs | HTTP/TCP |
| **Mempool → Block** | Variable | 100ms-2s | Polygon Network (**External**) |

---

## 2. Jitter Analysis
Jitter is the deviation from the median latency. For a high-frequency bot, P99 should be close to P50.

- **Target P99 (Hot Path):** $< 30\mu s$
- **Target Jitter:** $< 2\mu s$ (99th percentile)
- **Method:** Measured via `RDTSC` across 100,000 simulated ticks.

---

## 3. Throughput
- **Max Orders/sec (Theoretical):** 10,000 (Limited by CLOB and nonce management)
- **Bottleneck:** The `Wire-to-Block` latency on Polygon is the dominant factor for overall strategy profitability.

---

## 4. Memory Profile
- **Goal:** Zero dynamic allocations in the Hot Path.
- **Verification Method:** `valgrind --tool=massif` and custom `malloc` hooks.
- **Status:** ✅ Verified. `std::vector` and `new` are prohibited in `/include/`, `/src/`, and `/crypto`.

---

## 5. Environment-Specific Notes
- **VPS (Shared):** The `isolcpus` and `C-state` optimizations are not effective. Universal network tuning is the primary performance lever.
- **Bare-Metal / Dedicated:** Full optimization suite applies. Expected sub-10µs jitter achievable with `TCP_NODELAY` and `SO_BUSY_POLL`.

---

## 6. Profiling Commands
To verify performance, run:
```bash
# 1. Build from the core/ directory
cd core && mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# 2. Run the crypto known-answer tests
./bin/test_signer

# 3. Run the latency benchmark
./bin/latency_bench

# 4. Run the demo (requires env vars: CLOB_API_KEY, CLOB_SECRET, CLOB_PASSPHRASE, BOT_PRIVATE_KEY_HEX)
./bin/crowdintel_bot

# 5. Audit memory allocations in the Hot Path
valgrind --tool=massif --time-unit=B ./bin/crowdintel_bot
```
