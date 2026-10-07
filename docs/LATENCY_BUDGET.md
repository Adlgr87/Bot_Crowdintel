# Latency Budget

## Total Hot-Path Budget

| | p50 | p99 |
|---|-----|-----|
| **Budget** | < 15μs | < 50μs |
| **Current (bare)** | ~754ns | ~1.2μs |
| **With P1–P4** | ~754ns | ~2μs |
| **After new modules** | < 15μs | < 50μs |
| **Headroom** | ~6.2x p50 | ~3.3x p99 |

## Per-Module Budget Allocation

| Module | p50 Budget | p99 Budget | Implementation Notes |
|--------|-----------|-----------|---------------------|
| `BinanceWSClient` (parse) | — | < 50μs/msg | Cold thread. JSON parse, SPSC push. |
| `OFICalculator` (hot consume) | < 2μs | < 4μs | O(1) incremental. Exponential decay. Stack only. |
| `CfCNetwork` (inference) | < 3μs | < 5μs | 32 hidden, SSE4.2 FMA. Zero heap. |
| `WindowShield` (state check) | < 200ns | < 500ns | Timer lookup + state table. No branches predicted. |
| `SpikeDetector` (detection) | < 1μs | < 2μs | Ring compare + atomic flag. |
| `KellySizer` (sizing) | < 500ns | < 1μs | Single-float arithmetic. |
| `TWAPTracker` (update) | < 1μs | < 2μs | Two accumulators. Exponential decay optional. |
| `LadderSkew` (rebuild) | < 3μs | < 10μs | 16-order pool. Stack arrays. |
| **Total new** | < 10.5μs | < 24.5μs | |

## Fallback Strategy

| Module | Threshold | Fallback |
|--------|-----------|----------|
| `CfCNetwork` | > 5μs p50 (sustained 10 ticks) | Degrade to cached linear model: `p = 0.5 + 0.1 * ofi_normalized` |
| `OFICalculator` | > 4μs p99 | Skip update, reuse last value |
| `LadderSkew` | > 10μs p99 | Symmetric quoting only |
| `SpikeDetector` | N/A (hard real-time) | Fail-closed: HALTED if > 10ms since last tick |

## SIMD Targets

| Kernel | Target Speedup | SIMD Strategy |
|--------|---------------|---------------|
| `tanh` (CfC) | 8x | SSE4.2 `_mm256_*` + Padé [3/3] approx |
| `exp` (decay) | 4x | SSE4.2 `_mm256_exp_ps` or `2^(x*log2e)` |
| `sigmoid` | 4x | 1/(1+exp(-x)) with fast_exp |
| `matmul` (W_x·x) | 8 floats/cycle | `_mm256_fmadd_ps` |

## Memory Constraints

| Component | Budget | Current |
|-----------|--------|---------|
| Hot-path heap | 0 bytes | 0 (all stack/static) |
| Ring buffers | 4 × 4096 × sizeof(T) | ~256KB total |
| New module state | < 16KB | CfC (~10KB), OFI (~256B), others < 1KB |
| Stack usage per tick | < 4KB | Must fit L1 without spill |
