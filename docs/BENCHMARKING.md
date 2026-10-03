# Benchmark and replay methodology

## What `latency_bench` measures

The benchmark uses production `ExecutionEngine`, `PresignedPool`, `EIP712Signer`, and mock gateway interfaces. It reports nanoseconds for:

- consumable pre-signed pool lookup/copy;
- decision + pool + mock-submit;
- inline decision + EIP-712 sign + mock-submit fallback;
- isolated EIP-712 signature;
- warm-up and measured production/rejection counts.

It consumes slots exactly once and replenishes between batches. This avoids the old
error of repeatedly timing an unrealistically reusable signature.

The benchmark pins its own identity (`BOT_MODE=paper` plus an explicit
`BOT_TOKEN_ID`) because it measures the CPU path only and never contacts a venue;
since paper no longer inherits the replay test token id, an unpinned identity makes
every wire body fail. It counts rejections per `TickResult` and, if a loop produces no
sample at all, prints the reasons and exits 1 — a `p50` over an empty set used to be
reported as `0 ns`, which `check_latency.py` accepted as a pass. That script now also
rejects `p50=0` and any `no samples` line.

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

Portable Release+LTO, CPU-only run in the development sandbox (2 vCPU, GCC 12.2) on
2026-10-03, both loops 10000/10000 productive:

| Metric | min | p50 | p90 | p99 |
|---|---:|---:|---:|---:|
| consumable pool lookup/copy | 86 ns | 123 ns | 237 ns | 537 ns |
| decision + pool + mock-submit | 320 ns | 436 ns | 671 ns | 930 ns |
| decision + inline sign + mock-submit | 37224 ns | 37581 ns | 40512 ns | 60811 ns |
| isolated EIP-712 signature | 35984 ns | 36323 ns | 45695 ns | 63128 ns |

Reading: the inline path is ~86× the pooled path, and ~33 µs of its ~37 µs is
Keccak+ECDSA. The bottleneck is the signature, not the decision or the body copy, so
the pre-signed consumable ladder is what buys the fast path — and no part of the
decision path needs optimising on these numbers.

An equivalent run on 2026-09-30 (same build class, different sandbox) gave 122 ns /
431 ns / 30.869 µs / 28.894 µs; run-to-run differences of this size between machines
are expected. These values characterize that build and environment only. They are not
target-host or network measurements, and no figure here is an end-to-end order latency
or an SLO.

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
