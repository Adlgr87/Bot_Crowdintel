# Changelog — Bot_Crowdintel

## [Unreleased] — MutaLambda hot-path integration

### Integrated from MutaLambda evolutionary optimisation

Hot-path kernels in `core/crypto/`, `core/include/`, and `core/src/` were
optimised with MutaLambda's genetic engine (`benchmarks/cpp_hotpath.py`)
driving clang++ × agnes-2.5-flash / g++ × gemini-2.5-flash. All mutations
preserve bit-identical Keccak-256 KAT output and zero functional behaviour
changes.

#### `core/crypto/keccak256.hpp` — `keccak_f1600`
- **Mutation:** θ-step iteration reversed (x: 4→0) to break back-to-back
  latency chain between consecutive `C[x]` computations.
- **Metric:** 1.3156× speedup on clang++ 22.1.8 (−547.8 → 416.2 ns/op).
  (g++ 13.3.0: 1.0373× / +3.73 %.)

#### `core/crypto/eip712_signer.hpp` — `sign_order`
- **Mutation:** `order_typehash_` pre-computed once at `init()` time (eliminates
  1 redundant `keccak256` per `sign_order` call); `__builtin_expect` applied to
  cold ECDSA failure paths.

#### `core/include/spsc_ring_buffer.hpp` — `try_push` / `try_pop`
- **Mutation:** `__builtin_expect` on rare full/empty branches for
  improved branch prediction on the fast path.

#### `core/src/execution_engine.hpp` — `run_tick`
- **Mutation:** Hot scalars (`p_win`, `price_raw_d`) hoisted into locals
  with `__builtin_expect` on filter rejection branches.

### Correctness
- Keccak-256 KAT: bit-identical to Ethereum-canonical vectors under both
  clang++ and g++.
- EIP-712 signature output: bit-identical (deterministic ECDSA).
- SPSC ring buffer: same acquire/release ordering, unchanged semantics.
- Engine: identical `TickResult` classification for identical inputs.

### Reproducing the MutaLambda keccak run
```bash
MUTALAMBDA_UNSAFE_LOCAL=1 python benchmarks/cpp_hotpath.py \
  --compiler clang++ --backend openai --model agnes-2.5-flash \
  --generations 14 --islands 3 --population 50 \
  --samples 10 --warmups 2
```
Full results: `benchmarks/results/results_cpp_keccak.json` in MutaLambda.
