#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# check_dead_headers.sh — enforce the "no orphan headers" rule mechanically.
#
# BACKGROUND
# 27 of 63 headers under core/ and infra/ are unreachable from the binary. The
# most expensive consequence already happened once: fee_calculator_v2.hpp
# shipped an ADR, its own tests, and a Python parity oracle for a fee model the
# bot does not use, so a reader (or an agent) implementing "the fee phase"
# naturally edits the wrong file. A written rule in CONTRIBUTING does not stop
# that; a failing check does.
#
# WHAT IT DOES
#   1. Computes the transitive quoted-include closure from the production entry
#      point (core/src/main_hot_path.cpp).
#   2. Lists every .hpp under core/ and infra/ that the closure never reaches.
#   3. Compares that list against the recorded baseline.
#
# EXIT
#   0  the unreachable set matches the baseline exactly
#   1  a header became unreachable that was not in the baseline (new dead code)
#   2  a baselined header became reachable (baseline is stale: update it)
#
# Usage:
#   ./scripts/check_dead_headers.sh              # verify
#   ./scripts/check_dead_headers.sh --update     # rewrite the baseline
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

ENTRY="core/src/main_hot_path.cpp"
BASELINE="docs/dead_headers_baseline.txt"
UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1

[ -f "$ENTRY" ] || { echo "FATAL: $ENTRY not found" >&2; exit 2; }

# ── 1. transitive quoted-include closure ─────────────────────────────────────
work=()
for f in $(find core infra alpha -name '*.hpp' -o -name '*.cpp' 2>/dev/null); do :; done
work+=("$ENTRY")
declare -A seen=()

while [ ${#work[@]} -gt 0 ]; do
    file="${work[0]}"
    work=("${work[@]:1}")
    [ -n "${seen[$file]:-}" ] && continue
    [ -f "$file" ] || continue
    seen["$file"]=1

    dir="$(dirname "$file")"
    # Quoted includes only: <angle> ones are system/third-party headers.
    while IFS= read -r inc; do
        [ -z "$inc" ] && continue
        # Resolve relative to the including file, then to the include roots.
        for candidate in "$dir/$inc" "core/include/$inc" "core/src/$inc" \
                         "core/crypto/$inc" "alpha/crowdintel/$inc" \
                         "alpha/strategy/$inc" "infra/monitoring/$inc"; do
            # shellcheck disable=SC2086
            resolved=$(realpath -m -- "$candidate" 2>/dev/null)
            if [ -f "$resolved" ]; then
                case "$resolved" in
                    "$ROOT"/*) work+=("${resolved#"$ROOT"/}") ;;
                esac
                break
            fi
        done
    done < <(grep -o '#include[[:space:]]*"[^"]*"' "$file" 2>/dev/null \
                | sed 's/.*"\(.*\)"/\1/')
done

# ── 2. every header that the closure does not reach ──────────────────────────
unreachable=()
while IFS= read -r header; do
    [ -z "$header" ] && continue
    [ -n "${seen[$header]:-}" ] && continue
    unreachable+=("$header")
done < <(find core infra -name '*.hpp' 2>/dev/null | sort)

if [ "$UPDATE" -eq 1 ]; then
    {
        echo "# Headers unreachable from $ENTRY."
        echo "# Regenerate: ./scripts/check_dead_headers.sh --update"
        echo "# Entries may only be REMOVED (by wiring or deleting the header)."
        for h in "${unreachable[@]}"; do echo "$h"; done
    } > "$BASELINE"
    echo "baseline updated: $BASELINE (${#unreachable[@]} unreachable headers)"
    exit 0
fi

[ -f "$BASELINE" ] || {
    echo "FATAL: $BASELINE missing. Run: ./scripts/check_dead_headers.sh --update" >&2
    exit 2
}

declare -A baseline=()
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    baseline["$line"]=1
done < "$BASELINE"

quit=0
newly_dead=()
for h in "${unreachable[@]}"; do
    if [ -z "${baseline[$h]:-}" ]; then
        newly_dead+=("$h")
        quit=1
    fi
done

stale=()
for h in "${!baseline[@]}"; do
    found=0
    for u in "${unreachable[@]}"; do
        [ "$u" = "$h" ] && found=1 && break
    done
    [ "$found" -eq 0 ] && stale+=("$h")
done

echo "reachable from $ENTRY: $(( ${#seen[@]} - 1 )) files"
echo "unreachable headers:    ${#unreachable[@]} (baseline ${#baseline[@]})"

if [ ${#newly_dead[@]} -gt 0 ]; then
    echo
    echo "FAIL: headers became unreachable and are not in the baseline:"
    for h in "${newly_dead[@]}"; do echo "  + $h"; done
    echo
    echo "Either wire them into main_hot_path.cpp transitively, delete them, or"
    echo "(only for a deliberate freeze) add them with --update and say why."
fi

if [ ${#stale[@]} -gt 0 ]; then
    echo
    echo "NOTE: baselined headers are now reachable (baseline is stale):"
    for h in "${stale[@]}"; do echo "  - $h"; done
    echo "Run ./scripts/check_dead_headers.sh --update to refresh it."
    [ "$quit" -eq 0 ] && quit=2
fi

exit "$quit"
