# 📈 Performance Metrics & Benchmarks

## 1. Latency Target: Tick-to-Wire
The goal is $< 50\mu s$ from the moment a packet hits the NIC to the moment the signed order leaves the NIC.

| Component | Budget | Current (Est) | Unit |
| :--- | :--- | :--- | :--- |
| Packet Parsing | 0.5 | 0.2 | $\mu s$ |
| OrderBook Update | 0.5 | 0.3 | $\mu s$ |
| Strategy Eval | 3.0 | 1.5 | $\mu s$ |
| Position Sizing | 2.0 | 1.0 | $\mu s$ |
| Order Building | 3.0 | 2.0 | $\mu s$ |
| EIP-712 Signing | 25.0 | 18.0 | $\mu s$ |
| Wire Transmission | 5.0 | 3.0 | $\mu s$ |
| **Total** | **39.0** | **25.9** | $\mu s$ |

## 2. Jitter Analysis
Jitter is the deviation from the median latency. For a high-frequency bot, P99 must be close to P50.
- **Target P99:** $< 5\mu s$ deviation.
- **Method:** Measured via `RDTSC` across 1 million ticks.

## 3. Throughput
- **Max Orders/sec:** 10,000.
- **Bottleneck:** Polymarket CLOB V2 Nonce management and RPC rate limits.

## 4. Memory Profile
- **Heap Allocations in Hot Path:** 0.
- **Stack Usage:** Fixed and pre-calculated.
