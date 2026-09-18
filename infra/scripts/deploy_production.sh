#!/bin/bash
# deploy_production.sh
# Deploys the Bot CrowdIntel to a production bare-metal server.
# Prerequisites: Bare-metal server in Amsterdam (AWS eu-west-3 equivalent),
# with root access and an isolated CPU core (e.g., core 2).

set -e

APP_NAME="crowdintel-bot"
DEPLOY_USER="ubuntu"
DEPLOY_SERVER="${1:-your.bare.metal.server.com}"
SSH_KEY_PATH="${HOME}/.ssh/id_rsa"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# --- 1. Build the deterministic binary ---
echo "🔨 1. Building the production binary with LTO..."
cd "$BOT_ROOT/core/build"
make -j$(nproc)

# --- 2. Copy binary to server ---
echo "📦 2. Copying binary to $DEPLOY_SERVER..."
scp -i "$SSH_KEY_PATH" "$BOT_ROOT/core/build/bin/crowdintel_bot" "$DEPLOY_USER@$DEPLOY_SERVER:/home/$DEPLOY_USER/$APP_NAME"

# --- 3. Run Kernel Tuning on the server ---
echo "⚙️ 3. Running kernel tuning on the server..."
scp -i "$SSH_KEY_PATH" "$SCRIPT_DIR/kernel_tuning.sh" "$DEPLOY_USER@$DEPLOY_SERVER:/tmp/"
ssh "$DEPLOY_USER@$DEPLOY_SERVER" -i "$SSH_KEY_PATH" "sudo bash /tmp/kernel_tuning.sh"

echo "🚀 Deployment initiated. Please reboot the server and then run the bot with:"
echo "   ssh $DEPLOY_USER@$DEPLOY_SERVER"
echo "   sudo taskset -c 2 /home/$DEPLOY_USER/$APP_NAME"
