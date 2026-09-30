#!/usr/bin/env bash
# Build for the remote CPU contract, run tests, deploy a fail-closed systemd
# unit, and provision paths for systemd credentials.  It never copies secrets.
set -euo pipefail

DEPLOY_TARGET="${1:?usage: deploy_production.sh user@host [hot-core] [cold-core]}"
HOT_CORE="${2:-2}"
COLD_CORE="${3:-3}"
APP_DIR="/opt/crowdintel"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$BOT_ROOT/core/build-deploy}"

[[ "$DEPLOY_TARGET" =~ ^[A-Za-z0-9._-]+@[A-Za-z0-9._:-]+$ ]] || {
  echo "DEPLOY_TARGET must be a plain user@host (use SSH config for options)." >&2
  exit 2
}
[[ "$HOT_CORE" =~ ^[0-9]+$ && "$COLD_CORE" =~ ^[0-9]+$ ]] || {
  echo "hot-core and cold-core must be non-negative integers." >&2
  exit 2
}
[[ "$HOT_CORE" != "$COLD_CORE" ]] || {
  echo "hot-core and cold-core must be distinct." >&2
  exit 2
}

if [[ -n "${SECP256K1_LIBRARY:-}" && -n "${SECP256K1_INCLUDE_DIR:-}" ]]; then
  SECP_ARGS=(-DSECP256K1_LIBRARY="$SECP256K1_LIBRARY"
             -DSECP256K1_INCLUDE_DIR="$SECP256K1_INCLUDE_DIR")
elif [[ -n "${SECP256K1_ROOT:-}" ]]; then
  SECP_ARGS=(-DSECP256K1_ROOT="$SECP256K1_ROOT")
else
  echo "Set SECP256K1_ROOT or SECP256K1_LIBRARY + SECP256K1_INCLUDE_DIR." >&2
  exit 2
fi

# Let glibc's complete ISA-level probe decide; checking only AVX2/BMI2/FMA is
# insufficient for the x86-64-v3 contract. If support cannot be proven, use the
# portable target.
REMOTE_V3="$(ssh "$DEPLOY_TARGET" 'for loader in \
  /lib64/ld-linux-x86-64.so.2 \
  /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2; do
    if [ -x "$loader" ]; then
      "$loader" --help 2>/dev/null | grep -q "x86-64-v3 (supported" && echo yes || echo no
      exit
    fi
  done
  echo no')"
CPU_TARGET=portable
[[ "$REMOTE_V3" == "yes" ]] && CPU_TARGET=x86-64-v3
REMOTE_CPUS="$(ssh "$DEPLOY_TARGET" 'getconf _NPROCESSORS_ONLN')"
[[ "$REMOTE_CPUS" =~ ^[0-9]+$ ]] || {
  echo "Could not determine remote online CPU count." >&2; exit 2;
}
(( HOT_CORE < REMOTE_CPUS && COLD_CORE < REMOTE_CPUS )) || {
  echo "Requested CPU is not online on target (count=$REMOTE_CPUS)." >&2; exit 2;
}
echo "==> Remote CPU target: $CPU_TARGET (never -march=native)"

cmake -S "$BOT_ROOT/core" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release -DCROWDINTEL_CPU_TARGET="$CPU_TARGET" \
  -DCROWDINTEL_LTO=ON "${SECP_ARGS[@]}"
cmake --build "$BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure
"$BUILD_DIR/bin/latency_bench"

echo "==> Installing binary and restricted service account"
ssh "$DEPLOY_TARGET" "sudo sh -s" <<EOF
set -eu
getent group crowdintel >/dev/null || groupadd --system crowdintel
id crowdintel >/dev/null 2>&1 || useradd --system --gid crowdintel \
  --home-dir $APP_DIR --shell /usr/sbin/nologin crowdintel
install -d -o root -g crowdintel -m 0750 $APP_DIR
install -d -o root -g root -m 0700 /etc/crowdintel/credentials
EOF
REMOTE_TMP="$(ssh "$DEPLOY_TARGET" 'mktemp /tmp/crowdintel_bot.XXXXXX')"
trap 'ssh "$DEPLOY_TARGET" "rm -f \"$REMOTE_TMP\"" >/dev/null 2>&1 || true' EXIT
scp "$BUILD_DIR/bin/crowdintel_bot" "$DEPLOY_TARGET:$REMOTE_TMP"
ssh "$DEPLOY_TARGET" "sudo install -o root -g crowdintel -m 0750 \
  '$REMOTE_TMP' '$APP_DIR/crowdintel_bot' && rm -f '$REMOTE_TMP'"
trap - EXIT

echo "==> Installing fail-closed systemd unit"
ssh "$DEPLOY_TARGET" "sudo tee /etc/systemd/system/crowdintel.service >/dev/null" <<EOF
[Unit]
Description=CrowdIntel Polymarket CLOB V2 trader
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=60
StartLimitBurst=2

[Service]
Type=simple
User=crowdintel
Group=crowdintel
ExecStart=$APP_DIR/crowdintel_bot
EnvironmentFile=/etc/crowdintel/config
Environment=BOT_PRIVATE_KEY_HEX_FILE=%d/bot_private_key
Environment=CLOB_API_KEY_FILE=%d/clob_api_key
Environment=CLOB_SECRET_FILE=%d/clob_secret
Environment=CLOB_PASSPHRASE_FILE=%d/clob_passphrase
Environment=BOT_ALPHA_BEARER_TOKEN_FILE=%d/alpha_bearer_token
LoadCredential=bot_private_key:/etc/crowdintel/credentials/bot_private_key
LoadCredential=clob_api_key:/etc/crowdintel/credentials/clob_api_key
LoadCredential=clob_secret:/etc/crowdintel/credentials/clob_secret
LoadCredential=clob_passphrase:/etc/crowdintel/credentials/clob_passphrase
LoadCredential=alpha_bearer_token:/etc/crowdintel/credentials/alpha_bearer_token
RuntimeDirectory=crowdintel
RuntimeDirectoryMode=0750
UMask=0077
Restart=no
LimitMEMLOCK=64M
LimitCORE=0
RemoveIPC=true

NoNewPrivileges=true
CapabilityBoundingSet=
AmbientCapabilities=
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectKernelLogs=true
ProtectControlGroups=true
ProtectClock=true
ProtectHostname=true
ProtectProc=invisible
ProcSubset=pid
RestrictSUIDSGID=true
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
RestrictNamespaces=true
RestrictRealtime=true
LockPersonality=true
MemoryDenyWriteExecute=true
SystemCallArchitectures=native
SystemCallFilter=@system-service
# Keep @resources from @system-service: sched_setaffinity is required for the
# configured hot/cold CPU split; empty capabilities still block privileged use.
SystemCallFilter=~@mount @reboot @swap @obsolete @debug @privileged

[Install]
WantedBy=multi-user.target
EOF

cat <<EOF
Deployment staged but NOT started.

1. Create /etc/crowdintel/config (root:crowdintel, mode 0640) with non-secret values:
   BOT_MODE=live
   BOT_ENABLE_LIVE_TRADING=1
   BOT_TOKEN_ID=...
   BOT_MARKET_SLUG=...
   BOT_SIGNATURE_TYPE=0        # type 3 intentionally fails closed
   BOT_PIN_CPU=$HOT_CORE
   BOT_COLD_CPU=$COLD_CORE
   BOT_KILL_SWITCH_FILE=/run/crowdintel/kill
   BOT_TAKER_FEE_RATE=...      # fetch from market metadata
   BOT_INITIAL_POSITION_SHARES=...
   BOT_MAX_ORDER_USD=...
   BOT_MAX_EXPOSURE_USD=...
   BOT_MAX_DAILY_LOSS_USD=...

2. Put one value (optional trailing newline) in each root-only file under
   /etc/crowdintel/credentials/: bot_private_key, clob_api_key, clob_secret,
   clob_passphrase, alpha_bearer_token; chmod 0400.

3. Complete balance/allowance and paper-order preflight, then:
   sudo systemctl daemon-reload
   sudo systemctl enable --now crowdintel

The unit does not auto-restart: reconcile orders/positions before every restart.
EOF
