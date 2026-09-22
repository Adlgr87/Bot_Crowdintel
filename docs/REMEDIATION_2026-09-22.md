# 🛠️ Remediation Log — 2026-09-22

Full inventory of defects found in the audit of this repository and what was
done about each. Items are grouped: **[C]** critical, **[M]** major,
**[m]** minor/cosmetic, **[L]** logistics/hygiene.

---

## A. Correctness of the trading logic (the big ones)

| # | Sev | Defect | Resolution |
| :--- | :--- | :--- | :--- |
| A1 | C | The bot did **not** speak Polymarket's protocol. The order struct (`OrderParams(salt,maker,taker,price,size,nonce,side)`) was invented; the real V2 struct is `Order(salt,maker,signer,tokenId,makerAmount,takerAmount,side,signatureType,timestamp,metadata,builder)`. The wire body (`{"p":..,"s":..,"mker":..}`) was not the CLOB schema either. Every order would have been rejected. | Rewrote the order model (`core/src/polymarket_order.hpp`): exact V2 signed struct, ABI encoding, and the exact wire body field set/order from the official docs. Verified **byte-for-byte** against an independent Python reference. |
| A2 | C | **EIP-712 domain was zeros** (empty domain data → `domain_separator = 0x00…`): signatures not bound to any contract → invalid / replayable. | Real V2 domain: `Polymarket CTF Exchange`, version `"2"`, chainId 137, verifyingContract standard/neg-risk. Golden-vector tested. |
| A3 | C | **Wrong REST auth**: headers `X-API-Key/X-Signature/X-Passphrase/X-Timestamp` over a placeholder payload. CLOB requires `POLY_ADDRESS/POLY_SIGNATURE/POLY_TIMESTAMP/POLY_API_KEY/POLY_PASSPHRASE` with HMAC-SHA256 over `ts + METHOD + path + body` keyed by the **base64url-decoded** secret, output base64url. | `lightweight_client.hpp` implements the real scheme; HMAC key midstates precomputed at startup (~0.3 µs/order). |
| A4 | C | **Orders did not reference any market**: `OrderParams` had no `tokenId`; the engine traded `best_ask.price` from a book that **nobody ever updated** (empty book ⇒ price 0 orders). The WS "listener" pushed fake signals every 100 ms. | Single-market design: `BOT_TOKEN_ID` config → 32-byte ABI value + decimal string for the wire body; real WSS feed updates the book (snapshot + deltas); engine refuses to trade without a two-sided live book (`NO_BOOK` tick result). |
| A5 | C | Price model was `uint64_t` integer ("price × 1e6" was never used as such) with no tick rounding, no decimal support, no maker/taker amount derivation (V2 amounts are 6-decimal raw integers: BUY `makerAmount = price×size`, etc.). | Full fixed-point (×1e6) price/size model: decimal parsing, tick rounding + clamping, `__int128` amount math, liquidity clamp, min-size guard — all unit-tested. |
| A6 | C | **Salt/nonce logic broken**: `NonceManager` had a refill bug (returned stale/identical values on every 64th call boundary, `(now<<10)|i` collides across refills within the same ns) and used a fake hardcoded timestamp in earlier revisions; V2 removed nonces entirely. | `NonceManager` deleted. Salts = hardware RNG (RDRAND, ChaCha fallback); uniqueness = `timestamp(ms)` + salt per V2. 200k-draw uniqueness test. |
| A7 | M | **Kelly criterion was not Kelly**: `f = ev × confidence × 0.1`. | Exact binary-market Kelly: `f*_buy = (w−p)/(1−p)`, `f*_sell = (p−w)/p`, fractional cap + bankroll cap; known-answer tests. |
| A8 | M | Signal→order correlation gap: `AlphaSignal` carried `ev_per_dollar` that nothing recomputed against the live price; economic filters could not exist cold. | Signal carries `p_win` (+confidence, q); the engine applies the **economic** filters (edge vs live book, liquidity, min size) hot; the parser keeps the **statistical** filters (q, confidence) cold. `direction_hint` honored, else the engine trades the direction of the edge. |
| A9 | M | Latency benchmark was invalid: pre-filled 4095 signals before **every** tick outside the timed window into a queue whose fills it never verified, and labeled results "Post-MutaLambda" (fabricated lineage). Counts could be empty-vector UB. | Rewritten (`tests/benchmarks/latency_bench.cpp`): calibrated TSC (no assumed 3.0 GHz), rdtscp+lfence serialization, overhead measured, per-stage attribution (pool-hit tick / inline tick / sign-only / pool-scan), 100% productive samples, no fake labels. |
| A10 | M | `l2_backtester.run_replay` was an empty skeleton. | Implemented CSV replay through the real book + exact-Kelly taker strategy with PnL mark-outs, slippage, drawdown; smoke-tested in ctest. |
| A11 | M | `ws_market_listener` was a simulation (`sleep(100ms)` + fake signal). | Hand-rolled RFC 6455 client over OpenSSL TLS (no WS library): handshake with Accept validation (SHA-1), masked client frames, ping/pong keepalive, fragmentation, 1 MB cap, reconnect+resubscribe; zero-alloc JSON scanner for `book`/`price_change` (object AND legacy pair formats) feeding the book; parser unit-tested against official doc messages. |
| A12 | M | Old bench engine (`BenchExecutionEngine`) duplicated `ExecutionEngine` with divergent logic (hardcoded 0x11 maker, salt 0xCAFEBABE) — measurements didn't measure the real path. | `BenchEngine` mirrors the engine pipeline exactly, mock client templated; duplication removed. |
| A13 | m | `alpha_parser` hardcoded timestamp `123456789` (fixed in an earlier cycle) — kept an eye on; now uses the system clock and adds the p_win sanity filter. | Done. |
| A14 | m | MarketMakingEngine: `twap_` computed but never used; hardcoded bankroll/confidence. | EWMA fair value actually used for quotes; parameters injected; quotes validated (bid<ask, size>0). |

## B. Security

| # | Sev | Defect | Resolution |
| :--- | :--- | :--- | :--- |
| B1 | M | Secrets in `std::string` with no zeroization; HMAC over placeholder. | Fixed-size buffers, decoded secret wiped at shutdown, HMAC states wiped, `secure_zero` (volatile, non-elidable) in a shared header. |
| B2 | M | `HMAC()` return value unchecked (silent failure → empty signature). | In-house HMAC with RFC 4231 KAT; no unchecked library calls on the auth path. |
| B3 | m | No certificate pinning. | `CURLOPT_PINNEDPUBLICKEY` support via `BOT_TLS_PIN` (`sha256//…`); WSS verifies hostname + default CA store. |
| B4 | m | `BOT_PRIVATE_KEY_HEX`-less runs fell back to dummy keys in some paths (bench engines). | No dummy fallbacks: missing/invalid key = hard error; bench/test binaries use documented public KAT keys only. |
| B5 | m | `kernel_tuning.sh` rewrote GRUB unconditionally. | Already guarded by writability check; kept with BBR availability fallback. |

## C. Garbage / clutter / stale material removed

| # | Sev | Item | Action |
| :--- | :--- | :--- | :--- |
| C1 | L | `team/` — 25 files of agent-roster theater: prompts for 11 "agents", LLM assignment YAML with provider gossip (`auggie 502`, `theoldllm 403`…), verification JSONs, a "Director: DeepShe Harness" roster. ~476 agent/LLM mentions across the repo. | Deleted. Development tooling is credited once, briefly, in `docs/TEAM.md`. |
| C2 | L | `AUDITOR_VERDICT.json` — a stale audit of problems fixed long ago (referenced files that no longer exist, e.g. `core/build/bot_bin`). | Deleted (git history preserves it). |
| C3 | L | `PROJECT_ANALYSIS.md`, `docs/FINAL_AUDIT.md` — overlapping "audits of audits" with contradictory claims (one said "APPROVED FOR PRODUCTION", withdrawn in the other). | Replaced by a single honest `docs/STATUS.md`. |
| C4 | L | `docs/OPTIMIZATION_LINEAGE.md` — "MutaLambda evolution" history with pending-fiction tables and a reference to an engine that is gitignored/not in the repo. | Deleted; the real optimization record is `docs/PERF_METRICS.md` + this log. |
| C5 | L | `infra/mutalambda/` + `infra/scripts/mutalambda_optimize.py` — fake optimizer output ("AVX-512, 2.14% improvement" was printed by a mock), dead adapter, stale targets JSON. CI job ran it daily. | Deleted, CI job removed. Optimization is now done with real measurements only. |
| C6 | L | `core/crypto/test_signer` — a **compiled ELF binary committed to git**. | Deleted; binaries are build outputs (also `.gitignore`d). |
| C7 | L | `tests/benchmarks/mem_audit.py` — stub that printed "binary not yet compiled" and referenced `./bot_bin`. | Deleted (valgrind command documented in PERF_METRICS). |
| C8 | L | `.gitignore`/`.dockerignore` duplicated and contradictory (listed `core/build/` twice; "stray Keccak test artifacts" as first-class entries). | Rewritten, deduplicated, secrets excluded. |
| C9 | L | `WORKFLOW_MAESTRO_POLYMARKET.md` — the same table pasted twice, "8.0 ns (24 cycles)" claim (measured an empty queue), stale commit pin `5da1443`, contradictions with libsecp256k1 reality. | Replaced by `docs/ARQUITECTURA.es.md` (accurate, in Spanish, no duplicated tables). |
| C10 | m | CMake: `file(GLOB_RECURSE ...)`, hardcoded personal conda path (`/home/adlg/...`) as CURL fallback, "Post-MutaLambda" strings in bench output, `-march=native` forced with no opt-out, targets missing for tests. | Clean CMake: explicit sources, `find_package`/cache-var dependency resolution, `CROWDINTEL_MARCH_NATIVE` option, network layer optional, every test a target. |
| C11 | m | `order_book.hpp` had `#pragma GCC optimize("fast-math")` (no floats!) and misplacement; `Level2Entry` needlessly `packed`. | Rewritten: seqlock for torn-read safety, no packed, no pragmas. |
| C12 | m | `spsc_ring_buffer` used placement-new/destructor dance on slots + `std::optional` copies. | Trivially-copyable memcpy slots, cached indices, full/empty checks without false sharing; 200k-item MT test. |
| C13 | m | README typos: "CMake 3.16+, CMake 3.16+", stale paths, wrong claims about OpenSSL ECDSA. | README rewritten around the V2 reality with measured numbers. |
| C14 | m | Empty directories: none found (git does not track them). Verified with `find . -type d -empty`. | — |

## D. Agent/LLM mention reduction (the "dogs marking territory" problem)

- Before: **511** matches for agent/LLM/provider tokens across the repo; a
  whole top-level `team/` directory; agent names in CI, docs, benchmarks.
- After: **one** file (`docs/TEAM.md`) + one README line. Code, CI, and
  benchmarks contain zero agent references.

## E. What was added (new capability)

- Real CLOB **V2** support end-to-end (domain, struct, wire body, auth, tick/amounts).
- **PresignedOrderPool** — pre-signed taker grid; hot path ~90 ns P50.
- Real WSS market feed (hand-rolled, zero deps) + seqlock book.
- In-house SHA-256/HMAC/base64url with midstate reuse.
- Golden-vector crypto verification + independent Python cross-check.
- Complete test suite (crypto KATs, units, MT SPSC, WSS parser, backtester,
  benchmark) wired into CTest.
- Honest, current documentation (ES/EN).
