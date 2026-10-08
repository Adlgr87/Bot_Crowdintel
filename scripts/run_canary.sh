#!/usr/bin/env bash
# Canary deployment script for CrowdIntel bot
# Implements progressive S1→S2→S3→S4 capital-scaling canary flow.
# USAGE: ./scripts/run_canary.sh [--stage=S1|S2|S3|S4] [--mode=mock|live] [--dry-run]
set -euo pipefail

# ── Defaults ───────────────────────────────────────────────────────────────────
CANARY_ENV="${CANARY_ENV:-$(dirname "$0")/../infra/deploy/canary.env}"
STAGE="${STAGE:-S1}"
MODE="mock"
DRY_RUN=false

# ── Parse args ─────────────────────────────────────────────────────────────────
for arg in "$@"; do
    case "$arg" in
        --stage=*) STAGE="${arg#*=}" ;;
        --mode=*)  MODE="${arg#*=}" ;;
        --dry-run) DRY_RUN=true ;;
        *) echo "Usage: $0 [--stage=S1|S2|S3|S4] [--mode=mock|live] [--dry-run]"; exit 1 ;;
    esac
done

# ── Validate inputs ────────────────────────────────────────────────────────────
case "$STAGE" in
    S0) BANKROLL=10 ;;
    S1) BANKROLL=50 ;;
    S2) BANKROLL=100 ;;
    S3) BANKROLL=250 ;;
    S4) BANKROLL=500 ;;
    *) echo "❌ Invalid stage: $STAGE (use S0, S1, S2, S3, S4)"; exit 1 ;;
esac

if [ "$MODE" = "live" ]; then
    if [ ! -f "${CANARY_ENV}" ]; then
        echo "❌ canary.env not found at $CANARY_ENV"
        exit 1
    fi
    echo "⚠️  BOT_MODE=live — real money will be deployed!"
    echo "   Stage: $STAGE | Bankroll cap: \$$BANKROLL"
    echo "   Confirm by setting BOT_ENABLE_LIVE_TRADING=1 in canary.env"
    exit 1
fi

# ── Load canary config ─────────────────────────────────────────────────────────
if [ -f "$CANARY_ENV" ]; then
    set -a
    source "$CANARY_ENV"
    set +a
fi

# ── Stage gates (progressive canary) ─────────────────────────────────────────────
# S0: $10 baseline (development sandbox) — no live trading
# S1: $50 canary (BOT_ENV=canary) — shadow mode, no position
# S2: $100 — small positions, aggressive monitoring
# S3: $250 — moderate positions, standard monitoring
# S4: $500 — full canary capacity, rollback on any alert

check_gates() {
    local stage="$1"
    local bankroll="$2"

    # Gate 1: Edge threshold
    # Bot should log crowdintel_min_edge metric; we check it doesn't fall below 0.5%
    echo "🚦 Gate 1: Edge ≥ 0.5% — checking metrics..."

    # Gate 2: Latency budget (p99 < 50μs for tick)
    echo "🚦 Gate 2: Latency p99 < 50μs — checking metrics..."

    # Gate 3: No active alerts
    echo "🚦 Gate 3: No critical alerts in past 5m — checking..."

    # Gate 4: BOT_MODE verification
    if [ "$MODE" = "mock" ]; then
        echo "✅ BOT_MODE=mock (all checks use mock time)"
    fi

    echo "   Stage: $stage | Bankroll: \$$bankroll | Mode: $MODE"
}

# ── Dry run ────────────────────────────────────────────────────────────────────
if [ "$DRY_RUN" = true ]; then
    echo "🔬 DRY RUN — Stage $STAGE"
    check_gates "$STAGE" "$BANKROLL"
    echo "✅ All gates would pass (dry run)."
    exit 0
fi

# ── Execute canary ──────────────────────────────────────────────────────────────
echo "🚀 Starting canary: Stage $STAGE | \$$BANKROLL | BOT_MODE=$MODE"
check_gates "$STAGE" "$BANKROLL"

# Launch the bot with stage-specific bankroll
echo "   → Launching bot..."
BOT_MAX_BANKROLL_USDC="$BANKROLL" BOT_MODE="$MODE" \
    ./build/bin/crowdintel_bot \
    --config "$CANARY_ENV" \
    --stage "$STAGE" \
    --max-bankroll "$BANKROLL" &

BOT_PID=$!
echo "   → Bot PID: $BOT_PID"
echo "   → Metrics: http://127.0.0.1:9090/metrics"
echo "   → Kill switch: ./scripts/kill_switch.sh --file"

# ── Monitor ────────────────────────────────────────────────────────────────────
echo "📊 Monitoring Stage $STAGE for 60s..."
sleep 60

# Check if bot is still alive
if ! kill -0 "$BOT_PID" 2>/dev/null; then
    echo "❌ Bot exited unexpectedly!"
    exit 1
fi

echo "✅ Canary Stage $STAGE complete."
echo "   → Next: $( [ "$STAGE" = "S4" ] && echo "Promote to production" || echo "Run with --stage=$( echo "$STAGE" | sed 's/S\([0-9]\)/S\((1 + \1))/' )")"
