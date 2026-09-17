# ⚡ Bot CrowdIntel: Low-Latency Polymarket Trader

A C++20 implementation of a high-frequency trading bot for Polymarket's CLOB V2.
The bot is structured around a deterministic, zero-allocation **Hot Path** for order execution
and a **Cold Path** for signal processing and risk management.

It is designed for deployment on a low-latency Linux server (e.g., in an AWS eu-west-3 region).

---

## 📐 Project Structure

```
core/
  ├── include/          # Hot Path headers (Order Book, SPSC Queue)
  ├── crypto/           # EIP-712 signing logic
  └── src/              # Execution Engine, WebSocket listener, HTTP Client
alpha/
  ├── crowdintel/       # Signal parsing (FDR q-value filtering)
  └── strategy/         # Kelly Criterion position sizing
infra/
  ├── scripts/          # Kernel tuning, optimization orchestration
  └── docker/           # Production build definitions
tests/
  ├── benchmarks/       # Latency measurement (RDTSC)
  └── replay/           # L2 backtesting utilities
docs/                   # Technical documentation
```

---

## 🔧 Core Components

### 1. Hot Path (`core/`)
- **`OrderBookL2`**: An in-memory Level-2 order book using fixed-size arrays
  for O(1) updates. Aligned to cache lines to prevent false sharing.
- **`SPSC_RingBuffer`**: A lock-free, single-producer/single-consumer queue.
  Used to pass signals from the Cold Path to the Hot Path without mutexes.
- **`EIP712Signer`**: A signer for EIP-712 typed structured data.
  Pre-computes the domain separator to minimize cycles during signing.
- **`ExecutionEngine`**: The main loop. Pops signals from the SPSC queue,
  reads the `OrderBookL2`, computes size via the Kelly Criterion, and submits
  signed orders via the `LightweightCLOBClient`.
- **`LightweightCLOBClient`**: A TCP/HTTP client using `TCP_NODELAY` to send
  signed orders directly to the Polymarket CLOB, avoiding heavy SDKs.

### 2. Cold Path (`alpha/`)
- **`AlphaParser` / `AlphaReceiver`**: Receives webhook alerts from
  CrowdIntel, validates them (confidence, EV, FDR $q$-value), and enqueues
  valid signals onto the SPSC queue.
- **`WsMarketListener`**: Maintains a persistent WebSocket connection to
  `wss://ws-subscriptions-clob.polymarket.com/ws/market` to stream live
  market data and update the `OrderBookL2`.
- **`KellyEngine`**: Implements the Kelly Criterion formula to calculate
  the optimal fraction of capital to risk per trade.

### 3. Infrastructure (`infra/`)
- **`kernel_tuning.sh`**: Applies Linux kernel parameters optimized for
  low-latency networking (`net.ipv4.tcp_low_latency`, `BBR` congestion control,
  `TCP_NODELAY` defaults).
- **`Dockerfile.prod`**: A multi-stage build using `clang` with
  `-O3 -march=native -flto` for a deterministic production binary.
- **`mutalambda_optimize.py`**: An orchestration script that drives the
  evolutive optimization of hot-path functions.

---

## 🧬 Optimization with MutaLambda

The bot's performance was improved using **[MutaLambda](https://github.com/Adlgr87/MutaLambda)**,
a genetic programming engine for low-level code optimization.

The evolution process does not modify the source files directly.
Instead, it uses a **decoupled adapter** (`infra/mutalambda/adapter/mutalambda_adapter.py`)
to communicate with the MutaLambda engine.

### How It Works:
1. `mutalambda_optimize.py` identifies target functions (e.g., `sign_order`, `try_push`).
2. The adapter sends these functions to the MutaLambda engine.
3. MutaLambda applies genetic mutations (e.g., loop vectorization, instruction selection).
4. Mutated variants are compiled and benchmarked using `RDTSC` timers.
5. Beneficial mutations are logged and recommended for manual integration.

### Target Functions (Defined in `optimization_targets.json`):
- `EIP712Signer::sign_order` (`minimize_cycles`)
- `SPSC_RingBuffer::try_push` (`minimize_atomic_contention`)
- `ExecutionEngine::run_tick` (`minimize_cycle_time`)

For details on the optimization history and results, see
[`docs/OPTIMIZATION_LINEAGE.md`](docs/OPTIMIZATION_LINEAGE.md).

---

## 🚀 Getting Started

### Prerequisites
- Linux OS (Ubuntu 20.04+/Debian recommended).
- C++20 compiler (`g++` or `clang++`).
- CMake 3.16+.
- Python 3.8+ (for optimization scripts).

### Build
```bash
git clone https://github.com/Adlgr87/Bot_Crowdintel.git
cd Bot_Crowdintel/core
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=RELEASE ..
make -j$(nproc)
```

### Run
```bash
./bin/crowdintel_bot
```
This will simulate a few ticks of market data processing and order submission.
For a live run, configure your Polymarket API credentials and WebSocket endpoints
in the relevant modules.

---

## 🧪 Testing

Run the latency benchmark to measure Tick-to-Wire performance:
```bash
cd core/build
./bin/crowdintel_bot
```

Run the MutaLambda optimization pipeline (optional):
```bash
python3 infra/scripts/mutalambda_optimize.py
```

---

## ⚖️ Notes & Limitations
- The bot contains a **stub EIP-712 signer** for demonstration. A production version
  must integrate a real, audited `secp256k1` library.
- The actual latency to reach a Polygon block (`Wire-to-Block`) is ~2 seconds and is
  outside the control of this bot. The focus of the Hot Path is to minimize
  pre-block latency (`Tick-to-Wire`).
- `isolcpus` and `PREEMPT_RT` kernel tuning require bare-metal root access and are
  not applicable in shared virtualized environments.

---

## 📄 License
This project is licensed under the terms of the MIT license.
See [`LICENSE`](LICENSE) for details.
