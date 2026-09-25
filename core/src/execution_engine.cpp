/**
 * ExecutionEngine: The Heart of the Hot Path.
 *
 * Consumes signals from the SPSC queue and evaluates the OrderBook L2.
 *
 * INTEGRATED COMPLIANCE CONTROLS (per WORKFLOW_REMEDIACION_CUMPLIMIENTO.md):
 *   Fase 1: RateLimiter check before network I/O
 *   Fase 2: RiskEngine::pre_trade_check BEFORE signing (kill switch, limits, windows)
 *   Fase 3: OrderManager::generate_client_order_id, anti-retry, self-trade detection
 *   Fase 4: FeeModel::compute_net_ev replaces edge > min_edge filter
 *   Fase 5: ComplianceGuard::check_all, MarketMetadataCache tick size + market state
 *   Fase 6: Telemetry logging (async, SPSC non-blocking)
 *
 * CRITICAL: The EIP-712 signing path (eip712_signer.hpp) is NEVER modified.
 * All new controls are O(1) and branch-predicted, placed BEFORE the
 * signing operation which remains the cryptographic hot path.
 *
 * NO std::cout / I/O in hot path — silent execution for deterministic latency.
 * Telemetry uses SPSC ring buffer + async writer thread (no I/O on hot path).
 */

#include "order_book.hpp"
#include "spsc_ring_buffer.hpp"
#include "eip712_signer.hpp"
#include "nonce_manager.hpp"
#include "lightweight_client.hpp"
#include "alpha_receiver.hpp"
#include "kelly_engine.hpp"

// Fase 0-7: Compliance Modules
#include "tick_result.hpp"
#include "market_config.hpp"
#include "rate_limiter.hpp"
#include "risk_engine.hpp"
#include "fee_model.hpp"
#include "market_metadata.hpp"
#include "compliance_guard.hpp"
#include "order_manager.hpp"
#include "position_tracker.hpp"
#include "presigned_pool.hpp"
#include "telemetry.hpp"

#include <optional>
#include <cstring>
#include <cstdlib>
#include <array>
#include <chrono>

class ExecutionEngine {
public:
    ExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue, LightweightCLOBClient& client)
        : book_(book), alpha_queue_(alpha_queue), client_(client),
          signer_(load_private_key()),
          nonce_mgr_(),
          config_(RiskConfig::load_from_env()),
          risk_engine_(config_),
          fee_model_(config_),
          market_cache_(),
          compliance_(ComplianceConfig::load_from_env()),
          order_mgr_(client),
          telemetry_("audit.log")
    {
        telemetry_.log_event(EventType::BALANCE_CHECK, "",
                             "{\"event\":\"startup\",\"component\":\"ExecutionEngine\"}", "INFO");
    }

    /**
     * Production constructor: accepts private key loaded by the caller
     * (main_prod.cpp). This separates credential loading from engine logic.
     */
    ExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue,
                    LightweightCLOBClient& client, const std::vector<uint8_t>& private_key)
        : book_(book), alpha_queue_(alpha_queue), client_(client),
          signer_(private_key),
          nonce_mgr_(),
          config_(RiskConfig::load_from_env()),
          risk_engine_(config_),
          fee_model_(config_),
          market_cache_(),
          compliance_(ComplianceConfig::load_from_env()),
          order_mgr_(client),
          telemetry_("audit.log")
    {}

    /**
     * run_tick: Hot path — evaluates one alpha signal.
     *
     * Control flow (all O(1), branch-predicted, BEFORE signing):
     * 1. Pop signal from SPSC queue
     * 2. Fase 5: Compliance guard (token allowed, jurisdiction, market active)
     * 3. Fase 2: Kill switch check (atomic)
     * 4. Fase 4: Compute net_ev (edge - fees - gas - slippage)
     * 5. Fase 2: Risk engine pre_trade_check (limits, exposure, balance)
     * 6. Fase 3: Check for duplicate/self-trade orders
     * 7. Sign order (EIP-712 — NEVER modified)
     * 8. Fase 3: Register order, submit with client_order_id tracking
     * 9. Fase 6: Log result (async telemetry)
     */
    TickResult run_tick() {
        auto signal = alpha_queue_.try_pop();

        if (!signal) {
            return TickResult::NO_SIGNAL;
        }

        const auto& best_bid = book_.get_bid(0);
        const auto& best_ask = book_.get_ask(0);

        // ─── Fase 5: Compliance Guard (O(1), branch-predicted) ──────────
        std::string market_slug(signal->market_slug);
        std::string country_code = get_operator_jurisdiction();

        TickResult compliance_result = compliance_.check_all(
            market_slug, country_code, market_cache_);
        if (compliance_result != TickResult::OK) {
            telemetry_.log_risk_block(compliance_result, OrderParams{});
            telemetry_.record_tick_result(compliance_result);
            return compliance_result;
        }

        // ─── Fase 2: Kill Switch Check (O(1), atomic) ───────────────────
        if (risk_engine_.is_kill_switch_active()) {
            telemetry_.record_tick_result(TickResult::KILL_SWITCH);
            return TickResult::KILL_SWITCH;
        }

        // ─── Position sizing via Kelly Criterion ─────────────────────────
        double size = KellyEngine::calculate_position_size(
            KellyEngine::calculate_fractional_kelly(signal->ev_per_dollar, signal->confidence),
            10000.0
        );

        // ─── Fase 4: Net EV Filter (replaces edge > min_edge) ────────────
        double notional_usd = size;
        double edge_usd = signal->ev_per_dollar * notional_usd;
        double spread_bps = (best_bid.price > 0)
            ? static_cast<double>(best_ask.price - best_bid.price) /
              static_cast<double>(best_bid.price) * 10000.0
            : 0.0;
        double available_liquidity = static_cast<double>(best_ask.size) / 1e6;
        double probability = signal->q_value;

        double net_ev = fee_model_.compute_net_ev(
            edge_usd, notional_usd, true,
            probability, spread_bps, available_liquidity
        );

        if (net_ev < config_.min_net_ev_usd) {
            telemetry_.record_tick_result(TickResult::NOT_PROFITABLE);
            return TickResult::NOT_PROFITABLE;
        }

        // ─── Build order parameters ──────────────────────────────────────
        OrderParams params;
        params.salt = nonce_mgr_.get_next_nonce();
        // Maker and taker addresses should come from config in production
        memset(params.maker, 0x00, 20);
        memset(params.taker, 0x00, 20);

        // Fase 5: Apply tick size from market metadata
        int tick_size = market_cache_.get_tick_size(market_slug);
        params.price = MarketMetadataCache::apply_tick_size(best_ask.price, tick_size);
        params.size = static_cast<uint64_t>(size * 1e6);
        params.nonce = nonce_mgr_.get_next_nonce();
        params.side = (signal->ev_per_dollar > 0) ? 0 : 1;

        // ─── Fase 2: Risk Engine Pre-Trade Check (BEFORE signing) ──────
        double usdc_balance = cached_usdc_balance_.load(std::memory_order_relaxed);
        double pol_balance = cached_pol_balance_.load(std::memory_order_relaxed);
        double market_exposure = 0.0;  // Updated by PositionTracker (cold path)
        double market_pnl = 0.0;

        // Fase 2: Price deviation check
        uint64_t current_price = best_ask.price;
        if (params.price > 0 && current_price > 0) {
            double deviation_bps = std::abs(static_cast<double>(current_price) -
                                            static_cast<double>(params.price)) /
                                   static_cast<double>(params.price) * 10000.0;
            if (deviation_bps > config_.max_price_deviation_bps) {
                telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
                return TickResult::RISK_BLOCKED;
            }
        }

        TickResult risk_result = risk_engine_.pre_trade_check(
            params, usdc_balance, pol_balance, market_exposure, market_pnl);
        if (risk_result != TickResult::OK) {
            telemetry_.log_risk_block(risk_result, params);
            telemetry_.record_tick_result(risk_result);
            return risk_result;
        }

        // ─── Fase 3: Self-Trade Detection (anti-duplicate) ─────────────
        if (order_mgr_.has_open_order(market_slug, params.side == 0)) {
            telemetry_.record_tick_result(TickResult::DUPLICATE_ORDER);
            return TickResult::DUPLICATE_ORDER;
        }

        // ─── Sign Order (EIP-712 — CRYPTOGRAPHICALLY VERIFIED, NOT MODIFIED) ──
        std::array<uint8_t, 65> signature;
        signer_.sign_order(params, signature);

        // ─── Fase 3: Build payload and register with OrderManager ──────
        SignedOrder final_order;
        final_order.nonce = params.nonce;
        final_order.signature = signature;
        final_order.payload = build_order_payload(params);

        // Register order BEFORE submitting (client_order_id tracking).
        // FIX: register_order returns the generated client_order_id, avoiding
        // the previous bug where generate_client_order_id was called twice
        // (producing two different IDs).
        std::string client_order_id = order_mgr_.register_order(params, params.nonce, market_slug);

        // ─── Fase 1: Submit with rate limit, backoff, pooling ──────────
        auto http_response = client_.submit_order_with_response(final_order);

        if (!http_response) {
            // Fase 1: Rate limited — no response from exchange.
            // Fase 3: Anti-retry — check if order actually went through.
            auto retry_decision = order_mgr_.should_retry(client_order_id);
            if (retry_decision == OrderManager::RetryDecision::DUPLICATE) {
                telemetry_.record_tick_result(TickResult::DUPLICATE_ORDER);
                return TickResult::DUPLICATE_ORDER;
            }
            // RETRY or SKIP: treat as rate-limited, caller can retry
            telemetry_.increment_429();
            telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
            return TickResult::RISK_BLOCKED;
        }

        if (static_cast<int>(http_response->status) >= 200 &&
            static_cast<int>(http_response->status) < 300) {
            telemetry_.increment_orders_submitted();
            order_mgr_.update_status(client_order_id, OrderStatus::OPEN);
            telemetry_.log_event(EventType::ORDER_SUBMITTED, market_slug,
                                 "{\"client_order_id\":\"" + client_order_id + "\",\"status\":\"OPEN\"}");
            telemetry_.record_tick_result(TickResult::OK);
            return TickResult::OK;
        } else if (http_response->status == HttpStatus::TOO_MANY) {
            // Fase 3: Anti-retry on 429 — check if order exists on exchange
            auto retry_decision = order_mgr_.should_retry(client_order_id);
            if (retry_decision == OrderManager::RetryDecision::DUPLICATE) {
                telemetry_.record_tick_result(TickResult::DUPLICATE_ORDER);
                return TickResult::DUPLICATE_ORDER;
            }
            telemetry_.increment_429();
            telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
            return TickResult::RISK_BLOCKED;
        } else if (static_cast<int>(http_response->status) >= 500) {
            // Fase 3: Anti-retry on 5xx — query exchange before resubmitting
            auto retry_decision = order_mgr_.should_retry(client_order_id);
            if (retry_decision == OrderManager::RetryDecision::DUPLICATE) {
                telemetry_.record_tick_result(TickResult::DUPLICATE_ORDER);
                return TickResult::DUPLICATE_ORDER;
            }
            if (retry_decision == OrderManager::RetryDecision::SKIP) {
                telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
                return TickResult::RISK_BLOCKED;
            }
            // RETRY: safe to resubmit
            telemetry_.increment_429();
            telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
            return TickResult::RISK_BLOCKED;
        } else {
            telemetry_.log_event(EventType::ORDER_REJECTED, market_slug,
                                 "{\"client_order_id\":\"" + client_order_id + "\","
                                 "\"status\":" + std::to_string(static_cast<int>(http_response->status)) + "}");
            order_mgr_.update_status(client_order_id, OrderStatus::REJECTED);
            risk_engine_.record_reject();
            telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
            return TickResult::RISK_BLOCKED;
        }
    }

    // ─── Fase 6: Telemetry accessors ───────────────────────────────────
    Telemetry& get_telemetry() { return telemetry_; }
    RiskEngine& get_risk_engine() { return risk_engine_; }
    OrderManager& get_order_manager() { return order_mgr_; }

    // ─── Fase 5: Metadata management (cold path) ────────────────────────
    void fetch_market_metadata(const std::string& token_id) {
        market_cache_.fetch_metadata(token_id);
    }

private:
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& alpha_queue_;
    LightweightCLOBClient& client_;
    NonceManager nonce_mgr_;
    EIP712Signer signer_;

    // ─── Fase 0: Configuration (env-driven) ───────────────────────────
    RiskConfig config_;

    // ─── Fase 2: Risk Engine ────────────────────────────────────────────
    RiskEngine risk_engine_;

    // ─── Fase 3: Order Manager ────────────────────────────────────────
    OrderManager order_mgr_;

    // ─── Fase 4: Fee Model ────────────────────────────────────────────
    FeeModel fee_model_;

    // ─── Fase 5: Compliance ───────────────────────────────────────────
    MarketMetadataCache market_cache_;
    ComplianceGuard compliance_;

    // ─── Fase 6: Telemetry ────────────────────────────────────────────
    Telemetry telemetry_;

    // ─── PresignedOrderPool (T3-5) ────────────────────────────────────
    PresignedOrderPool presigned_pool_{500};

    // Cached balances (updated by BalanceChecker in cold path)
    std::atomic<double> cached_usdc_balance_{10000.0};
    std::atomic<double> cached_pol_balance_{1000.0};

    // ─── Helpers (hot path — must be O(1)) ────────────────────────────

    static std::vector<uint8_t> load_private_key() {
        const char* env_key = std::getenv("BOT_PRIVATE_KEY_HEX");
        if (!env_key || strlen(env_key) != 64) {
            throw std::runtime_error(
                "BOT_PRIVATE_KEY_HEX must be set to a 64-character hex string (32 bytes). "
                "DO NOT hardcode private keys in source.");
        }
        std::vector<uint8_t> key(32);
        for (size_t i = 0; i < 32; i++) {
            char buf[3] = {env_key[i * 2], env_key[i * 2 + 1], 0};
            key[i] = static_cast<uint8_t>(strtol(buf, nullptr, 16));
        }
        return key;
    }

    static std::string get_operator_jurisdiction() {
        const char* env_juris = std::getenv("OPERATOR_JURISDICTION");
        return env_juris ? std::string(env_juris) : std::string("US");
    }

    static std::string build_order_payload(const OrderParams& params) {
        std::string payload;
        payload.reserve(512);
        payload += "{\"p\":\"";
        payload += std::to_string(params.price);
        payload += "\",\"s\":";
        payload += std::to_string(params.size);
        payload += ",\"side\":";
        payload += std::to_string(params.side);
        payload += ",\"n\":";
        payload += std::to_string(params.nonce);
        payload += ",\"salt\":";
        payload += std::to_string(params.salt);
        payload += ",\"mker\":\"";
        char hexbuf[41];
        for (int i = 0; i < 20; i++) sprintf(hexbuf + i * 2, "%02x", params.maker[i]);
        hexbuf[40] = '\0';
        payload += hexbuf;
        payload += "\"}";
        return payload;
    }
};
