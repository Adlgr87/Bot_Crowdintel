# Historical remediation note — 2026-09-22

This path is retained so old links do not break. It described an earlier remediation pass whose implementation and benchmark assumptions have since changed materially.

The authoritative current sources are:

- [REMEDIATION_STATUS.md](REMEDIATION_STATUS.md): observation-by-observation status and residual risk;
- [STATUS.md](STATUS.md): release readiness;
- [ARCHITECTURE.md](ARCHITECTURE.md): concurrency, signing, order, and failure invariants;
- [BENCHMARKING.md](BENCHMARKING.md): current consumable-pool methodology;
- [DEPLOYMENT.md](DEPLOYMENT.md): preflight, controlled canary, stop, reconciliation, and rollback.

In particular, earlier reusable-pool timing, `-march=native` artifacts, RDRAND fallback, same-tick replay PnL, and production-readiness language are obsolete and must not be used as evidence.
