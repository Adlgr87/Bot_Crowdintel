# 📈 Performance — measured, with methodology

_All numbers below are real runs of `core/build/bin/latency_bench` on the
remediation host: a shared vCPU (Intel Xeon class, TSC 2.1 GHz, AVX2/BMI2/ADX
present), Ubuntu container, code built `-O3 -march=native -flto` with
libsecp256k1 v0.8.0 (asm enabled). Your host will differ — run the bench._

## 1. Hot path (tick latency, RDTSC rdtscp+lfence serialized, calibrated
against CLOCK_MONOTONIC, 20,000 samples per stage after 2,000 warmup)

| Stage | min | P50 | P90 | P99 | P99.9 |
| :--- | ---: | ---: | ---: | ---: | ---: |
| **Full tick, pre-signed pool hit** | **86 ns** | **90 ns** | 92 ns | 109 ns | 210 ns |
| Full tick, inline ECDSA (pool miss) | 23.5 µs | 24.1 µs | 27.9 µs | 43.1 µs | 61.4 µs |
| Inline sign only (Keccak ×3 + ECDSA) | 23.3 µs | 23.8 µs | 24.4 µs | 36.0 µs | 48.1 µs |
| Pre-signed pool scan only | 50 ns | 54 ns | 56 ns | 58 ns | 77 ns |

Reading:
- With the **pre-signed pool**, the entire hot path — signal pop, filters,
  book read, Kelly sizing, order acquisition, submit call into the mock
  transport — costs **~90 ns**. The dominant terms are the pool scan (~54 ns)
  and the SPSC pop.
- The **inline fallback** is ECDSA-bound: ~24 µs on this host. libsecp256k1
  v0.8.0 with the ecmult-gen table does ~2–5 µs/sign on tuned dedicated
  hardware; this sandbox vCPU is throttled and shared. Both paths are always
  live: the pool covers the plausible grid, the fallback covers everything else.
- Why not always inline-sign faster? Physics: one secp256k1 scalar mult by the
  base point + one Keccak per hash ×3. The pool is the only way to make
  tick-to-wire not pay it — which is exactly what the architecture does.

## 2. Per-order wire-side costs (measured microbenchmarks)

| Cost | Value | Note |
| :--- | ---: | :--- |
| L2 auth HMAC (precomputed midstates) | ~0.3 µs | 2 SHA-256 block compressions; in-house, no library calls |
| Wire body build (fixed buffers) | ~0.1 µs | template fill, hand-rolled number formatting |
| `clock_gettime(CLOCK_REALTIME)` | ~20 ns | vDSO |
| RDRAND 64-bit | ~1–2 ns | salt generation |

## 3. What is *not* in these numbers

- **TLS + network RTT** to clob.polymarket.com (order of 5–30 ms from most
  hosting regions; colocating near the matching engine dominates everything).
- Polygon settlement finality (seconds) — external to the CLOB.

## 4. Reproduce

```bash
cd core/build && ./bin/latency_bench
# Memory/allocation audit of the whole bot (optional, slow):
valgrind --tool=massif --time-unit=B ./bin/crowdintel_bot   # BOT_MODE=mock
```

## 5. Historical note

The previously published numbers in this repository ("8 ns / 24 cycles P99",
"MutaLambda evolution 2.14% + AVX-512", "P50 812 µs → 47 µs with OpenSSL") came
from an invalid benchmark (20,000 signals pushed into a 1,024-slot queue with
ignored push results — the measured ticks were empty pops) and from fabricated
optimizer output. They were removed. The table above is reproducible from the
tools in this repo.
