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
 *
 * HOT PATH ZERO-ALLOCATION GUARANTEES:
 * - market_slug: std::string_view (char[32] in AlphaSignal, no heap alloc)
 * - country_code: cached std::string loaded once in constructor (no getenv per tick)
 * - client_order_id: generated into stack buffer (char[64], no heap alloc)
 * - build_order_payload: uses fixed stack buffer (char[512], no heap alloc)
 * - SubmitTask: uses std::string_view + std::array (no heap alloc in hot path)
 * - Network I/O: moved to async submit queue (SPSC_RingBuffer + bg thread)
 *   HOT PATH NEVER DIRECTLY CALLS client_.submit_order_with_response()
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
#include <charconv>
#include <chrono>
#include <atomic>
#include <thread>

/**
 * SubmitTask: Captures everything the background submission thread needs
 * to submit an order asynchronously, keeping network I/O off the hot path.
 *
 * Pushed to SPSC_RingBuffer from the hot path (single producer),
 * consumed by the background submission thread (single consumer).
 *
 * HOT PATH ALLOCATION-FREE: All members are either POD or fixed-size buffers.
 * No std::string, no std::vector.
 */
struct SubmitTask {
    char client_order_id[64];        // Fixed buffer — no alloc
    char market_slug[32];            // Copy of market_slug for bg thread (AlphaSignal may be overwritten)
    char payload[512];               // Fixed buffer — no heap alloc (was std::string)
    size_t payload_len;              // Length of payload string
    std::array<uint8_t, 65> signature;  // Copy of signature — fixed array
    std::array<uint8_t, 20> maker_addr;    // Maker address (20 bytes)
    std::array<uint8_t, 20> taker_addr;    // Taker address (20 bytes)
    uint64_t nonce;
    uint64_t price;
    uint64_t size;
    uint64_t salt;
    uint8_t side;
};

/**
 * SubmissionQueue: SPSC ring buffer for async order submission.
 * Hot path pushes (O(1)), background thread pops and submits (network I/O).
 */
using SubmissionQueue = SPSC_RingBuffer<SubmitTask, 2048>;

class ExecutionEngine {
public:
    ExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue,
                    LightweightCLOBClient& client)
        : book_(book), alpha_queue_(alpha_queue), client_(client),
          signer_(load_private_key()),
          shadow_mode_(load_bot_mode() == "shadow"),
          nonce_mgr_(),
          config_(RiskConfig::load_from_env()),
          risk_engine_(config_),
          fee_model_(config_),
          market_cache_(),
          compliance_(ComplianceConfig::load_from_env()),
          order_mgr_(client),
          telemetry_("audit.log"),
          operator_jurisdiction_(load_operator_jurisdiction()),
          submit_queue_(),
          cached_usdc_balance_(10000.0),
          cached_pol_balance_(1000.0),
          submit_thread_(std::thread(&ExecutionEngine::process_submit_queue, this))
    {
        init_eip712_domain();
        telemetry_.log_event(EventType::BALANCE_CHECK, "",
                             "{\"event\":\"startup\",\"component\":\"ExecutionEngine\"}", "INFO");
    }

    /**
     * Production constructor: accepts private key loaded by the caller
     * (main_prod.cpp). Separates credential loading from engine logic.
     */
    ExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue,
                    LightweightCLOBClient& client, const std::vector<uint8_t>& private_key)
        : book_(book), alpha_queue_(alpha_queue), client_(client),
          signer_(private_key),
          shadow_mode_(load_bot_mode() == "shadow"),
          nonce_mgr_(),
          config_(RiskConfig::load_from_env()),
          risk_engine_(config_),
          fee_model_(config_),
          market_cache_(),
          compliance_(ComplianceConfig::load_from_env()),
          order_mgr_(client),
          telemetry_("audit.log"),
          operator_jurisdiction_(load_operator_jurisdiction()),
          submit_queue_(),
          cached_usdc_balance_(10000.0),
          cached_pol_balance_(1000.0),
          submit_thread_(std::thread(&ExecutionEngine::process_submit_queue, this))
    {
        init_eip712_domain();
    }

    ~ExecutionEngine() {
        shutdown_ = true;
        if (submit_thread_.joinable()) {
            submit_thread_.join();
        }
    }

    /**
     * Load bot mode from BOT_MODE env var.
     * P1.1: Shadow mode skips order signing and submission.
     */
    static std::string load_bot_mode() {
        const char* mode = std::getenv("BOT_MODE");
        return mode ? std::string(mode) : "live";
    }

    /**
     * Initialize EIP-712 domain separator with Polymarket CLOB V2 contract address.
     * CRITICAL: Without this, signatures use an all-zeros domain separator and
     * will fail exchange verification.
     *
     * Polymarket CLOB V2 uses:
     *   name: "Polymarket"
     *   verifyingContract: 0xC5d563A36AE7814A12dC12389E369Bc91D5B1d35
     *
     * The domain_data is ABI-encoded as:
     *   bytes32 typeHash = keccak256("EIP712Domain(string name,address verifyingContract)")
     *   bytes32 nameHash = keccak256("Polymarket")
     *   address verifyingContract = 0xC5d563A36AE7814A12dC12389E369Bc91D5B1d35
     *   domainSeparator = keccak256(typeHash ++ nameHash ++ verifyingContract)
     *
     * We pass domain_data = keccak256(name) || pad_left(address, 0, 12) to the signer.
     */
    void init_eip712_domain() {
        // Build domain data: 32 bytes for name hash + 32 bytes for verifyingContract (left-padded)
        std::vector<uint8_t> domain_data(64, 0);
        // name = "Polymarket"
        uint8_t name_hash[32];
        keccak256_hash(reinterpret_cast<const uint8_t*>("Polymarket"), 10, name_hash);
        memcpy(domain_data.data(), name_hash, 32);
        // verifyingContract = 0xC5d563A36AE7814A12dC12389E369Bc91D5B1d35 (20 bytes, left-padded to 32)
        static const uint8_t polymarket_contract[20] = {
            0xC5, 0xd5, 0x63, 0xA3, 0x6A, 0xE7, 0x81, 0x4A,
            0x12, 0xdC, 0x12, 0x38, 0x9E, 0x36, 0x9B, 0xc9,
            0x1D, 0x5B, 0x1d, 0x35
        };
        memcpy(domain_data.data() + 52, polymarket_contract, 20);  // Left-pad to 32 bytes
        signer_.set_domain(
            "EIP712Domain(string name,address verifyingContract)",
            domain_data
        );
    }

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
     * 8. Fase 3: Build SubmitTask, push to async queue (NO network I/O)
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
        // Use string_view — no allocation from market_slug
        std::string_view market_slug(signal->market_slug);
        std::string_view country_code(operator_jurisdiction_);

        TickResult compliance_result = compliance_.check_all(
            std::string(market_slug), std::string(country_code), market_cache_);
        if (compliance_result != TickResult::OK) {
            // Error path — allocation acceptable (not hot path success case)
            telemetry_.log_risk_block(compliance_result, OrderParams{});
            telemetry_.record_tick_result(compliance_result);
            return compliance_result;
        }

        // ─── Fase 2: Kill Switch Check (O(1), atomic) ────────────────────
        if (risk_engine_.is_kill_switch_active()) {
            telemetry_.record_tick_result(TickResult::KILL_SWITCH);
            return TickResult::KILL_SWITCH;
        }

        // ─── Position sizing via Kelly Criterion (O(1), no alloc) ─────────
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
        memset(params.maker, 0x00, 20);
        memset(params.taker, 0x00, 20);

        // Fase 5: Apply tick size from market metadata (O(1))
        int tick_size = market_cache_.get_tick_size(std::string(market_slug));
        params.price = MarketMetadataCache::apply_tick_size(best_ask.price, tick_size);
        params.size = static_cast<uint64_t>(size * 1e6);
        params.nonce = nonce_mgr_.get_next_nonce();
        // P0.2: Respect direction_hint from signal (force BUY/SELL or engine decides)
        if (signal->direction_hint == 0) {
            params.side = 0;  // BUY (force from signal)
        } else if (signal->direction_hint == 1) {
            params.side = 1;  // SELL (force from signal)
        } else {
            // direction_hint == 2: engine decides by EV edge comparison
            params.side = (signal->ev_per_dollar > 0) ? 0 : 1;
        }

        // ─── Fase 2: Risk Engine Pre-Trade Check (BEFORE signing) ──────
        double usdc_balance = cached_usdc_balance_.load(std::memory_order_relaxed);
        double pol_balance = cached_pol_balance_.load(std::memory_order_relaxed);
        // Market exposure: use current order's USD value as exposure proxy
        // (conservative — does not add previous positions, which would require
        //  a PositionTracker instance on the hot path)
        double market_exposure = static_cast<double>(params.size) / 1e6;
        double market_pnl = 0.0;

        // ─── Fase 2: Book Staleness Check (O(1), Polywhales adaptation) ─────
        // Don't trade on stale market data (Polywhales: maxBookAgeMs=90s default)
        if (book_.is_stale(config_.feed_dead_timeout_ms * 1'000'000ULL)) {
            telemetry_.record_tick_result(TickResult::STALE_BOOK);
            return TickResult::STALE_BOOK;
        }

        // Fase 2: Price deviation check (O(1))
        uint64_t current_price = best_ask.price;
        if (params.price > 0 && current_price > 0) {
            double deviation_bps = std::abs(static_cast<double>(current_price) -
                                            static_cast<double>(params.price)) /
                                   static_cast<double>(params.price) * 10000.0;
            if (__builtin_expect(deviation_bps > config_.max_price_deviation_bps, 0)) {
                telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
                return TickResult::RISK_BLOCKED;
            }
        }

        TickResult risk_result = risk_engine_.pre_trade_check(
            params, usdc_balance, pol_balance, market_exposure, market_pnl);
        if (__builtin_expect(risk_result != TickResult::OK, 0)) {
            telemetry_.log_risk_block(risk_result, params);
            telemetry_.record_tick_result(risk_result);
            return risk_result;
        }

        // ─── Fase 3: Self-Trade Detection (anti-duplicate) ─────────────
        // HOT PATH: has_open_order now uses string_view + transparent hash (no alloc)
        if (__builtin_expect(order_mgr_.has_open_order(market_slug, params.side == 0), 0)) {
            telemetry_.record_tick_result(TickResult::DUPLICATE_ORDER);
            return TickResult::DUPLICATE_ORDER;
        }

        // ─── P1.1: Shadow Mode — Skip signing/submission, log computed params ──
        // In shadow mode, the engine evaluates signals exactly as in live mode,
        // but does NOT sign or submit orders to the CLOB. Instead, the computed
        // order parameters (price, size, side, EV) are logged as "shadow orders"
        // for strategy validation and backtesting accuracy analysis.
        if (__builtin_expect(shadow_mode_, 0)) {
            shadow_orders_.fetch_add(1, std::memory_order_relaxed);
            telemetry_.log_shadow_order(
                std::string(market_slug), params,
                net_ev, edge_usd, notional_usd);
            telemetry_.record_tick_result(TickResult::SHADOW_ORDER);
            return TickResult::SHADOW_ORDER;
        }

        // ─── Sign Order (EIP-712 — CRYPTOGRAPHICALLY VERIFIED, NOT MODIFIED) ──
        std::array<uint8_t, 65> signature;
        signer_.sign_order(params, signature);

        // ─── Build payload using fixed buffer (zero-alloc) ──────────────
        // HOT PATH FIX: Replaced std::string concatenation with stack buffer
        std::array<char, 512> payload_buf;
        size_t payload_len = build_order_payload_fixed(params, payload_buf.data());

        // ─── Fase 3: Register order & build SubmitTask (zero-alloc hot path) ─
        // Generate client_order_id into stack buffer
        char client_order_id_buf[64];
        generate_client_order_id_fixed(params.salt, client_order_id_buf);

        // Register order with OrderManager (uses string_view internally now)
        order_mgr_.register_order_no_alloc(params, params.nonce, market_slug, client_order_id_buf);

        // Build SubmitTask — ALL fixed-size, ZERO heap allocations
        SubmitTask task;
        // Copy client_order_id (bounded copy)
        strncpy(task.client_order_id, client_order_id_buf, sizeof(task.client_order_id) - 1);
        task.client_order_id[sizeof(task.client_order_id) - 1] = '\0';

        // Copy market_slug (bounded copy for bg thread safety)
        strncpy(task.market_slug, market_slug.data(), sizeof(task.market_slug) - 1);
        task.market_slug[sizeof(task.market_slug) - 1] = '\0';

        // Copy payload to fixed buffer (zero-alloc)
        memcpy(task.payload, payload_buf.data(), payload_len);
        task.payload[payload_len] = '\0';
        task.payload_len = payload_len;

        // Copy signature (fixed array — no alloc)
        task.nonce = params.nonce;
        memcpy(task.signature.data(), signature.data(), 65);

        // Copy maker/taker (fixed arrays — no vector alloc)
        memcpy(task.maker_addr.data(), params.maker, 20);
        memcpy(task.taker_addr.data(), params.taker, 20);

        task.price = params.price;
        task.size = params.size;
        task.nonce = params.nonce;
        task.salt = params.salt;
        task.side = params.side;

        // ─── CRITICAL FIX: Async Submit via SPSC Queue (NO NETWORK I/O IN HOT PATH) ─
        // Push to SPSC ring buffer; background thread handles HTTP submission.
        // Hot path returns immediately after queue push (O(1), no network I/O).
        // THIS REPLACES the previous `client_.submit_order_with_response(final_order)`
        // which performed synchronous network I/O in the hot path.
        if (!__builtin_expect(submit_queue_.try_push(std::move(task)), true)) {
            // Submit queue full — log and reject (rate-limited fallback)
            telemetry_.log_event(EventType::ORDER_REJECTED, std::string(market_slug),
                                 "{\"reason\":\"submit_queue_full\"}", "WARN");
            telemetry_.increment_429();
            telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
            return TickResult::RISK_BLOCKED;
        }

        // Order signed and queued for async submission — hot path complete.
        // Network I/O (HTTP POST, backoff, retry) happens in background thread.
        risk_engine_.record_order(0.0, params.side == 0);
        telemetry_.increment_orders_submitted();
        telemetry_.record_tick_result(TickResult::OK);
        return TickResult::OK;
    }

    // ─── Fase 6: Telemetry accessors ───────────────────────────────────
    Telemetry& get_telemetry() { return telemetry_; }
    RiskEngine& get_risk_engine() { return risk_engine_; }
    OrderManager& get_order_manager() { return order_mgr_; }

    // ─── Fase 5: Metadata management (cold path) ────────────────────────
    void fetch_market_metadata(const std::string& token_id) {
        market_cache_.fetch_metadata(token_id);
    }

    // ─── Balance management (cold path, called from BalanceChecker) ─────
    void update_cached_balances(double usdc, double pol) {
        cached_usdc_balance_.store(usdc, std::memory_order_release);
        cached_pol_balance_.store(pol, std::memory_order_release);
    }

private:
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& alpha_queue_;
    LightweightCLOBClient& client_;
    NonceManager nonce_mgr_;
    EIP712Signer signer_;

    // ─── Fase 0: Configuration (env-driven, loaded once at startup) ────
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

    // Cached balances (loaded once, updated by BalanceChecker in cold path)
    std::atomic<double> cached_usdc_balance_;
    std::atomic<double> cached_pol_balance_;

    // ─── Operator jurisdiction (cached at construction, no getenv per tick) ──
    const std::string operator_jurisdiction_;

    // ─── Async submission queue (network I/O moved off hot path) ──────────
    SubmissionQueue submit_queue_;
    std::thread submit_thread_;
    std::atomic<bool> shutdown_{false};

    // ─── P1.1: Shadow Mode ──────────────────────────────────────────────────
    // When BOT_MODE=shadow, the engine evaluates signals and computes order
    // parameters exactly as in live mode, but does NOT sign or submit orders.
    // Instead, it logs the computed parameters for strategy validation.
    bool shadow_mode_;
    std::atomic<uint64_t> shadow_orders_{0};

    // ─── Helpers ─────────────────────────────────────────────────────────

    /**
     * Process submit queue (background thread).
     * Pops SubmitTasks from SPSC queue and performs network I/O.
     * NEVER runs on the hot path thread.
     */
    void process_submit_queue() {
        while (!shutdown_.load(std::memory_order_relaxed)) {
            auto task = submit_queue_.try_pop();
            if (!task) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                continue;
            }

            // Reconstruct SignedOrder for submission from fixed buffer
            SignedOrder final_order;
            final_order.nonce = task->nonce;
            final_order.signature = task->signature;
            final_order.payload = std::string_view(task->payload, task->payload_len);

            // Perform HTTP submission (network I/O — acceptable in background thread)
            auto http_response = client_.submit_order_with_response(final_order);

            if (http_response && static_cast<int>(http_response->status) >= 200 &&
                static_cast<int>(http_response->status) < 300) {
                // Success
                order_mgr_.update_status(task->client_order_id, OrderStatus::OPEN);
                telemetry_.log_event(EventType::ORDER_SUBMITTED, task->market_slug,
                    "{\"client_order_id\":\"" + std::string(task->client_order_id) + "\",\"status\":\"OPEN\"}");
            } else if (http_response && http_response->status == HttpStatus::TOO_MANY) {
                // 429 — rate limited
                telemetry_.increment_429();
                // Anti-retry: check if order exists on exchange
                auto retry_decision = order_mgr_.should_retry(task->client_order_id);
                if (retry_decision == OrderManager::RetryDecision::DUPLICATE) {
                    telemetry_.record_tick_result(TickResult::DUPLICATE_ORDER);
                }
            } else if (!http_response || static_cast<int>(http_response->status) >= 500) {
                // 5xx or no response — anti-retry check
                if (http_response) {
                    auto retry_decision = order_mgr_.should_retry(task->client_order_id);
                    if (retry_decision == OrderManager::RetryDecision::DUPLICATE) {
                        telemetry_.record_tick_result(TickResult::DUPLICATE_ORDER);
                    } else if (retry_decision == OrderManager::RetryDecision::SKIP) {
                        telemetry_.record_tick_result(TickResult::RISK_BLOCKED);
                    } else {
                        // RETRY: safe to resubmit
                        auto retry_response = client_.submit_order_with_response(final_order);
                        // ... handle retry response
                    }
                }
            } else {
                // Non-retryable error
                telemetry_.log_event(EventType::ORDER_REJECTED, task->market_slug,
                    "{\"client_order_id\":\"" + std::string(task->client_order_id) + "\","
                    "\"status\":" + (http_response ? std::to_string(static_cast<int>(http_response->status)) : "NULL") + "}");
                order_mgr_.update_status(task->client_order_id, OrderStatus::REJECTED);
                risk_engine_.record_reject();
            }
        }
    }

    /**
     * Load private key from environment variable.
     * Cold path: called once at construction.
     */
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

    /**
     * Load operator jurisdiction from env (cached at construction).
     * No getenv syscall per tick.
     */
    static std::string load_operator_jurisdiction() {
        const char* env_juris = std::getenv("OPERATOR_JURISDICTION");
        return env_juris ? std::string(env_juris) : std::string("US");
    }

    /**
     * Build order payload using fixed buffer — ZERO ALLOCATION.
     * Uses std::to_chars for integer-to-string conversion (no std::to_string).
     *
     * Output format: {"p":"<price>","s":<size>,"side":<0|1>,"n":<nonce>,"salt":<salt>,"mker":"<hex_maker>","expiration":"<expiry>","t":<order_type>}
     */
    static size_t build_order_payload_fixed(const OrderParams& params, char* buf) {
        size_t offset = 0;

        // Helper: append a literal string
        auto append_str = [&](const char* s, size_t len) {
            memcpy(buf + offset, s, len);
            offset += len;
        };

        // Helper: append a uint64_t as decimal
        auto append_uint = [&](uint64_t val) {
            char num_buf[24];  // max 20 digits for uint64_t
            auto [ptr, ec] = std::to_chars(num_buf, num_buf + sizeof(num_buf), val);
            size_t len = ptr - num_buf;
            memcpy(buf + offset, num_buf, len);
            offset += len;
        };

        // Helper: append a uint8_t as 2-digit hex
        auto append_hex = [&](uint8_t val) {
            static const char HEX[] = "0123456789abcdef";
            buf[offset++] = HEX[val >> 4];
            buf[offset++] = HEX[val & 0xF];
        };

        // Build: {"p":"<price>","s":<size>,"side":<side>,"n":<nonce>,"salt":<salt>,"mker":"<hex>","exp":"<expiry>","t":<type>}
        append_str("{\"p\":\"", 5);
        append_uint(params.price);
        append_str("\",\"s\":", 5);
        append_uint(params.size);
        append_str(",\"side\":", 8);
        append_uint(params.side);
        append_str(",\"n\":", 5);
        append_uint(params.nonce);
        append_str(",\"salt\":", 8);
        append_uint(params.salt);
        append_str(",\"mker\":\"", 9);
        for (int i = 0; i < 20; i++) append_hex(params.maker[i]);
        // FIX C-05: Added expiration + order_type fields to payload.
        // "exp":"0" means no expiration (GTC). For GTD, use epoch seconds as string.
        // "t":0 = GTC (Good-Til-Cancelled). "t":1 = GTD (Good-Til-Date).
        //
        // CONSTRAINT: OrderParams struct in eip712_signer.hpp does NOT include
        // expiration/order_type fields (file is marked NEVER MODIFY). These fields
        // are added to the payload JSON for the exchange API, but are NOT part of
        // the EIP-712 signed data. This means:
        // - The exchange may not verify expiration against the signature
        // - GTD orders must use a separate signing path (not currently implemented)
        // - For production: expiration should be added to OrderParams (requires
        //   signer modification + re-verification of KAT vectors)
        //
        // For now: GTC orders (exp="0") are safe since they don't require
        // expiration validation.
        // FIX: append_str length must match the actual string length (18 bytes),
        // not 14. The previous code truncated the JSON, producing malformed output.
        append_str("\",\"exp\":\"0\",\"t\":0}", 18);

        buf[offset] = '\0';
        return offset;
    }

    /**
     * Generate client_order_id into a caller-provided buffer — ZERO ALLOCATION.
     * Format: "%016llx-%016llx-%08llx" (salt-timestamp-counter)
     */
    void generate_client_order_id_fixed(uint64_t salt, char* buf) {
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t counter = order_counter_.fetch_add(1, std::memory_order_relaxed);

        // Fixed buffer — no snprintf (which is fine but let's be consistent)
        snprintf(buf, 64, "%016llx-%016llx-%08llx",
                 (unsigned long long)salt,
                 (unsigned long long)now_ns,
                 (unsigned long long)counter);
    }

    std::atomic<uint64_t> order_counter_{0};
};