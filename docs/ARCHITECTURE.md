# 🏛️ Architecture: Bot CrowdIntel (Ultra-Low Latency)

## 1. Design Philosophy
The bot is designed for **Deterministic Execution**. Every microsecond counts. The system is split into a **Hot Path** (critical execution) and a **Cold Path** (alpha generation and management).

### Core Principles:
- **Zero Allocation:** No `malloc`, `new`, or `std::vector` resizing during the Hot Path.
- **Lock-Free:** Communication between the Cold Path and Hot Path via Single-Producer Single-Consumer (SPSC) Ring Buffers.
- **Cache Alignment:** All critical structures are padded to 64 bytes to prevent False Sharing.
- **Deterministic Latency:** CPU pinning and kernel isolation to eliminate jitter.

## 2. System Topology

### A. The Hot Path (The Fast Lane) - C++/Rust
- **Network Ingest:** Custom WebSocket parser $\rightarrow$ Binary frames $\rightarrow$ OrderBook.
- **OrderBook L2:** Static array-backed L2 book. Update complexity: $O(1)$.
- **Execution Engine:** Strategy Eval $\rightarrow$ Position Sizing $\rightarrow$ Order Builder.
- **Crypto Engine:** EIP-712 Signing using AVX2/SIMD.
- **Wire Transmission:** `TCP_NODELAY` sockets $\rightarrow$ Polymarket CLOB V2.

### B. The Cold Path (The Brain) - Rust/Python
- **CrowdIntel Ingest:** Webhook receiver $\rightarrow$ Alpha Parser.
- **Alpha Engine:** FDR $q$-value filtering $\rightarrow$ Kelly Sizing calculation.
- **Management:** API Key rotation, Heartbeat monitoring, Logging.

## 3. Data Flow (Tick-to-Wire)
`NIC` $\xrightarrow{0.1\mu s}$ `Packet Parser` $\xrightarrow{0.5\mu s}$ `OrderBook Update` $\xrightarrow{3\mu s}$ `Strategy` $\xrightarrow{5\mu s}$ `SPSC Queue` $\xrightarrow{20\mu s}$ `EIP-712 Sign` $\xrightarrow{5\mu s}$ `Wire`

## 4. Infrastructure Topology
- **Location:** AWS Amsterdam (eu-west-3).
- **OS:** Linux 6.18+ with `PREEMPT_RT`.
- **CPU:** Intel Xeon Sapphire Rapids $\rightarrow$ `isolcpus=2,3`.
- **NIC:** ENA Express.
