#!/usr/bin/env bash
# Local developer toolchain for minimal containers (no cmake / curl / OpenSSL
# development packages installed, /tmp wiped between sessions).
#
# Everything is reconstructed from pinned upstream sources into /tmp; nothing
# is invented and no secret is used. Reproducible endpoints are limited to what
# this sandbox can reach: pypi.org (cmake/ctest wheel) and codeload.github.com
# (curl, OpenSSL, libsecp256k1). deb.debian.org/apt does not work here.
#
#   infra/scripts/dev_toolchain.sh            # toolchain only
#   infra/scripts/dev_toolchain.sh --build    # toolchain + 3 builds + ctest
#
# The pinned commit/tag values mirror infra/docker/Dockerfile.prod and
# docs/DEPLOYMENT.md; keep them in sync when those are updated.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DL=/tmp/dl
SRC=/tmp/src
SECP=/tmp/secp256k1
SECP_COMMIT=6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d
OPENSSL_TAG=openssl-3.0.13
CURL_TAG=curl-7_88_1
LIBS=/usr/lib/x86_64-linux-gnu

mkdir -p "$DL" "$SRC"

fetch() {  # fetch <url> <file>
  [ -f "$DL/$2" ] || python3 - "$1" "$DL/$2" <<'PY'
import sys, urllib.request
urllib.request.urlretrieve(sys.argv[1], sys.argv[2])
print("downloaded", sys.argv[2])
PY
}

# 1. cmake/ctest (PyPI wheel; the wrappers need PYTHONPATH=/tmp/pytools).
if [ ! -x /tmp/pytools/bin/cmake ]; then
  python3 -m pip install --quiet --target=/tmp/pytools --no-cache-dir cmake
fi
export PYTHONPATH=/tmp/pytools
CMAKE=/tmp/pytools/bin/cmake
CTEST=/tmp/pytools/bin/ctest

# 2. curl headers (the runtime libcurl.so.4 is already installed).
if [ ! -f "$SRC/curl-src/include/curl/curl.h" ]; then
  fetch "https://codeload.github.com/curl/curl/tar.gz/refs/tags/$CURL_TAG" curl.tar.gz
  tar -C "$SRC" -xzf "$DL/curl.tar.gz"
  mv "$SRC"/curl-curl-* "$SRC/curl-src" 2>/dev/null || true
fi

# 3. OpenSSL headers, generated from the source tree matching libssl.so.3.
if [ ! -f "$SRC/openssl-src/include/openssl/ssl.h" ]; then
  fetch "https://codeload.github.com/openssl/openssl/tar.gz/refs/tags/$OPENSSL_TAG" openssl.tar.gz
  tar -C "$SRC" -xzf "$DL/openssl.tar.gz"
  mv "$SRC/openssl-$OPENSSL_TAG" "$SRC/openssl-src" 2>/dev/null || true
  ( cd "$SRC/openssl-src"
    perl Configure --prefix=/tmp/openssl-headers \
         --openssldir=/tmp/openssl-headers/ssl \
         no-shared no-tests no-asm linux-x86_64 >/tmp/openssl-configure.log
    make -j4 build_generated >/tmp/openssl-generated.log )
fi

# 4. libsecp256k1 with the recovery module (pinned commit).
if [ ! -f "$SECP/build/lib/libsecp256k1.a" ]; then
  [ -d "$SECP/.git" ] || git clone --filter=blob:none \
      https://github.com/bitcoin-core/secp256k1.git "$SECP"
  git -C "$SECP" fetch --depth 1 origin "$SECP_COMMIT"
  git -C "$SECP" checkout --detach FETCH_HEAD
  test "$(git -C "$SECP" rev-parse HEAD)" = "$SECP_COMMIT"
  "$CMAKE" -S "$SECP" -B "$SECP/build" -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
      -DSECP256K1_BUILD_BENCHMARK=OFF -DSECP256K1_BUILD_TESTS=OFF \
      -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF >/dev/null
  "$CMAKE" --build "$SECP/build" -j4 >/dev/null
fi

COMMON=(-DSECP256K1_LIBRARY="$SECP/build/lib/libsecp256k1.a"
        -DSECP256K1_INCLUDE_DIR="$SECP/include")
NET=(-DCURL_LIBRARY="$LIBS/libcurl.so.4"
     -DCURL_INCLUDE_DIR="$SRC/curl-src/include"
     -DOPENSSL_INCLUDE_DIR="$SRC/openssl-src/include"
     -DOPENSSL_SSL_LIBRARY="$LIBS/libssl.so.3"
     -DOPENSSL_CRYPTO_LIBRARY="$LIBS/libcrypto.so.3")

"$CMAKE" --version | head -1
echo "toolchain ready"
[ "${1:-}" = "--build" ] || exit 0

"$CMAKE" -S "$ROOT/core" -B /tmp/build-final -DCMAKE_BUILD_TYPE=Release \
    "${COMMON[@]}" "${NET[@]}"
"$CMAKE" --build /tmp/build-final -j4

"$CMAKE" -S "$ROOT/core" -B /tmp/build-offline -DCMAKE_BUILD_TYPE=Release \
    "${COMMON[@]}" -DCROWDINTEL_NETWORK=OFF
"$CMAKE" --build /tmp/build-offline -j4

"$CMAKE" -S "$ROOT/core" -B /tmp/build-asan -DCMAKE_BUILD_TYPE=Debug \
    "${COMMON[@]}" "${NET[@]}" -DCROWDINTEL_ASAN=ON -DCROWDINTEL_UBSAN=ON
"$CMAKE" --build /tmp/build-asan -j4

for dir in /tmp/build-final /tmp/build-offline /tmp/build-asan; do
  echo "=== ctest $dir ==="
  ( cd "$dir" && "$CTEST" --output-on-failure )
done
