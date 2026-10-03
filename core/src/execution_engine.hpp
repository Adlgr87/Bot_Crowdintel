#ifndef EXECUTION_ENGINE_HPP
#define EXECUTION_ENGINE_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "../crypto/eip712_signer.hpp"
#include "../crypto/fast_random.hpp"
#include "../include/order_book.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "alpha_receiver.hpp"
#include "kelly_engine.hpp"
#include "market_config.hpp"
#include "order_ledger.hpp"
#include "polymarket_order.hpp"
#include "presigned_pool.hpp"

enum class TickResult : uint8_t {
    NO_SIGNAL = 0,
    FILTERED_STATS,
    WRONG_MARKET,
    STALE_SIGNAL,
    DUPLICATE_SIGNAL,
    NO_BOOK,
    NO_EDGE,
    NO_INVENTORY,
    RISK_REJECTED,
    RECONCILE_REQUIRED,
    TOO_SMALL,
    SIGN_FAILED,
    BODY_FAILED,
    SUBMIT_FAILED,
    QUEUED,
    SUBMITTED,
    COUNT
};

inline const char* tick_result_name(TickResult result) {
    switch (result) {
        case TickResult::NO_SIGNAL: return "no_signal";
        case TickResult::FILTERED_STATS: return "filtered_stats";
        case TickResult::WRONG_MARKET: return "wrong_market";
        case TickResult::STALE_SIGNAL: return "stale_signal";
        case TickResult::DUPLICATE_SIGNAL: return "duplicate_signal";
        case TickResult::NO_BOOK: return "no_book";
        case TickResult::NO_EDGE: return "no_edge";
        case TickResult::NO_INVENTORY: return "no_inventory";
        case TickResult::RISK_REJECTED: return "risk_rejected";
        case TickResult::RECONCILE_REQUIRED: return "reconcile_required";
        case TickResult::TOO_SMALL: return "too_small";
        case TickResult::SIGN_FAILED: return "sign_failed";
        case TickResult::BODY_FAILED: return "body_failed";
        case TickResult::SUBMIT_FAILED: return "submit_failed";
        case TickResult::QUEUED: return "queued";
        case TickResult::SUBMITTED: return "submitted";
        case TickResult::COUNT: break;
    }
    return "unknown";
}

template <typename Client>
class ExecutionEngine {
public:
    ExecutionEngine(const MarketConfig& cfg,
                    OrderBookL2& book,
                    SPSC_RingBuffer<AlphaSignal>& signals,
                    const EIP712Signer& signer,
                    PresignedOrderPool& pool,
                    Client& client,
                    const std::atomic<bool>* trading_enabled = nullptr)
        : cfg_(cfg), book_(book), signals_(signals), signer_(signer),
          pool_(pool), client_(client), trading_enabled_(trading_enabled),
          confirmed_inventory_(cfg.initial_position_shares) {}

    // Optional Phase-2 journal. When attached, every intent is durable before
    // egress and any order that is not proven terminal blocks trading.
    void attach_ledger(cledger::OrderLedger* ledger) noexcept { ledger_ = ledger; }
    cledger::OrderLedger* ledger() const noexcept { return ledger_; }

    TickResult run_tick() {
        AlphaSignal signal{};
        if (!signals_.try_pop(signal)) return TickResult::NO_SIGNAL;

        if (!std::isfinite(signal.p_win) || !std::isfinite(signal.confidence) ||
            !std::isfinite(signal.q_value) ||
            !(signal.p_win > 0.0 && signal.p_win < 1.0) ||
            signal.confidence < cfg_.min_confidence ||
            signal.q_value > cfg_.max_q_value || signal.direction_hint > 2)
            return TickResult::FILTERED_STATS;
        if (signal.market_hash != cfg_.market_hash)
            return TickResult::WRONG_MARKET;

        const uint64_t now_ns = realtime_ns();
        const uint64_t ttl_ns = cfg_.signal_ttl_ms * 1000000ULL;
        constexpr uint64_t MAX_FUTURE_SKEW_NS = 1000000000ULL;
        if (signal.timestamp_ns == 0 ||
            (signal.timestamp_ns > now_ns &&
             signal.timestamp_ns - now_ns > MAX_FUTURE_SKEW_NS) ||
            (now_ns > signal.timestamp_ns && now_ns - signal.timestamp_ns > ttl_ns))
            return TickResult::STALE_SIGNAL;
        if (seen_before(signal.signal_id, now_ns, ttl_ns))
            return TickResult::DUPLICATE_SIGNAL;
        if (trading_enabled_ && !trading_enabled_->load(std::memory_order_acquire))
            return TickResult::RISK_REJECTED;
        // Live mode trades only with venue-resolved metadata (Fase 1): no
        // hardcoded tick/min-size/fee may substitute a venue value.
        if (!cfg_.trading_parameters_ready()) return TickResult::RISK_REJECTED;
        // A journaled order without a proven venue outcome (UNKNOWN, sent but
        // unconfirmed, partially filled) stops trading until reconciliation.
        if (ledger_ && !ledger_->gate_open()) {
            ++ledger_blocks_;
            return TickResult::RECONCILE_REQUIRED;
        }

        OrderBookL2::Top top{};
        bool have_book = false;
        const uint64_t max_age_ns = cfg_.max_book_age_ms * 1000000ULL;
        for (int attempt = 0; attempt < 4 && !have_book; ++attempt)
            have_book = book_.read_top(top, max_age_ns);
        if (!have_book || top.bid.size == 0 || top.ask.size == 0 ||
            top.bid.price >= top.ask.price)
            return TickResult::NO_BOOK;

        uint8_t side = K_SIDE_BUY;
        if (signal.direction_hint == 0) {
            side = K_SIDE_BUY;
        } else if (signal.direction_hint == 1) {
            side = K_SIDE_SELL;
        } else {
            const double buy_edge = net_edge(K_SIDE_BUY, signal.p_win,
                                             static_cast<double>(top.ask.price) * 1e-6);
            const double sell_edge = net_edge(K_SIDE_SELL, signal.p_win,
                                              static_cast<double>(top.bid.price) * 1e-6);
            side = buy_edge >= sell_edge ? K_SIDE_BUY : K_SIDE_SELL;
        }

        if (side == K_SIDE_SELL && confirmed_inventory_ < cfg_.effective_min_size())
            return TickResult::NO_INVENTORY;

        const uint64_t tick = book_.tick_size(cfg_.effective_tick());
        if (tick == 0 || amount_quantum_for_tick(tick) == 0)
            return TickResult::NO_BOOK;  // unknown venue grid -> fail closed
        // The venue may publish a tick change for this market at any time.
        // The resolved snapshot (tick, min size, fee curve) belongs to the grid
        // it was resolved for, so a new grid stops trading until the metadata
        // is resolved again (Fase 5 automates the re-resolution).
        if (cfg_.live_transport() && cfg_.runtime.resolved &&
            tick != cfg_.runtime.tick_size)
            return TickResult::RISK_REJECTED;
        const uint64_t raw_book_price = side == K_SIDE_BUY
                                      ? top.ask.price : top.bid.price;
        const uint64_t price_raw = round_price_to_tick(raw_book_price, tick);
        const double price = static_cast<double>(price_raw) * 1e-6;
        const double edge = net_edge(side, signal.p_win, price);
        if (edge < cfg_.min_edge) return TickResult::NO_EDGE;

        // Size against the fee-adjusted execution price.  This is conservative:
        // fees reduce both the gate and the Kelly fraction. The coefficient and
        // exponent come from the resolved venue fee curve (p*(1-p))^e.
        const double fee_per_share = cfg_.fee_per_share(price);
        const double sizing_price = side == K_SIDE_BUY
            ? std::min(0.999999, price + fee_per_share)
            : std::max(0.000001, price - fee_per_share);
        const double kelly = side == K_SIDE_BUY
            ? KellyEngine::kelly_buy(signal.p_win, sizing_price)
            : KellyEngine::kelly_sell(signal.p_win, sizing_price);
        double usd = KellyEngine::position_usd(
            kelly, cfg_.kelly_fraction, cfg_.bankroll_usd);
        double available_budget = cfg_.max_order_usd;
        if (side == K_SIDE_BUY) {
            available_budget = std::min(
                available_budget,
                std::max(0.0, cfg_.max_exposure_usd - committed_exposure_usd_));
            available_budget = std::min(
                available_budget,
                std::max(0.0, cfg_.max_daily_loss_usd - worst_case_loss_usd_));
        }
        usd = std::min(usd, available_budget);
        uint64_t requested_shares = KellyEngine::usd_to_shares_fixed(usd, price);

        const uint64_t visible = side == K_SIDE_BUY ? top.ask.size : top.bid.size;
        requested_shares = std::min(requested_shares, visible);
        if (side == K_SIDE_SELL)
            requested_shares = std::min(requested_shares, confirmed_inventory_);
        if (requested_shares < cfg_.effective_min_size())
            return TickResult::TOO_SMALL;

        const bool market_order = std::strcmp(cfg_.order_type, "FAK") == 0 ||
                                  std::strcmp(cfg_.order_type, "FOK") == 0;
        uint64_t maker_amount = 0, taker_amount = 0, effective_shares = 0;
        if (!compute_order_amounts(side, price_raw, requested_shares, tick,
                                   market_order, maker_amount, taker_amount,
                                   effective_shares) ||
            effective_shares < cfg_.effective_min_size())
            return TickResult::TOO_SMALL;

        // Local circuit breaker is conservative until user-channel fill
        // reconciliation lands: accepted BUYs reserve their full worst-case
        // cost and are not credited as sellable inventory.
        const double order_notional = static_cast<double>(
            side == K_SIDE_BUY ? maker_amount : taker_amount) * 1e-6;
        if (order_notional > cfg_.max_order_usd + 1e-9 ||
            (side == K_SIDE_BUY &&
             (committed_exposure_usd_ + order_notional > cfg_.max_exposure_usd ||
              worst_case_loss_usd_ + order_notional > cfg_.max_daily_loss_usd)))
            return TickResult::RISK_REJECTED;

        WireBody body{};
        uint64_t pool_size = 0, pool_maker = 0, pool_taker = 0;
        bool presigned = pool_.acquire_at_most(
            side, price_raw, tick, effective_shares, body,
            pool_size, pool_maker, pool_taker);
        if (presigned) {
            effective_shares = pool_size;
            maker_amount = pool_maker;
            taker_amount = pool_taker;
        } else {
            OrderV2 order{};
            fill_order(order, side, maker_amount, taker_amount);
            uint8_t signature[65];
            if (!signer_.sign_order(order, signature))
                return TickResult::SIGN_FAILED;
            const uint64_t expiration = cfg_.wire_expiration(order.timestamp_ms / 1000ULL);
            if (!build_wire_body(order, signature, cfg_.effective_token_id_dec(),
                                 cfg_.maker_hex, cfg_.signer_hex,
                                 cfg_.owner_api_key, cfg_.order_type, body,
                                 expiration))
                return TickResult::BODY_FAILED;
        }

        char client_order_id[cledger::kClientOrderIdChars];
        cledger::make_client_order_id(cfg_.market_hash, signal.signal_id,
                                      client_order_id);
        if (ledger_) {
            cledger::IntentRecord intent{};
            std::snprintf(intent.client_order_id, sizeof(intent.client_order_id),
                          "%s", client_order_id);
            intent.signal_id = signal.signal_id;
            intent.market_hash = cfg_.market_hash;
            intent.price_fixed6 = price_raw;
            intent.shares_fixed6 = effective_shares;
            intent.notional_fixed6 = static_cast<uint64_t>(
                accepted_notional_usd(side, maker_amount, taker_amount) * 1e6);
            intent.side = side;
            intent.order_type = static_cast<uint8_t>(
                std::strcmp(cfg_.order_type, "FOK") == 0 ? 2 : 1);
            char ledger_error[96];
            if (!ledger_->record_intent(intent, ledger_error,
                                        sizeof(ledger_error))) {
                std::snprintf(ledger_error_, sizeof(ledger_error_), "%s",
                              ledger_error);
                return TickResult::RECONCILE_REQUIRED;
            }
        }

        const SubmitResult response = client_.submit(body);
        if (ledger_) {
            char ledger_error[96];
            const bool ambiguous = !response.ok && response.http_code == 0;
            const bool recorded = apply_ledger_response(
                client_order_id, response, ambiguous, ledger_error,
                sizeof(ledger_error));
            if (!recorded) {
                std::snprintf(ledger_error_, sizeof(ledger_error_), "%s",
                              ledger_error);
            }
        }
        if (!response.ok) {
            ++submit_failed_;
            return TickResult::SUBMIT_FAILED;
        }

        const double accepted_notional = static_cast<double>(
            side == K_SIDE_BUY ? maker_amount : taker_amount) * 1e-6;
        if (side == K_SIDE_BUY) {
            committed_exposure_usd_ += accepted_notional;
            worst_case_loss_usd_ += accepted_notional;
        } else {
            // Reserve as if fully filled; never permit two sells against the
            // same confirmed inventory while fills are not reconciled.
            confirmed_inventory_ = effective_shares >= confirmed_inventory_
                ? 0 : confirmed_inventory_ - effective_shares;
        }
        if (!response.final) {
            ++queued_;
            return TickResult::QUEUED;
        }
        ++submitted_;
        return TickResult::SUBMITTED;
    }

    uint64_t ledger_blocks() const noexcept { return ledger_blocks_; }
    const char* ledger_error() const noexcept { return ledger_error_; }
    uint64_t submitted() const noexcept { return submitted_; }
    uint64_t queued() const noexcept { return queued_; }
    uint64_t submit_failed() const noexcept { return submit_failed_; }
    uint64_t confirmed_inventory() const noexcept { return confirmed_inventory_; }
    double committed_exposure_usd() const noexcept { return committed_exposure_usd_; }

private:
    static uint64_t realtime_ns() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    static double accepted_notional_usd(uint8_t side, uint64_t maker_amount,
                                        uint64_t taker_amount) noexcept {
        return static_cast<double>(side == K_SIDE_BUY ? maker_amount
                                                      : taker_amount) * 1e-6;
    }

    // Journals the venue response. A transport failure/timeout has no venue
    // evidence (http_code == 0) and therefore leaves the order UNKNOWN; a
    // definitive HTTP error is a venue rejection.
    bool apply_ledger_response(const char* client_order_id,
                               const SubmitResult& response, bool ambiguous,
                               char* error, size_t error_cap) noexcept {
        cledger::LedgerEvent event = cledger::LedgerEvent::kSubmitAck;
        cledger::Evidence evidence = cledger::Evidence::kVenueAck;
        if (ambiguous) {
            event = cledger::LedgerEvent::kSubmitAmbiguous;
            evidence = cledger::Evidence::kNone;
        } else if (!response.ok) {
            event = cledger::LedgerEvent::kSubmitRejected;
        }
        const cledger::Transition step = ledger_->record_transition(
            client_order_id, event, evidence, response.order_id,
            /*filled_fixed6=*/0, error, error_cap);
        if (step.result == cledger::TransitionResult::kIllegal ||
            step.result == cledger::TransitionResult::kNeedsReconcile)
            return false;
        return true;
    }

    double net_edge(uint8_t side, double p_win, double price) const noexcept {
        const double fee = cfg_.fee_per_share(price);
        return side == K_SIDE_BUY ? p_win - price - fee
                                  : price - p_win - fee;
    }

    static uint64_t dedupe_hash(uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    static bool bloom_contains(
            const std::array<uint64_t, 65536>& filter,
            const std::array<size_t, 3>& positions) noexcept {
        for (const size_t position : positions)
            if ((filter[position >> 6] & (1ULL << (position & 63U))) == 0)
                return false;
        return true;
    }

    bool seen_before(uint64_t id, uint64_t now_ns,
                     uint64_t ttl_ns) noexcept {
        if (id == 0 || ttl_ns == 0) return true;
        const uint64_t epoch = now_ns / ttl_ns;
        if (dedupe_epoch_ == 0) {
            dedupe_epoch_ = epoch;
        } else if (epoch > dedupe_epoch_) {
            if (epoch == dedupe_epoch_ + 1) {
                dedupe_previous_ = dedupe_current_;
            } else {
                dedupe_previous_.fill(0);
            }
            dedupe_current_.fill(0);
            dedupe_epoch_ = epoch;
        }

        static constexpr size_t MASK = (1U << 22) - 1U;
        const std::array<size_t, 3> positions = {
            static_cast<size_t>(dedupe_hash(id)) & MASK,
            static_cast<size_t>(dedupe_hash(id ^ 0xa0761d6478bd642fULL)) & MASK,
            static_cast<size_t>(dedupe_hash(id ^ 0xe7037ed1a0b428dbULL)) & MASK};
        if (bloom_contains(dedupe_current_, positions) ||
            bloom_contains(dedupe_previous_, positions))
            return true;
        for (const size_t position : positions)
            dedupe_current_[position >> 6] |= 1ULL << (position & 63U);
        return false;
    }

    void fill_order(OrderV2& order, uint8_t side,
                    uint64_t maker_amount, uint64_t taker_amount) {
        order.salt = rng_.next_salt();
        order.timestamp_ms = PresignedOrderPool::now_ms();
        std::memcpy(order.maker, cfg_.maker, 20);
        std::memcpy(order.signer, cfg_.signer, 20);
        std::memcpy(order.token_id, cfg_.effective_token_id_be(), 32);
        order.maker_amount = maker_amount;
        order.taker_amount = taker_amount;
        order.side = side;
        order.signature_type = cfg_.signature_type;
    }

    const MarketConfig& cfg_;
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& signals_;
    const EIP712Signer& signer_;
    PresignedOrderPool& pool_;
    Client& client_;
    const std::atomic<bool>* trading_enabled_;
    FastRandom rng_;
    // Two rotating Bloom epochs guarantee no false negatives inside the
    // configured TTL. False positives only reject work, which is fail-safe.
    std::array<uint64_t, 65536> dedupe_current_{};
    std::array<uint64_t, 65536> dedupe_previous_{};
    uint64_t dedupe_epoch_ = 0;
    uint64_t confirmed_inventory_ = 0;
    cledger::OrderLedger* ledger_ = nullptr;
    uint64_t ledger_blocks_ = 0;
    char ledger_error_[96]{};
    double committed_exposure_usd_ = 0.0;
    double worst_case_loss_usd_ = 0.0;
    uint64_t submitted_ = 0;
    uint64_t queued_ = 0;
    uint64_t submit_failed_ = 0;
};

#endif  // EXECUTION_ENGINE_HPP
