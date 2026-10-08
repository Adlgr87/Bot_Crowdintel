#!/usr/bin/env bash
# Kill switch for CrowdIntel bot
# Usage: ./scripts/kill_switch.sh [--signal|--file|--api]
set -euo pipefail

BOT_PID="${BOT_PID:-}"
BOT_KILL_SWITCH_FILE="${BOT_KILL_SWITCH_FILE:-/tmp/crowdintel_kill}"
BOT_ADMIN_URL="${BOT_ADMIN_URL:-http://127.0.0.1:9090/admin/kill}"

method="${1:---file}"

case "$method" in
  --file)
    touch "$BOT_KILL_SWITCH_FILE"
    echo "✅ Kill switch file created: $BOT_KILL_SWITCH_FILE"
    echo "   Bot will HALT at next tick (fail-closed)."
    ;;
  --signal)
    if [ -z "$BOT_PID" ]; then
      echo "❌ BOT_PID not set. Usage: BOT_PID=<pid> $0 --signal"
      exit 1
    fi
    kill -SIGUSR1 "$BOT_PID"
    echo "✅ SIGUSR1 sent to PID $BOT_PID. Graceful shutdown initiated."
    ;;
  --api)
    curl -sf -X POST "$BOT_ADMIN_URL" -H 'Content-Type: application/json' \
      -d '{"action":"halt","reason":"manual_kill_switch"}' \
      && echo "✅ Kill signal sent via API." \
      || echo "❌ API kill failed (is the admin endpoint enabled?)."
    ;;
  *)
    echo "Usage: $0 [--file|--signal|--api]"
    exit 1
    ;;
esac
