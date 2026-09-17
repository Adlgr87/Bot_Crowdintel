# ⚡ Bot CrowdIntel: Ultra-Low Latency Polymarket Trader

A high-frequency trading bot for Polymarket, structured around a deterministic,
zero-allocation **Hot Path** and a signal-driven **Cold Path**.

> ⚠️ **Status**: This is a **functional open-source prototype** demonstrating a complete
> low-latency architecture. It compiles, signs orders using OpenSSL's ECDSA, and has been
> optimized via genetic algorithms (MutaLambda). For real money deployment, the crypto
> library (currently using a Keccak-256 stub) and network client (libcurl HTTPS) must be
> fully integrated and security-audited.

---

## 🗂️ Project Structure

\`\`\`
core/                  # HOT PATH (C++20) - Zero Alloc, Lock-Free
  ├── include/         # OrderBookL2, SPSC_RingBuffer
  ├── crypto/          # EIP712Signer (OpenSSL/ECDSA)
  └── src/             # ExecutionEngine, LightweightCLOBClient, WsMarketListener
alpha/                 # COLD PATH - Alpha Signals & Risk
  ├── crowdintel/      # AlphaParser (FDR q-value filtering)
  └── strategy/        # KellyEngine, MarketMakingEngine
infra/                 # Infrastructure
  ├── scripts/         # kernel_tuning.sh, deploy_production.sh
  ├── docker/          # Dockerfile.prod (Deterministic LTO/PGO build)
  └── mutualambda/     # MutaLambda optimizer adapter
tests/benchmarks/      # Latency measurement (RDTSC)
docs/                  # Architecture, Perf Metrics, Optimization Lineage
.github/workflows/     # CI/CD pipeline
\`\`\`

---

## 🏁 Quick Start

### Prerequisites
- Ubuntu 22.04+, Clang 15+, CMake 3.16+, libcurl, OpenSSL 3.x.
- For production: Bare-metal server (Amsterdam) with \`isolcpus\` and \`PREEMPT_RT\`.

### Build & Run
\`\`\`bash
# Native build (Fastest iteration)
cd core && mkdir build && cd build
cmake .. && make -j\$(nproc)
./crowdintel_bot

# Deterministic build (Production)
docker build -f infra/docker/Dockerfile.prod -t crowdintel .
docker run --cpuset-cpus="2,3" --privileged crowdintel
\`\`\`

---

## 🧬 Optimization: MutaLambda Evolved

This bot's Hot Path was genetically evolved using **[MutaLambda](https://github.com/Adlgr87/MutaLambda)**,
an open-source framework for AI-driven low-level code optimization. Key mutations applied:

- **Memory Ordering**: \`memory_order_acquire\` → \`memory_order_relaxed\` in SPSC queue.
- **Stack Allocation**: \`std::vector\` → \`std::array\` in EIP-712 struct hashing.
- **Compiler Pragmas**: \`#pragma GCC optimize\` and \`#pragma unroll\` in Order Book.

### Results (RDTSC Benchmark, 20K iterations):
| Metric | Value (cycles) |
| :--- | :--- |
| Min | 20 |
| P50 | 22 |
| P99 | 24 |

> See [\`docs/OPTIMIZATION_LINEAGE.md\`](docs/OPTIMIZATION_LINEAGE.md) for the full history.

---

## 🛡️ Disclaimer
This codebase is educational and for research. Trading crypto and derivatives involves substantial risk.
The author assumes no liability for any trading losses or damages caused by using this software.
