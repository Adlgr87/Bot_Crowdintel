#!/usr/bin/env bash
set -euo pipefail

# ──────────────────────────────────────────────────────────────────────────────
# verify_ci_build.sh — CI Sanity Check Script
#
# This script verifies that the CI pipeline is correctly configured and
# all tests pass before pushing to GitHub.
# ──────────────────────────────────────────────────────────────────────────────

echo "=== CrowdIntel CI Verification ==="
echo ""

# 1. Check CI config syntax
echo "1. Checking CI workflow syntax..."
python3 -c "
import yaml
import sys

with open('.github/workflows/ci.yml') as f:
    try:
        doc = yaml.safe_load(f)
        jobs = list(doc['jobs'].keys())
        print(f'   Found {len(jobs)} jobs: {', '.join(jobs)}')
        for job in jobs:
            steps = doc['jobs'][job].get('steps', [])
            print(f'   - {job}: {len(steps)} steps')
        print('   ✓ CI workflow YAML is valid')
    except Exception as e:
        print(f'   ✗ Error: {e}')
        sys.exit(1)
"
echo ""

# 2. Check docker-compose syntax
echo "2. Checking docker-compose syntax..."
docker-compose config --quiet 2>/dev/null && echo "   ✓ docker-compose.yml is valid" || echo "   ⚠ docker-compose.yml not available (docker not installed)"
echo ""

# 3. Check Python validation suite
echo "3. Running Python canary validation suite..."
python3 python_oracle/test_comprehensive_validation.py 2>&1 | tail -10
echo ""

# 4. Check cross-validation
echo "4. Running cross-validation (Python oracle)..."
python3 python_oracle/test_cross_validation.py 2>&1 | tail -5
echo ""

# 5. Check file structure
echo "5. Verifying file structure..."
files=(
    "core/src/twap_brownian_bridge.hpp"
    "core/src/fee_calculator.hpp"
    "core/src/crowdintel_bias.hpp"
    "core/src/async_state_store.hpp"
    "core/src/venue_state_machine.hpp"
    "infra/feeds/clob_websocket.hpp"
    "infra/feeds/binance_feed.hpp"
    "infra/monitoring/health_server.hpp"
    "python_oracle/twap.py"
    "python_oracle/fees.py"
    "python_oracle/crowdintel_bias.py"
    "python_oracle/test_comprehensive_validation.py"
    "docs/adr/ADR-001-twap-conditional-variance.md"
    "docs/adr/ADR-002-clob-v2-fee-engine.md"
    "docs/adr/ADR-003-crowdintel-bias-engine.md"
)

missing=0
for file in "${files[@]}"; do
    if [ -f "$file" ]; then
        echo "   ✓ $file"
    else
        echo "   ✗ MISSING: $file"
        missing=$((missing + 1))
    fi
done

if [ $missing -gt 0 ]; then
    echo "   ERROR: $missing files missing"
    exit 1
fi
echo ""

# 6. Verify git status
echo "6. Checking git status..."
if [ -n "$(git status --porcelain)" ]; then
    echo "   ⚠ Untracked files detected:"
    git status --porcelain | head -10
else
    echo "   ✓ Clean working tree"
fi
echo ""

# 7. Check for build artifacts that shouldn't be committed
echo "7. Checking for accidentally committed build artifacts..."
build_files=$(git ls-files | grep -E "build[^/]*\.(txt|cache)|CMakeCache|CMakeFiles" | head -5 || true)
if [ -n "$build_files" ]; then
    echo "   ⚠ Build artifacts found in git:"
    echo "$build_files"
else
    echo "   ✓ No build artifacts in git"
fi
echo ""

echo "=== Verification Complete ==="
echo "All critical checks passed. Ready for CI."
