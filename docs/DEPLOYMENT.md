# Bot_Crowdintel — Deployment & Build Guide

> **Reproducible offline build**. Build once, verify 28/28 tests + ASan/UBSan/TSan.
> This document is the single source of truth for reproducing the CI environment locally.

## Prerequisites

```bash
# Core build tools
sudo apt-get update
sudo apt-get install -y \
  build-essential clang cmake ninja-build git \
  libssl-dev libcurl4-openssl-dev python3-pip

# Python dependencies (for signer cross-check)
python3 -m pip install --user pycryptodome==3.23.0 coincurve==21.0.0

# TSan (Clang TSan runtime — needed for ThreadSanitizer builds)
sudo apt-get install -y libclang-rt-dev  # or: sudo apt-get install -y clang
# Note: GCC TSan runtime may be missing on some distros — use Clang for TSan:
#   ldconfig -p | grep tsan  → must show libtsan.so
```

## Step 1 — Build pinned libsecp256k1

**Commit pin:** `6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d`

```bash
# Exact CI commands
git clone --filter=blob:none https://github.com/bitcoin-core/secp256k1.git /tmp/secp256k1
git -C /tmp/secp256k1 checkout --detach 6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d
test "$(git -C /tmp/secp256k1 rev-parse HEAD)" = "6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d"

cmake -S /tmp/secp256k1 -B /tmp/secp256k1/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
  -DSECP256K1_BUILD_BENCHMARK=OFF \
  -DSECP256K1_BUILD_TESTS=OFF \
  -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF

cmake --build /tmp/secp256k1/build
```

## Step 2 — Build Bot_Crowdintel (Release)

```bash
cmake -S core -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=OFF \
  -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1

cmake --build build
```

## Step 3 — Run tests

```bash
# Native tests (28/28)
ctest --test-dir build --output-on-failure

# Python signer cross-check
build/bin/test_signer --json > /tmp/signer.json
python3 tests/crypto/cross_check_v2.py /tmp/signer.json
build/bin/test_signer --json-neg-risk > /tmp/signer-neg-risk.json
python3 tests/crypto/cross_check_v2.py /tmp/signer-neg-risk.json

# CPU latency smoke test
build/bin/latency_bench | tee /tmp/latency.txt
python3 tests/benchmarks/check_latency.py /tmp/latency.txt
```

## Step 4 — Sanitizers

### ASan + UBSan (GCC)

```bash
cmake -S core -B build_asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCROWDINTEL_NETWORK=OFF \
  -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1 \
  -DCROWDINTEL_ASAN=ON -DCROWDINTEL_UBSAN=ON

cmake --build build_asan

ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir build_asan --output-on-failure
```

### TSan (Clang recommended)

```bash
# Use Clang for TSan (GCC TSan runtime may not be installed on all distros)
cmake -S core -B build_tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCROWDINTEL_NETWORK=OFF \
  -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1 \
  -DCROWDINTEL_TSAN=ON \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang

cmake --build build_tsan

TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  ctest --test-dir build_tsan --output-on-failure
```

## Reproducible Build Script

Save as `scripts/build_offline.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="${1:-$(pwd)}"
SECP_COMMIT="6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d"

echo "=== Building libsecp256k1 at ${SECP_COMMIT} ==="
if [ ! -d /tmp/secp256k1 ]; then
  git clone --filter=blob:none https://github.com/bitcoin-core/secp256k1.git /tmp/secp256k1
fi
git -C /tmp/secp256k1 checkout --quiet --detach "$SECP_COMMIT"
cmake -S /tmp/secp256k1 -B /tmp/secp256k1/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
  -DSECP256K1_BUILD_BENCHMARK=OFF -DSECP256K1_BUILD_TESTS=OFF \
  -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF
cmake --build /tmp/secp256k1/build -j"$(nproc)"

echo "=== Building Bot_Crowdintel ==="
cmake -S "$REPO_ROOT/core" -B "$REPO_ROOT/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=OFF \
  -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1

cmake --build "$REPO_ROOT/build" -j"$(nproc)"

echo "=== Running tests ==="
ctest --test-dir "$REPO_ROOT/build" --output-on-failure

echo "=== Build complete ==="
```

## Verification Matrix

| Check | GCC | Clang | Status |
|-------|-----|-------|--------|
| 28/28 tests | ✅ | ✅ | Green |
| ASan (memory errors) | ✅ | ✅ | Green |
| UBSan (undefined behavior) | ✅ | ✅ | Green |
| TSan (data races) | ⚠️ See notes | ✅ | Green* |
| Latency bench | ✅ | ✅ | Pass |

> *GCC TSan may fail on distros missing `libtsan.so.2.0.0`. Use Clang TSan as workaround: install `libclang-rt-dev`.

## Deployment Checklist

- [x] libsecp256k1 pinned to commit `6e2c8bc4`
- [x] Recovery module enabled (`-DSECP256K1_ENABLE_MODULE_RECOVERY=ON`)
- [x] 28/28 tests pass in 3 build modes (Release, ASan, TSan)
- [x] CPU isolation configured (isolcpus=2,3 via systemd)
- [ ] Production TLS reverse proxy in front of alpha HTTP receiver
- [ ] Wallet private key encrypted at rest (outside scope of T0.1)
