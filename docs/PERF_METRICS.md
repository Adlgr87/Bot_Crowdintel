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

## 2. Baseline Metrics (Regla: línea base antes de remediación)

> **Registros de línea base** — medidos antes de aplicar las fases de remediación de cumplimiento.
> Build: `cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)` (con libsecp256k1 v0.8.0)
> Benchmark: `latency_bench` (20K ticks, 5K warmup, RDTSC calibrado)

| Métrica | Valor | Evidencia |
| :--- | :--- | :--- |
| **CPU frequency calibrated** | 0.372 ns/cycle (~2.69 GHz) | RDTSC + clock_gettime |
| **Min latency** | 44.4 µs (119,256 cycles) | latency_bench |
| **P50 latency** | 45.6 µs (122,610 cycles) | latency_bench |
| **P99 latency** | 101.8 µs (273,542 cycles) | latency_bench |
| **Hot path P99 target** | < 30 µs | — |
| **Keccak-256 KAT** | 3/3 PASS | test_signer |
| **EIP-712 signatures** | 20/20 valid (v=27) | test_signer |
| **ctest** | 2/2 PASS (1.39s) | ctest --output-on-failure |
| **Throughput claim** | **Sujeto a rate limits publicados de Polymarket CLOB** | No hardcoded throughput targets |

> ⚠️ **Nota sobre latencia**: El P99 baseline varía (49-102 µs) debido al jitter del sistema.
> La remediación de cumplimiento (rate limiter, risk checks) añade O(1) checks al hot path.
> Regla: P50 post-remediación debe estar dentro del +10% de este baseline (≤ 50.2 µs).

---

## 3. Jitter Analysis
Jitter is the deviation from the median latency. For a high-frequency bot, P99 should be close to P50.

- **Target P99 (Hot Path):** $< 30\mu s$ (goal, not currently met)
- **Target Jitter:** $< 2\mu s$ (99th percentile)
- **Method:** Measured via `RDTSC` across 100,000 simulated ticks.

---

## 3. Throughput
- **Max Orders/sec:** Sujeto a rate limits publicados de Polymarket CLOB (no se hardcodea throughput)
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
