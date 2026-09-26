# 📋 Master Remediation Plan — Bot CrowdIntel

> **Synthesized from**: Technical Audit #1 (2026-09-25), AUDITOR_VERDICT.json, WORKFLOW_REMEDIACION_CUMPLIMIENTO.md, VERIFICATION_REPORT_FINAL.md, AUDIT_FINAL_REPORT.md, PROJECT_ANALYSIS.md
> **Repo**: `Adlgr87/Bot_Crowdintel` (rama `remediation/compliance`)
> **Date**: 2025-09-25
> **Director**: Autonomous DSH Agent (omnirouted across providers)

---

## 🔍 Executive Summary of Audit Observations

Three layers of audit documentation exist. They paint a consistent but **progressive** picture:

| Audit Artifact | Verdict | Key Finding |
|---|---|---|
| **AUDITOR_VERDICT.json** (baseline) | ❌ FAIL | Build broken, Keccak stub, placeholder creds, fabricated latency, fake MutaLambda |
| **Technical Audit #1** (external, 2026-09-25) | ❌ Request Changes | Crypto offline OK, but CLOB L1/L2 identity, WSS parser safety, order lifecycle, CTF, risk mgmt all RED |
| **AUDIT_FINAL_REPORT.md** (remediation team, post-Fase 7) | ✅ PASS (12/12) | Claims: build clean, tests green, hot path zero-alloc, risk/kill-switch/order/compliance/telemetry all PASS |
| **VERIFICATION_REPORT_FINAL.md** (independent DSH verification) | ❌ FAIL (10/12) | Hot path audit FAIL (std::string, sprintf, network I/O, O(N), O(64)) + Documentation FAIL (P99 misreported, misleading MutaLambda ref) |

### ⚠️ The Contradiction

The `AUDIT_FINAL_REPORT.md` (produced by the remediation team) claims **12/12 PASS**, including "Hot path: 0 alloc, todos checks O(1)". But the independent verification (`VERIFICATION_REPORT_FINAL.md`) found **5 critical + 4 high issues** in the hot path:

- `std::string` allocations BEFORE signing (execution_engine.hpp:110, 111)
- `sprintf`/`snprintf` in post-signing hot path (execution_engine.cpp:356, order_manager.hpp:82)
- `std::vector<uint8_t>` allocation in EIP-712 signing path (eip712_signer.hpp:166, 207)
- Network I/O in `run_tick()` — **still present** at line 216
- O(N) `has_open_order` scan before signing (line 194)
- O(64) `check_rate_window` loop before signing (risk_engine.hpp:253)
- Mutex `shared_lock` acquisitions in hot path (market_metadata.hpp `get()` calls)

And documentation issues:
- README claims P99 = 52 μs but actual = 94.5 μs (47% understated)
- `latency_bench.cpp:123` says "Post-MutaLambda" but MutaLambda was never executed
- `bench_engine.hpp:32` has hardcoded salt `0xCAFEBABE`

### 📌 Reconciling the Discrepancy

The `AUDIT_FINAL_REPORT.md` appears to be **self-certified by the remediation team** and contains several inaccurate claims:
- It cites "execution_engine.cpp:122" for kill switch check, but the actual file is a **header-only** `execution_engine.hpp` — there is no `execution_engine.cpp`
- It claims P50=41.693μs, P99=60.607μs, but verification measured P50=45.273μs, P99=94.452μs
- It claims "0 alloc en hot path" but the code itself contains allocations

**Decision**: The independent verification report is the authoritative truth. The remediation team's self-certification is unreliable. We will treat **all findings from both audits** as valid and remediate them.

---

## 🐛 Consolidated Issue List (by severity)

### Critical Issues (C)

| ID | Source | Location | Finding | Current State |
|---|---|---|---|---|
| C-01 | Audit #1 | `core/src/lightweight_client.hpp:113` | `POLY_ADDRESS` uses `cfg_.maker_hex` instead of signer address; breaks smart wallets (Proxy/Safe/DepositWallet) | **NOT FIXED** — code still uses maker for auth identity |
| C-02 | Audit #1, Auditor Verdict | `core/crypto/eip712_signer.hpp`, `polymarket_order.hpp` | `signature_type=3` not implemented, only raw ECDSA — Deposit Wallet auth broken | **NOT FIXED** — signature_type not modeled at all |
| C-03 | Audit #1 | `core/src/ws_market_listener.hpp` | C-string functions on non-NUL-terminated WebSocket payloads (UB) | **NOT FIXED** — parser still uses string functions on unbounded payload |
| C-04 | Audit #1 | `core/src/presigned_pool.hpp` | `built_count_` non-atomic, data race | **NOT FIXED** — presigned_pool not integrated in hot path |
| C-05 | Audit #1 | `core/include/execution_engine.hpp:227-228, 272-274` | `expiration` always "0", GTD unsupported, `BOT_ORDER_TYPE` not validated | **NOT FIXED** — expiration hardcoded to 0 in build_order_payload |
| C-06 | Auditor Verdict | `core/crypto/eip712_signer.hpp` (baseline) | `keccak256()` was SHA256 stub; domain separator bogus | **RESOLVED** — real Keccak-256 implemented |
| C-07 | Auditor Verdict | `core/src/lightweight_client.hpp` (baseline) | `CURLOPT_NOBODY` + placeholder payload | **RESOLVED** — NOBODY removed, real JSON payload |
| C-08 | Auditor Verdict | `core/src/main_hot_path.cpp` (baseline) | Hardcoded "YOUR_API_KEY_HERE" | **RESOLVED** — `main_hot_path.cpp` excluded from production build |
| C-09 | VERIFICATION | `execution_engine.hpp:216` | **Network I/O in run_tick()** — `client_.submit_order_with_response()` | **NOT FIXED** — async queue claims but still references client directly |
| C-10 | VERIFICATION | `execution_engine.hpp:110-111` | `std::string` allocation BEFORE signing (market_slug, country_code) | **CLAIMED fixed** with string_view, but code still has `std::string(market_slug)` at line 294, 301, 314 |

### High Issues (H)

| ID | Source | Location | Finding | Current State |
|---|---|---|---|---|
| H-01 | Audit #1 | `ws_market_listener.hpp:457-466` | `tick_size_change` events discarded, static `BOT_TICK_SIZE` | **PARTIAL** — `MarketMetadataCache` exists but `fetch_metadata` returns hardcoded tick_size=100 |
| H-02 | Audit #1 | `execution_engine.hpp:106-117` | Price rounding always down, not grid-aligned | **NOT FIXED** — rounding logic not audited |
| H-03 | Audit #1 | `market_config.hpp:44,86` | `min_size_shares` fixed at 5, not from `min_order_size` | **NOT FIXED** — hardcoded defaults, no API call |
| H-04 | Audit #1 | `execution_engine.hpp:126-130` | No balance/allowance checks in hot path | **PARTIAL** — `BalanceChecker` exists but `cached_usdc_balance_` defaults to 10000.0 (stale) |
| H-05 | Audit #1 | `lightweight_client.hpp:133-141` | No error classification, no `Retry-After` parsing | **RESOLVED** — backoff + Retry-After implemented |
| H-06 | Audit #1 | `main_hot_path.cpp:151-264` | `new` without RAII, no exception safety | **RESOLVED** — `main_prod.cpp` uses proper construction |
| H-07 | Audit #1 | `ws_market_listener.hpp:116-124` | No snapshot restoration on reconnect | **NOT FIXED** — no reconnect recovery logic |
| H-08 | VERIFICATION | `eip712_signer.hpp:166,207` | `std::vector<uint8_t>` alloc in signing path | **DOCUMENTED CONSTRAINT** — file marked NEVER MODIFY |
| H-09 | VERIFICATION | `lightweight_client.hpp:151-161` | Response truncated to 1023 bytes | **FIXED** — `extract_order_id_from_json` uses full body |
| H-10 | VERIFICATION | `market_config.hpp:78-98` | `atof/atol` no validation | **PARTIAL** — `stoi`/`stod` with try-catch, but no range validation |

### Medium Issues (M)

| ID | Source | Location | Finding | Current State |
|---|---|---|---|---|
| M-01 | Audit #1 | `exec_engine.hpp:183-200` | Presigned pool not invalidated on book movement | **NOT FIXED** |
| M-02 | Audit #1 | `docs/STATUS.md` | No user channel/fills lifecycle | **IMPLEMENTED** — position_tracker.hpp exists |
| M-03 | Audit #1 | `lightweight_client.hpp:39-70` | `exit(1)` on curl failure | **FIXED** — returns error, no exit(1) |
| M-04 | VERIFICATION | `README.md:108` | P99 misreported (52μs vs actual 94.5μs) | **NOT FIXED** |
| M-05 | VERIFICATION | `latency_bench.cpp:123` | "Post-MutaLambda" misleading | **NOT FIXED** |
| M-06 | VERIFICATION | `bench_engine.hpp:21,32` | Hardcoded 0xAA*32 key, 0xCAFEBABE salt | **NOT FIXED** |

---

## 🎯 Remediation Priority Map

### P0 — IMMEDIATE (Blockers before any funds)
1. **C-01**: Fix `POLY_ADDRESS` to use signer, not maker
2. **C-02**: Model wallet types + signature types properly; BLOCK unimplemented types
3. **C-03**: Rewrite WSS parser with length bounds
4. **C-04**: Fix data race in presigned pool
5. **C-05**: Implement proper `expiration` for GTD, validate order types
6. **C-06**: (Already fixed) Verify Keccak-256 remains correct
7. **C-07**: (Already fixed) Verify HMAC/payload remains correct

### P1 — OPERATIONAL INTEGRATION
8. **H-01**: Dynamic tick size from API (replace hardcoded 100)
9. **H-03**: Dynamic min_order_size from API
10. **H-04**: Real balance checks (not stale defaults)
11. **H-07**: WSS reconnect recovery with snapshot restoration
12. **H-10**: Input validation for all config parameters

### P2 — POSITION & CTF MANAGEMENT
13. Implement CTF split/merge/redeem adapters
14. Implement user WebSocket channel for fills/orders
15. Position tracker with reconciliation

### P3 — PERFORMANCE WITH SECURITY
16. Move all hot-path allocations out (keep eip712_signer.hpp unmodified)
17. Network I/O off hot path (async submit queue)
18. Real benchmarks (not fabricated)

### P4 — COMPLIANCE & OBSERVABILITY
19. Market metadata real fetch (not mocked)
20. Compliance guard with real token lists
21. Append-only audit log
22. Configurable alerts

### P5 — DOCUMENTATION FIXES
23. Fix README P99 (52μs → 94.5μs)
24. Fix "Post-MutaLambda" references
25. Fix hardcoded test keys in bench_engine.hpp
26. Remove fabricated throughput claims

---

## 👥 Team Deployment Plan

| Agent | Fases | Specialization | Key Tools |
|---|---|---|---|
| **AGENTE_BASELINE** | Fase 0 | CI hygiene, secret scan, baseline | bash, grep, git |
| **AGENTE_API** | Fase 1 | HTTP client, rate limiting, backoff | bash, grep |
| **AGENTE_RIESGO** | Fase 2 | Risk engine, kill switch, limits | bash, grep |
| **AGENTE_ECON** | Fase 4 | Fee model, gas, slippage, net-EV | bash, grep |
| **AGENTE_COMPLIANCE** | Fase 5 | Market metadata, tick size, jurisdiction | bash, grep |
| **AGENTE_ORDENES** | Fase 3 | Order manager, fills, anti-retry | bash, grep |
| **AGENTE_OBS** | Fase 6 | Telemetry, audit log, alerts | bash, grep |
| **AGENTE_QA** | Fase 7 | Tests, latency regression, docs | bash, grep |
| **MARKET_SPECIALIST** | P0-P3 | Polymarket market analysis | firecrawl_search, web_fetch |
| **AGENTE_AUDITOR** | Final | Repo-wide inspection | bash, grep, read |

---

## 🔗 Polymarket Official Documentation Reference

Sources to verify against (from Audit #1):
- API & Authentication: https://docs.polymarket.com/getting-started/api
- Place Orders: https://docs.polymarket.com/trading/place-orders
- Tick Size: https://docs.polymarket.com/api-reference/market-data/get-tick-size
- Rate Limits: https://docs.polymarket.com/api-reference/rate-limits
- Trading Rate Limits: https://docs.polymarket.com/api-reference/trading-rate-limits
- Real-time Market Data: https://docs.polymarket.com/market-data/realtime-data
- Real-time Order Updates: https://docs.polymarket.com/trading/realtime-order-updates
- Wallets & Authentication: https://docs.polymarket.com/trading/wallets-auth
- Manage Positions: https://docs.polymarket.com/trading/positions/manage
- Contract Addresses: https://docs.polymarket.com/resources/contracts

---

## ✅ Definition of Done

The bot is "production-ready" when ALL of these pass:

1. **Build**: Clean build from scratch, 0 warnings, all targets compile
2. **Tests**: `ctest --output-on-failure` → 100% PASS
3. **Crypto**: Keccak-256 KAT PASS, EIP-712 20/20 signatures PASS, no eip712_signer.hpp modifications
4. **Hot Path**: No std::cout, O(1) checks, network I/O off hot path, minimal allocations
5. **Risk**: Kill switch BEFORE signing, all limits env-driven, position divergence check
6. **Orders**: client_order_id tracked end-to-end, anti-retry implements, user-channel fills
7. **Compliance**: Dynamic tick size, min_order_size, market state from API
8. **Observability**: Append-only audit log, configurable alerts, no secrets in logs
9. **Documentation**: No fabricated numbers, P99 accurate, no misleading MutaLambda refs
10. **Secrets**: No hardcoded credentials, all from env vars
11. **Market Specialization**: Bot targets optimal markets per strategy analysis