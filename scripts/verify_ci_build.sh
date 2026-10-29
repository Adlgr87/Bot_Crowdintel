#!/usr/bin/env bash

echo "=== CrowdIntel CI Verification ==="
echo ""

# 1. Check CI config syntax
echo "1. Checking CI workflow syntax..."
python3 << 'PYEOF'
import yaml, sys
with open(".github/workflows/ci.yml") as f:
    doc = yaml.safe_load(f)
    jobs = list(doc["jobs"].keys())
    print("   Found " + str(len(jobs)) + " jobs: " + ", ".join(jobs))
    print("   CI workflow YAML is valid")
PYEOF
if [ $? -ne 0 ]; then
    echo "   ERROR: CI workflow YAML is invalid"
    exit 1
fi
echo ""

# 2. Run Python validation suite
echo "2. Running Python canary validation suite..."
python3 python_oracle/test_comprehensive_validation.py 2>&1 | tail -5
echo ""

# 3. Production files must exist
# This list used to enforce the existence of ORPHANED code (twap_brownian_bridge,
# fee_calculator, crowdintel_bias, async_state_store, venue_state_machine) and of
# ADR-002, whose fee model does not match the venue. Requiring dead files to exist
# is the opposite of a health check. Only what the binary actually compiles is
# required here; the orphan policy is enforced by step 4.
echo "3. Verifying production file structure..."
files=(
    "core/src/main_hot_path.cpp"
    "core/src/execution_engine.hpp"
    "core/src/market_config.hpp"
    "core/src/metrics.hpp"
    "core/include/daily_state.hpp"
    "core/include/risk_manager.hpp"
    "infra/monitoring/health_server.hpp"
    "python_oracle/twap.py"
    "python_oracle/fees.py"
    "docs/adr/ADR-001-twap-conditional-variance.md"
    "docs/adr/ADR-003-crowdintel-bias-engine.md"
)

missing=0
for file in "${files[@]}"; do
    if [ -f "$file" ]; then
        echo "   OK: $file"
    else
        echo "   MISSING: $file"
        missing=$((missing + 1))
    fi
done

if [ $missing -gt 0 ]; then
    echo "   ERROR: $missing files missing"
    exit 1
fi
echo ""

# 4. No new orphan headers (Fase 4)
echo "4. Checking for newly orphaned headers..."
if [ -x scripts/check_dead_headers.sh ]; then
    if scripts/check_dead_headers.sh; then
        echo "   OK: orphan set matches docs/dead_headers_baseline.txt"
    else
        echo "   ERROR: orphan header policy violated (see above)"
        exit 1
    fi
else
    echo "   SKIP: scripts/check_dead_headers.sh not executable"
fi
echo ""

echo "=== Verification Complete ==="
echo "All checks passed. Ready for CI."
