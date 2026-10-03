#!/usr/bin/env python3
"""Reject gross CPU-path latency regressions without pretending CI is a NIC test."""
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
checks = {
    "consumable pool lookup+copy": 10_000,
    "decision+pool+mock-submit": 20_000,
    "decision+inline-sign+mock-submit": 500_000,
    "sign only (Keccak+ECDSA)": 500_000,
}
failed = False
if "no samples" in text:
    print("FAIL the benchmark reported a loop with no samples")
    failed = True
for label, budget_ns in checks.items():
    match = re.search(
        rf"^{re.escape(label)}.*?p50=\s*([0-9]+)\b", text, re.MULTILINE
    )
    if not match:
        print(f"FAIL missing metric: {label}")
        failed = True
        continue
    actual = int(match.group(1))
    if actual == 0:
        # A p50 of zero means the loop never submitted anything, not that the hot
        # path is infinitely fast.  This gate used to pass on exactly that.
        print(f"FAIL {label}: p50=0 ns means no sample was measured")
        failed = True
        continue
    status = "PASS" if actual <= budget_ns else "FAIL"
    print(f"{status} {label}: p50={actual} ns, gross-regression budget={budget_ns} ns")
    failed |= actual > budget_ns
print("Budgets are CPU-only smoke limits, not production latency SLOs.")
raise SystemExit(1 if failed else 0)
