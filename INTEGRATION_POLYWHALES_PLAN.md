# Integration Plan: Polywhales Bots Components → CrowdIntel Bot

## Objective
Acoportar los componentes validados de los bots de Polywhales al bot CrowdIntel C++20
para operar en la red Polymarket CLOB V2. Estos componentes ya están probados en
producción (paper trading) y deben ser adaptados conservando las invariantes
de baja latencia del hot path.

## Análisis de Componentes (Polywhales → CrowdIntel)

### 1. Compliance Policy (`apps/web/src/lib/copy-trading/policy.ts`)
**Componentes clave:**
- `CopyStrategy` enum: DIRECTIONAL, MARKET_MAKING, ARBITRAGE, WASH_RISK, SUB_MINUTE_BOT, ONE_SHOT_INSIDER_SHAPED
- `CopyPolicyInput`: subscription limits, state, limits
- Rules: entry band 35-70¢, flat notional, BUY-only hasta shadow ledger validado
- Categorías allowlist + strategy allowlist

**Adaptación a C++:**
- Crear `ComplianceConfig` en `core/include/risk_engine.hpp` (ya existe básico)
- Ampliar con `allowedCategories[]`, `allowedStrategies[]`, `minPriceMicros`, `maxPriceMicros`
- Entry band: 35-70¢ → `RiskConfig.min_order_usd` y `max_order_usd`
- BUY-only hasta validación → flag `copy_mode_paper`
- Reclamar: "Never re-propose disproven strategies" → `PositionTracker` histórico

### 2. Order Sizing & Slippage (`policy.ts` lines 200-230)
**Componentes clave:**
- Sizing modes: FIXED_NOTIONAL, BANKROLL_PPM
- Slippage params: `buySlippageMicros`, `sellSlippageMicros`, `hardSlippageCeilingMicros`
- Slippage = |fillPrice - bookPrice| / bookPrice, capped at `hardSlippageCeilingMicros`
- `maxDepthSharePpm`: No más del X% de la profundidad visible

**Adaptación a C++:**
- `FeeModel::compute_net_ev` already has slippage awareness (lines 203-206)
- Need to add `maxDepthSharePpm` enforcement in `run_tick()`
- Need to add slippage ceiling check post-firm (cold path)

### 3. Book Validation & Ask Walk (`copclob.ts`, `clob-books.ts`)
**Componentes clave:**
- `fetchClobBook(tokenId)`: REST GET `https://clob.polymarket.com/book?token_id={tokenId}`
- `normalizeClobLevels`: parse + sort asks/bids
- `walkAsks(asks, maxEntry, stake)`: VWAP calculation walking asks up to maxEntry
- Stale book detection: `maxBookAgeMs` (90s default)

**Adaptación a C++:**
- `MarketMetadataCache` ya existe para tick sizes
- `OrderBookL2` ya parsea market data — need to add freshness check
- Add `OrderBookL2::is_stale(maxAgeMs)` method
- Add VWAP helper for limit price checks

### 4. Risk Engine Limits (`policy.ts` limits)
**Componentes clave:**
- `maxPositionExposureMicros` / `maxTotalExposureMicros` / `maxEventClusterExposureMicros`
- `maxDailyCopiedNotionalMicros` / `maxDailyLossMicros`
- `maxOrderUnits`
- `strategyTtlMs`: max hold time per strategy

**Adaptación a C++:**
- `RiskEngine` ya tiene `max_position_exposure`, `max_daily_loss_usd`
- Need to add: `max_total_exposure`, `max_event_cluster_exposure`, `max_daily_notional_usd`
- Need to add: `max_order_units` (en `RiskConfig`)

### 5. Evidence & Gate Reports (`evidence.ts`)
**Componentes clave:**
- `PaperEvidenceRecord`: atMs, sourceWallet, category, latencyMs, liquidity, status, reasonCode, fillRatioPpm, slippageMicros, edgeCaptureMicros
- `buildFourWeekGateReport`: min 28 days runtime, min 20 eligible intents, fill ratio ≥ 90%, p95 slippage ≤ 10bps

**Adaptación a C++:**
- `Telemetry` ya loguea eventos — extend with evidence tracking
- Add `Telemetry::record_evidence()` for paper-trading P&L
- Gate report → CSV export for offline analysis

### 6. Guard & State Management (`guard.ts`, `store.ts`)
**Componentes clave:**
- Account/subscription pause checks
- Rules version staleness detection
- Transaction-locked intent lifecycle

**Adaptación a C++:**
- `RiskEngine::is_kill_switch_active()` ya existe
- Need to add: `operator_jurisdiction_` checks (compliance with sanctions lists)
- Intent lifecycle → `OrderManager` state machine (already exists)

## Implementation Priority

### Phase A: Critical (Hot Path)
| Component | File | Priority |
|---|---|---|
| Entry band check (35-70¢) | `risk_engine.hpp::pre_trade_check` | 🔴 CRITICAL |
| Exposure limits | `risk_engine.hpp` | 🔴 CRITICAL |
| Kill switch before signing | `execution_engine.cpp::run_tick` | 🔴 CRITICAL (DONE) |
| Book staleness check | `order_book.hpp` | 🟡 IMPORTANT |
| Depth share cap (maxDepthSharePpm) | `execution_engine.cpp::run_tick` | 🟡 IMPORTANT |

### Phase B: Cold Path
| Component | File | Priority |
|---|---|---|
| Evidence logging | `telemetry.hpp` | 🟡 IMPORTANT |
| Slippage ceiling check | `process_submit_queue` | 🟡 IMPORTANT |
| Daily notional cap | `risk_engine.hpp` | 🟢 NICE-TO-HAVE |
| Strategy TTL | `position_tracker.hpp` | 🟢 NICE-TO-HAVE |
| Category allowlist | `compliance_config.hpp` | 🟢 NICE-TO-HAVE |

## Polymarket CLOB V2 API Reference

| Function | Endpoint |
|---|---|
| Get order book | `GET https://clob.polymarket.com/book?token_id={token_id}` |
| Submit order | `POST https://clob.polymarket.com/order` | 
| Cancel order | `POST https://clob.polymarket.com/cancel` |
| Get market | `GET https://clob.polymarket.com/market?token_id={token_id}` |
| WebSocket market | `wss://ws-subscriptions-clob.polymarket.com/ws/market` |
| WebSocket user | `wss://ws-subscriptions-clob.polymarket.com/ws/user` |

Auth: HMAC-SHA256(timestamp + method + path + body), Base64 encode, header: `Authorization: <key>:<sig>:<timestamp>`

Rate limits: 200 requests/min per API key.

## Files to Modify

| File | Change |
|---|---|
| `core/include/risk_engine.hpp` | Add entry band, exposure caps, daily notional |
| `core/src/execution_engine.cpp` | Add depth share cap, strategy TTL check |
| `core/include/order_book.hpp` | Add staleness check + VWAP helper |
| `core/src/telemetry.hpp` | Add evidence record + gate report |
| `core/include/position_tracker.hpp` | Add strategy TTL + position exposure tracking |
| `core/src/compliance_config.hpp` | New file: category/strategy allowlist |

## Files to Create

| File | Purpose |
|---|---|
| `core/include/compliance_config.hpp` | Category/strategy allowlist (adapted from Polywhales policy.ts) |
| `core/src/evidence_logger.hpp` | Paper-trading evidence CSV logger (adapted from evidence.ts) |
| `core/crypto/polywhales_compat.hpp` | Compatibility layer for Polywhales paper-trading semantics |
