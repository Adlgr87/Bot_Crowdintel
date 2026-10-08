#!/usr/bin/env bash
# Rollback procedure for CrowdIntel bot
# Usage: ./scripts/rollback.sh [--dry-run]
set -euo pipefail

DRY_RUN="${1:---go}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="/var/log/crowdintel"

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "=== DRY RUN ==="
fi

echo "1. Sending kill switch..."
touch /tmp/crowdintel_kill
echo "   Kill file created."

echo "2. Closing open positions at market..."
# In production, this would call the Close-All endpoint
# For canary, we halt and let the journal guide position unwind
echo "   Positions will be closed on next tick HALT."

echo "3. Preserving journal..."
mkdir -p "$LOG_DIR/backup_$(date +%s)"
cp -r "$LOG_DIR/journal"* "$LOG_DIR/backup_$(date +%s)/" 2>/dev/null || true
echo "   Journal preserved."

echo "4. Finding last good commit..."
LAST_GOOD=$(git -C "$REPO_ROOT" log --oneline -20 --grep="green\|pass\|stable\|ready" | head -1 | cut -d' ' -f1)
if [ -z "$LAST_GOOD" ]; then
  LAST_GOOD=$(git -C "$REPO_ROOT" log --oneline main~1 -1 | cut -d' ' -f1)
fi
echo "   Last known-good: $LAST_GOOD"

if [ "$DRY_RUN" = "--dry-run" ]; then
  echo "=== DRY RUN: would revert to $LAST_GOOD ==="
else
  echo "5. Reverting to last known-good..."
  git -C "$REPO_ROOT" revert --no-commit "${LAST_GOOD}..HEAD" 2>/dev/null || \
    git -C "$REPO_ROOT" reset --hard "$LAST_GOOD"
  echo "6. Rebuild + redeploy..."
  cmake -S core -B build -DCMAKE_BUILD_TYPE=Release -DCROWDINTEL_NETWORK=OFF
  cmake --build build -j
fi

echo "✅ Rollback complete."
