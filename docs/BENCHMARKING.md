# Benchmark and replay methodology

## What `latency_bench` measures

The benchmark uses production `ExecutionEngine`, `PresignedPool`, `EIP712Signer`, and mock gateway interfaces. It reports nanoseconds for:

- consumable pre-signed pool lookup/copy;
- decision + pool + mock-submit;
- inline decision + EIP-712 sign + mock-submit fallback;
- isolated EIP-712 signature;
- warm-up and measured production/rejection counts.

It consumes slots exactly once and replenishes between batches. This avoids the old error of repeatedly timing an unrealistically reusable signature.

## What it does not measure

The CPU benchmark does **not** include:

- DNS resolution;
- TCP connect or retransmission;
- TLS handshake/session reuse;
- HTTP serialization/write/read;
- HMAC header generation on the real gateway path;
- WSS parsing or alpha HTTP parsing;
- venue queue/matching/semantic acknowledgement;
- scheduler/IRQ/NIC effects on the deployment host.

Therefore a sub-microsecond pool result is not a sub-microsecond order. The relevant production result is a segmented end-to-end latency distribution plus correctness and reconciliation.

## Reproducible local procedure

```bash
cmake -S core -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_CPU_TARGET=portable -DSECP256K1_ROOT=/path/to/pinned/secp256k1
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure

# Record host/kernel/compiler/governor/affinity and then run several trials.
uname -a
c++ --version
lscpu
build-release/bin/latency_bench | tee latency-$(date -u +%Y%m%dT%H%M%SZ).txt
python3 tests/benchmarks/check_latency.py latency-*.txt
```

The CI checker uses deliberately loose p50 budgets to detect gross algorithmic regressions. Shared-runner timing is too noisy for p99 SLO enforcement or comparisons between commits.

## Representative validated result

One portable Release+LTO, CPU-only run in the development sandbox on 2026-09-30 produced:

| Metric | p50 |
|---|---:|
| pool lookup/copy | 122 ns |
| decision + pool + mock-submit | 431 ns |
| inline decision + sign + mock-submit | 30.869 µs |
| isolated EIP-712 signature | 28.894 µs |

Those values characterize that build/environment only. They are not target-host or network measurements.

## Production measurement

Use monotonic timestamps and report p50/p95/p99/p99.9 separately for:

1. alpha receive → normalized queue publication;
2. hot-loop queue receive → decision;
3. decision → gateway enqueue;
4. gateway dequeue → HMAC/body ready;
5. DNS, connect, TLS (new and reused sessions);
6. request write → first byte;
7. first byte → parsed semantic response;
8. submit → independently observed order/fill state.

Correlate by a non-secret local intent ID and venue order ID. Do not log signed headers or credentials. Compare warm/cold connections, feed reconnects, CPU migration, and packet loss. Optimize only after correctness and state consistency pass.

## Replay backtester

`l2_backtester` is a deterministic smoke/research tool, not a profitability claim. It:

- marks each simulated execution to the next tick rather than the same tick;
- applies the V2 fee model `C × feeRate × p × (1-p)`;
- excludes the final trade when no future mark exists;
- reports observed trades/win rate/PnL after fees.

Remaining limitations include no queue position, partial fills, impact, adverse venue selection, alpha transport delay, network delay, rejects, balance constraints, settlement, or correlated regime analysis. A strategy must survive event-driven simulation with calibrated delay/impact and then controlled shadow/canary validation before any economic conclusion.
