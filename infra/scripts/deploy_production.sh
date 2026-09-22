#!/bin/bash
# deploy_production.sh — build locally, ship the binary, install a systemd unit.
# Usage: ./deploy_production.sh [user@host] [core-id-for-hot-loop]
set -euo pipefail

DEPLOY_TARGET="${1:?usage: deploy_production.sh user@host [hot-core-id]}"
HOT_CORE="${2:-2}"
APP_DIR="/opt/crowdintel"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$BOT_ROOT/core/build"

echo "==> 1/4 Building (deterministic flags, tests must pass)"
cmake -S "$BOT_ROOT/core" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "==> 2/4 Shipping binary to $DEPLOY_TARGET:$APP_DIR"
ssh "$DEPLOY_TARGET" "sudo mkdir -p $APP_DIR && sudo chown \$(whoami) $APP_DIR"
scp "$BUILD_DIR/bin/crowdintel_bot" "$DEPLOY_TARGET:$APP_DIR/crowdintel_bot"

echo "==> 3/4 Installing systemd unit (hot core $HOT_CORE)"
ssh "$DEPLOY_TARGET" "sudo tee /etc/systemd/system/crowdintel.service > /dev/null" <<EOF
[Unit]
Description=CrowdIntel Polymarket CLOB V2 trader
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=$APP_DIR/crowdintel_bot
EnvironmentFile=$APP_DIR/env
Restart=on-failure
RestartSec=2
CPUAffinity=$HOT_CORE
Nice=-10
IOSchedulingClass=realtime
LimitMEMLOCK=64M
# Hardening
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

echo "==> 4/4 Next manual steps (secrets never travel through this script):"
echo "    ssh $DEPLOY_TARGET"
echo "    sudo editor $APP_DIR/env   # BOT_PRIVATE_KEY_HEX, CLOB_*, BOT_TOKEN_ID, BOT_PIN_CPU=$HOT_CORE"
echo "    sudo chmod 600 $APP_DIR/env"
echo "    sudo systemctl daemon-reload && sudo systemctl enable --now crowdintel"
echo "    (first time: also run infra/scripts/kernel_tuning.sh on the host)"
