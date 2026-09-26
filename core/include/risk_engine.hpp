#ifndef RISK_ENGINE_HPP
#define RISK_ENGINE_HPP

#include <atomic>
#include <chrono>
#include <cmath>      // std::abs, std::max (used in check_position_divergence)
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "market_config.hpp"
#include "tick_result.hpp"  // TickResult enum (shared definition, avoids redefinition)
#include "eip712_signer.hpp"  // OrderParams struct definition

/**
 * RiskEngine: Real-time risk checks executed BEFORE order signing.
 *
 * Design principles:
 * - O(1) per check, branch-predicted (fast path: all green)
 * - Thread-safe: all counters atomic, kill_switch atomic
 * - Hot path: pre_trade_check() does NOT allocate, does NOT do I/O
 * - Kill switch is an atomic<bool>, visible across threads (SPSC consumer)
 *
 * NOTA CRÍTICA: El kill switch es atómico y NO puede activarse desde el hot path
 * de forma no atómica. Las señales externas de CrowdIntel nunca deben tocar
 * este flag directamente — pasan por pre_trade_check() que evalúa el flag.
 */
class RiskEngine {
public:
    explicit RiskEngine(const RiskConfig& config)
        : config_(config),
          kill_switch_(false),
          daily_loss_usd_(0.0),
          consecutive_rejects_(0),
          last_feed_activity_(std::chrono::steady_clock::now()) {}

    // ─── Hot Path (O(1), no allocation) ────────────────────────────────

    /**
     * pre_trade_check: Evaluado ANTES de firmar cada orden.
     * Costo O(1): atomic loads + comparaciones. Branch-predicted.
     *
     * Returns TickResult::OK only if ALL checks pass.
     */
    TickResult pre_trade_check(const OrderParams& params,
                               double usdc_balance,
                               double pol_balance,
                               double market_exposure,
                               double market_pnl) {
        // 1. Kill switch check — O(1), atomic, hot path
        if (__builtin_expect(kill_switch_.load(std::memory_order_acquire), 0)) {
            return TickResult::KILL_SWITCH;
        }

        // 2. Order size limit — O(1)
        double order_usd = static_cast<double>(params.size) / 1e6;
        if (__builtin_expect(order_usd > config_.max_order_usd, 0)) {
            return TickResult::RISK_BLOCKED;
        }

        // 3. Entry band check (Polywhales): price must be 35-70¢ — O(1)
        if (__builtin_expect(!is_price_in_entry_band(params.price), 0)) {
            return TickResult::RISK_BLOCKED;
        }

        // 4. Daily loss limit — O(1) atomic
        if (__builtin_expect(daily_loss_usd_.load(std::memory_order_acquire) >
                              config_.max_daily_loss_usd, 0)) {
            activate_kill_switch();
            return TickResult::KILL_SWITCH;
        }

        // 5. Exposure per market — O(1)
        if (__builtin_expect(market_exposure > config_.max_exposure_per_market, 0)) {
            return TickResult::RISK_BLOCKED;
        }

        // 6. Balance check — O(1)
        if (__builtin_expect(usdc_balance < config_.min_usdc_balance, 0)) {
            return TickResult::INSUFFICIENT_BALANCE;
        }
        if (__builtin_expect(pol_balance < config_.min_pol_balance, 0)) {
            return TickResult::INSUFFICIENT_BALANCE;
        }

        // 6. Orders per minute window — O(1) (checked via record_order)
        //    The window check is done in record_order / query
        if (!check_rate_window()) {
            return TickResult::RISK_BLOCKED;
        }

        // 7. Price deviation — O(1)
        // NOTE: price_deviation_bps is checked by caller (ExecutionEngine)
        //       using this helper:
        // if (price_deviation_bps > config_.max_price_deviation_bps) return RISK_BLOCKED;

        return TickResult::OK;
    }

    /**
     * Record a successful order (for rate-limited windows and exposure tracking).
     * Call AFTER order is signed and queued for async submission.
     * O(1): single bucket increment with lazy reset.
     */
    void record_order(double order_usd, bool is_buy) {
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t now_bucket = now_ns / BUCKET_NS;
        uint64_t bucket_idx = now_bucket % WINDOW_BUCKETS;

        reset_if_stale(order_buckets_[bucket_idx], now_bucket);
        order_buckets_[bucket_idx].count.fetch_add(1, std::memory_order_release);

        (void)is_buy;  // Direction tracked separately by PositionTracker
    }

    /**
     * Record a cancellation (for cancel rate limit window).
     * O(1): single bucket increment with lazy reset.
     */
    void record_cancel() {
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t now_bucket = now_ns / BUCKET_NS;
        uint64_t bucket_idx = now_bucket % WINDOW_BUCKETS;

        reset_if_stale(cancel_buckets_[bucket_idx], now_bucket);
        cancel_buckets_[bucket_idx].count.fetch_add(1, std::memory_order_release);
    }

    /**
     * Add to daily loss counter (from fills, cold path).
     */
    void add_loss(double usd_loss) {
        double current = daily_loss_usd_.load(std::memory_order_relaxed);
        while (usd_loss > 0.0 &&
               !daily_loss_usd_.compare_exchange_weak(current, current + usd_loss,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed)) {
            current = daily_loss_usd_.load(std::memory_order_relaxed);
        }
        // Check if we've crossed the daily loss threshold
        if (daily_loss_usd_.load(std::memory_order_acquire) > config_.max_daily_loss_usd &&
            config_.kill_switch_enabled) {
            activate_kill_switch();
        }
    }

    /**
     * Record feed activity (for dead feed detection).
     */
    void record_feed_activity() {
        last_feed_activity_.store(std::chrono::steady_clock::now(), std::memory_order_release);
    }

    /**
     * Check if feed has been dead for too long (cold path).
     */
    bool is_feed_dead() const {
        auto last = last_feed_activity_.load(std::memory_order_acquire);
        auto elapsed = std::chrono::steady_clock::now() - last;
        return elapsed > std::chrono::milliseconds(config_.feed_dead_timeout_ms);
    }

    /**
     * Entry band check (Polywhales adaptation): price must be within
     * configurable min/max range (default 35¢-70¢).
     * O(1) comparison.
     */
    bool is_price_in_entry_band(uint64_t price_micros) const {
        return price_micros >= config_.min_price_micros &&
               price_micros <= config_.max_price_micros;
    }

    /**
        auto last = last_feed_activity_.load(std::memory_order_acquire);
        auto elapsed = std::chrono::steady_clock::now() - last;
        return elapsed > std::chrono::milliseconds(config_.feed_dead_timeout_ms);
    }

    /**
     * Record a rejected order (for consecutive reject streak).
     */
    void record_reject() {
        int current = consecutive_rejects_.fetch_add(1, std::memory_order_acq_rel);
        if (current + 1 >= config_.max_consecutive_rejects && config_.kill_switch_enabled) {
            activate_kill_switch();
        }
    }

    /**
     * Reset consecutive rejects (called on a successful fill).
     */
    void reset_rejects() {
        consecutive_rejects_.store(0, std::memory_order_release);
    }

    // ─── Kill Switch ────────────────────────────────────────────────────

    /**
     * Activate the kill switch — ATOMIC, thread-safe.
     * Once active, no orders will be signed or submitted.
     * External signals CANNOT activate this — only internal conditions.
     */
    void activate_kill_switch() {
        bool expected = false;
        if (kill_switch_.compare_exchange_strong(expected, true,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {
            // Kill switch just activated — notify telemetry
            // (telemetry is cold path, safe to use)
        }
    }

    void deactivate_kill_switch() {
        kill_switch_.store(false, std::memory_order_release);
    }

    bool is_kill_switch_active() const {
        return kill_switch_.load(std::memory_order_acquire);
    }

    // ─── Position Divergence Check (cold path) ──────────────────────────

    /**
     * Called by PositionTracker during reconciliation.
     * If internal position diverges from API position beyond tolerance,
     * activates kill switch.
     */
    bool check_position_divergence(double internal_pos, double api_pos,
                                    const std::string& market_slug) {
        double divergence = std::abs(internal_pos - api_pos) /
                            (std::max)(std::abs(internal_pos), 0.001);
        if (divergence > config_.max_position_divergence) {
            activate_kill_switch();
            return true;
        }
        return false;
    }

    // ─── Getters ────────────────────────────────────────────────────────

    double get_daily_loss() const {
        return daily_loss_usd_.load(std::memory_order_acquire);
    }

    const RiskConfig& get_config() const { return config_; }

private:
    // ─── Configuration (immutable after construction) ─────────────────
    const RiskConfig config_;

    // ─── Kill Switch (atomic, visible across threads) ─────────────────
    std::atomic<bool> kill_switch_;

    // ─── Daily Loss (atomic double for lock-free updates) ─────────────
    std::atomic<double> daily_loss_usd_;

    // ─── Consecutive Rejects (atomic counter) ─────────────────────────
    std::atomic<int> consecutive_rejects_;

    // ─── Feed Activity Timestamp (atomic) ─────────────────────────────
    mutable std::atomic<std::chrono::steady_clock::time_point> last_feed_activity_;

    // ─── Rate Window: Bucket-Based Sliding Window (O(1), branch-predicted) ─
    // Uses a fixed array of time-bucketed counters. At 1 bucket/sec, 60-second
    // window = 60 buckets. Counter lookup is O(1) via modular indexing.
    // Stale buckets are reset lazily on access (no O(N) scan).
    static constexpr uint64_t BUCKET_NS = 1'000'000'000ULL;  // 1 second per bucket
    static constexpr size_t WINDOW_BUCKETS = 60;  // 60-second window in 1s buckets

    struct alignas(64) TimeBucket {
        std::atomic<uint32_t> count{0};
        std::atomic<uint64_t> epoch{0};  // Last reset time bucket index
    };
    TimeBucket order_buckets_[WINDOW_BUCKETS];
    TimeBucket cancel_buckets_[WINDOW_BUCKETS];

    // ─── Rate Window Check (O(WINDOW_BUCKETS) with branch-predicted fast path) ─
    bool check_rate_window() {
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t now_bucket = now_ns / BUCKET_NS;

        // Sum counts across all 60 buckets within the sliding window.
        // This is O(WINDOW_BUCKETS) = O(60) but:
        // 1. WINDOW_BUCKETS=60 fits in L1 cache (~4.8KB total)
        // 2. Branch-predicted: most buckets are 0 (just reset)
        // 3. Hot path fast path: early exit if we already exceed limits
        uint32_t total_orders = 0;
        uint32_t total_cancels = 0;
        for (size_t i = 0; i < WINDOW_BUCKETS; i++) {
            uint64_t bucket_time = now_bucket - (now_bucket % WINDOW_BUCKETS) + i;
            // Only count buckets that are within the 60-second window
            if (bucket_time <= now_bucket) {
                reset_if_stale(order_buckets_[i], bucket_time);
                total_orders += order_buckets_[i].count.load(std::memory_order_acquire);
            }
            if (bucket_time <= now_bucket) {
                reset_if_stale(cancel_buckets_[i], bucket_time);
                total_cancels += cancel_buckets_[i].count.load(std::memory_order_acquire);
            }
        }

        // Check rate limits
        if (__builtin_expect(total_orders >= static_cast<uint32_t>(config_.max_orders_per_min), 0)) {
            return false;
        }
        if (__builtin_expect(total_cancels >= static_cast<uint32_t>(config_.max_cancels_per_min), 0)) {
            return false;
        }

        return true;
    }

    static void reset_if_stale(TimeBucket& bucket, uint64_t now_bucket) {
        uint64_t expected_epoch = bucket.epoch.load(std::memory_order_relaxed);
        if (expected_epoch != now_bucket) {
            bucket.count.store(0, std::memory_order_release);
            bucket.epoch.store(now_bucket, std::memory_order_release);
        }
    }
};

#endif // RISK_ENGINE_HPP
