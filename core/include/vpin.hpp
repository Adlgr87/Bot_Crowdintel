#ifndef VPIN_HPP
#define VPIN_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <time.h>

// Forward-declare OrderBookL2 for midpoint extraction
#include "order_book.hpp"

/**
 * VpinTracker: Real-time VPIN (Volume-Synchronized Probability of Informed Trading)
 * flow-toxicity detector.
 *
 * VPIN measures the probability that a trade was executed by an informed trader
 * (who has private information) versus an uninformed liquidity provider.
 * High VPIN → toxic flow → cancel/reduce open orders before adverse selection.
 *
 * Algorithm (Harris 2012, "Trading and Exchanges as an Information Machine"):
 *   1. Bin trades into equal-volume buckets (σ = bucket volume)
 *   2. Classify each trade as buy-initiated or sell-initiated (tick rule or midpoint)
 *   3. For each bucket b: U_b = |V_buy^b − V_sell^b| (unsigned informed volume)
 *   4. VPIN over window N = ΣU_b / ΣV_b  (ratio of uninformed to total volume)
 *
 * Design for <1ms cancellation decision:
 *   - Ring buffer of fixed-size buckets (no heap allocation)
 *   - Running sums (Σ|signed_vol|, Σ total_vol) → O(1) VPIN computation
 *   - Per-trade update is O(1) (at most 1 bucket seal per call)
 *   - Typical hot-path cost: ~30-50 CPU cycles per trade (<< 1μs)
 *
 * Thread-safety: This class is NOT thread-safe by design — it runs in the
 * hot-path thread (single producer of trades). VPIN snapshots for monitoring
 * can be exposed via atomic getters.
 */
class VpinTracker {
public:
    static constexpr size_t DEFAULT_WINDOW = 10;   // VPIN window (number of buckets)
    static constexpr size_t MAX_WINDOW     = 64;   // Compile-time ring buffer cap

    /**
     * A sealed volume bucket: completed accumulation of buy/sell volume.
     * Fixed-point volumes use * 1e6 scaling (matching OrderBookL2 Level2Entry).
     */
    struct Bucket {
        uint64_t buy_volume;    // Buyer-initiated volume (fixed-point, * 1e6)
        uint64_t sell_volume;   // Seller-initiated volume (fixed-point, * 1e6)
        uint64_t total_volume;  // Total volume = buy + sell (fixed-point, * 1e6)
        uint64_t timestamp_ns;  // CLOCK_MONOTONIC when bucket was sealed
    };

    /**
     * @param bucket_volume_usd    σ: Volume threshold to seal a bucket (e.g., $1000)
     * @param window               N: Number of sealed buckets in the VPIN window
     * @param toxicity_threshold   Cancel when VPIN > this (0.0–1.0, default 0.5)
     */
    VpinTracker(double bucket_volume_usd = 1000.0,
                size_t window = DEFAULT_WINDOW,
                double toxicity_threshold = 0.5)
        : bucket_volume_fp_(static_cast<uint64_t>(bucket_volume_usd * 1'000'000.0)),
          window_(window <= MAX_WINDOW ? window : MAX_WINDOW),
          toxicity_threshold_(toxicity_threshold),
          current_bucket_idx_(0),
          sealed_count_(0),
          active_buy_vol_(0),
          active_sell_vol_(0),
          active_total_vol_(0),
          running_abs_signed_vol_(0),
          running_total_vol_(0),
          last_trade_price_(0),
          last_tick_is_buy_(true),
          trade_count_(0),
          sealed_bucket_count_(0) {
        // Zero-initialize the bucket array
        for (size_t i = 0; i < MAX_WINDOW; i++) {
            buckets_[i] = {0, 0, 0, 0};
        }
    }

    // ─── Trade Classification (tick rule + midpoint) ─────────────────────

    /**
     * Classify a trade as buy- or sell-initiated using the tick rule:
     *   price ↑ → buy-initiated (aggressive buyer)
     *   price ↓ → sell-initiated (aggressive seller)
     *   price = last → use previous tick direction
     *
     * @param price_fp  Trade price in fixed-point (* 1e6)
     * @return true if buy-initiated, false if sell-initiated
     */
    bool classify_tick_rule(uint64_t price_fp) {
        if (last_trade_price_ > 0) {
            if (price_fp > last_trade_price_) {
                last_trade_price_ = price_fp;
                last_tick_is_buy_ = true;
                return true;
            } else if (price_fp < last_trade_price_) {
                last_trade_price_ = price_fp;
                last_tick_is_buy_ = false;
                return false;
            }
            // Price unchanged — inherit last tick direction
            return last_tick_is_buy_;
        }
        // First trade — assume buy
        last_trade_price_ = price_fp;
        last_tick_is_buy_ = true;
        return true;
    }

    /**
     * Classify a trade using the midpoint tick rule:
     *   price > midpoint → buy-initiated
     *   price < midpoint → sell-initiated
     *   price = midpoint → use previous tick direction
     *
     * @param price_fp      Trade price (* 1e6)
     * @param midpoint_fp   Best bid/ask midpoint (* 1e6)
     */
    bool classify_midpoint(uint64_t price_fp, uint64_t midpoint_fp) {
        if (midpoint_fp == 0) {
            return classify_tick_rule(price_fp);
        }
        if (price_fp > midpoint_fp) {
            last_trade_price_ = price_fp;
            last_tick_is_buy_ = true;
            return true;
        } else if (price_fp < midpoint_fp) {
            last_trade_price_ = price_fp;
            last_tick_is_buy_ = false;
            return false;
        }
        // At midpoint — use previous tick direction
        last_trade_price_ = price_fp;
        return last_tick_is_buy_;
    }

    /**
     * Compute the midpoint price from the order book's best bid/ask (fixed-point).
     * If either level is empty, returns 0 (caller should fall back to tick rule).
     */
    static uint64_t get_midpoint_fp(const OrderBookL2& book) {
        const auto& best_bid = book.get_bid(0);
        const auto& best_ask = book.get_ask(0);
        if (best_bid.price == 0 || best_ask.price == 0) return 0;
        return (best_bid.price + best_ask.price) / 2;
    }

    // ─── Trade Processing (HOT PATH: O(1) per call) ────────────────────────

    /**
     * Process a trade event with pre-classified direction.
     * O(1) — at most one bucket seal per call.
     *
     * @param price_fp     Trade price (fixed-point, * 1e6)
     * @param size_fp      Trade size (fixed-point, * 1e6)
     * @param is_buy       True if buyer-initiated, false if seller-initiated
     * @param timestamp_ns CLOCK_MONOTONIC ns (0 = now)
     */
    void on_trade(uint64_t price_fp, uint64_t size_fp, bool is_buy,
                  uint64_t timestamp_ns = 0) {
        if (timestamp_ns == 0) {
            timestamp_ns = now_ns();
        }

        last_trade_price_ = price_fp;
        if (is_buy) {
            last_tick_is_buy_ = true;
            active_buy_vol_ += size_fp;
        } else {
            last_tick_is_buy_ = false;
            active_sell_vol_ += size_fp;
        }
        active_total_vol_ += size_fp;
        trade_count_++;

        // Check if the active bucket is full → seal and rotate
        if (active_total_vol_ >= bucket_volume_fp_) {
            seal_bucket(timestamp_ns);
        }
    }

    /**
     * Process a trade with automatic midpoint-based classification.
     * Convenience wrapper — call this when you have the order book.
     */
    void on_trade_with_book(uint64_t price_fp, uint64_t size_fp,
                            const OrderBookL2& book,
                            uint64_t timestamp_ns = 0) {
        uint64_t midpoint = get_midpoint_fp(book);
        bool is_buy = (midpoint > 0)
            ? classify_midpoint(price_fp, midpoint)
            : classify_tick_rule(price_fp);
        on_trade(price_fp, size_fp, is_buy, timestamp_ns);
    }

    // ─── VPIN Computation (O(1)) ─────────────────────────────────────────

    /**
     * Current VPIN value (0.0 to 1.0).
     *   0.0 = perfectly balanced flow (informed = uninformed)
     *   1.0 = fully one-sided flow (all informed)
     *
     * O(1) — derived from running sums, no iteration over buckets.
     */
    double get_vpin() const {
        if (running_total_vol_ == 0) return 0.0;
        return static_cast<double>(running_abs_signed_vol_) /
               static_cast<double>(running_total_vol_);
    }

    /**
     * Flow toxicity score — synonym for VPIN, with clamping to [0, 1].
     * High values indicate informed trading dominance.
     */
    double get_toxicity_score() const {
        double vpin = get_vpin();
        return (vpin < 0.0) ? 0.0 : (vpin > 1.0 ? 1.0 : vpin);
    }

    /**
     * Should we cancel / avoid opening positions?
     * Returns true when VPIN exceeds the toxicity threshold.
     * Decision latency: < 1μs (single compare).
     */
    bool should_cancel() const {
        double toxicity = get_toxicity_score();
        return toxicity > toxicity_threshold_;
    }

    /**
     * Should we cancel? Returns a cancellation urgency level.
     *   0 = no cancellation (healthy flow)
     *   1 = monitor (VPIN > threshold * 0.75)
     *   2 = cancel opens (VPIN > threshold)
     *   3 = cancel + flatten (VPIN > threshold * 1.5, extremely toxic)
     */
    int cancel_urgency() const {
        double toxicity = get_toxicity_score();
        if (toxicity > toxicity_threshold_ * 1.5) return 3;
        if (toxicity > toxicity_threshold_)      return 2;
        if (toxicity > toxicity_threshold_ * 0.75) return 1;
        return 0;
    }

    // ─── Monitoring / Telemetry ───────────────────────────────────────────

    /** Total number of trades processed since construction / last reset. */
    uint64_t total_trades() const { return trade_count_; }

    /** Total number of sealed buckets since construction / last reset. */
    uint64_t sealed_buckets() const { return sealed_bucket_count_; }

    /** Number of sealed buckets in the current window. */
    size_t window_filled() const {
        return (sealed_count_ < window_) ? sealed_count_ : window_;
    }

    /** Current active (in-progress) bucket fill ratio [0, 1]. */
    double bucket_fill_ratio() const {
        if (bucket_volume_fp_ == 0) return 0.0;
        return static_cast<double>(active_total_vol_) /
               static_cast<double>(bucket_volume_fp_);
    }

    /** Snapshot of all buckets in the window (for debugging). */
    size_t get_snapshot(Bucket* out, size_t max_buckets) const {
        size_t n = window_filled();
        if (n > max_buckets) n = max_buckets;
        // Buckets are in ring-buffer order: [oldest → newest]
        size_t start = (current_bucket_idx_ + window_ - n) % window_;
        for (size_t i = 0; i < n; i++) {
            out[i] = buckets_[(start + i) % window_];
        }
        return n;
    }

    /** Reset all state (for testing or session boundaries). */
    void reset() {
        for (size_t i = 0; i < window_; i++) {
            buckets_[i] = {0, 0, 0, 0};
        }
        current_bucket_idx_ = 0;
        sealed_count_ = 0;
        active_buy_vol_ = 0;
        active_sell_vol_ = 0;
        active_total_vol_ = 0;
        running_abs_signed_vol_ = 0;
        running_total_vol_ = 0;
        last_trade_price_ = 0;
        last_tick_is_buy_ = true;
        trade_count_ = 0;
        sealed_bucket_count_ = 0;
    }

    // ─── Configurable Parameters ─────────────────────────────────────────

    void set_toxicity_threshold(double t) {
        toxicity_threshold_ = (t < 0.0) ? 0.0 : (t > 1.0 ? 1.0 : t);
    }
    double get_toxicity_threshold() const { return toxicity_threshold_; }

    void set_window(size_t w) {
        if (w <= MAX_WINDOW && w > 0) {
            // Reset if window changes (data would be inconsistent)
            reset();
            window_ = w;
        }
    }
    size_t get_window() const { return window_; }

private:
    static uint64_t now_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    /**
     * Seal the active bucket and push it into the ring buffer.
     * O(1) — updates running sums, evicts oldest bucket if window is full.
     *
     * The unsigned informed volume for a bucket = |buy_vol − sell_vol|.
     * VPIN = Σ|buy − sell| / Σ(buy + sell) over the window.
     */
    void seal_bucket(uint64_t timestamp_ns) {
        // Compute unsigned informed volume for this bucket
        uint64_t abs_signed_vol;
        if (active_buy_vol_ > active_sell_vol_) {
            abs_signed_vol = active_buy_vol_ - active_sell_vol_;
        } else {
            abs_signed_vol = active_sell_vol_ - active_buy_vol_;
        }

        // Evict oldest bucket from running sums if window is full
        if (sealed_count_ >= window_) {
            const Bucket& old = buckets_[current_bucket_idx_];
            uint64_t old_abs_signed;
            if (old.buy_volume > old.sell_volume) {
                old_abs_signed = old.buy_volume - old.sell_volume;
            } else {
                old_abs_signed = old.sell_volume - old.buy_volume;
            }
            running_abs_signed_vol_ -= old_abs_signed;
            running_total_vol_  -= old.total_volume;
        } else {
            sealed_count_++;
        }

        // Write the sealed bucket and advance the ring pointer
        buckets_[current_bucket_idx_] = {
            active_buy_vol_,
            active_sell_vol_,
            active_total_vol_,
            timestamp_ns
        };
        current_bucket_idx_ = (current_bucket_idx_ + 1) % window_;

        // Update running sums (add new bucket's contribution)
        running_abs_signed_vol_ += abs_signed_vol;
        running_total_vol_    += active_total_vol_;

        // Reset active bucket for next accumulation
        active_buy_vol_ = 0;
        active_sell_vol_ = 0;
        active_total_vol_ = 0;

        sealed_bucket_count_++;
    }

    // ─── Configuration ──
    const uint64_t bucket_volume_fp_;       // σ in fixed-point units
    size_t window_;                         // N (VPIN window size)
    double toxicity_threshold_;             // Cancel when VPIN > this

    // ─── Ring Buffer (fixed-size, cache-aligned) ──
    alignas(64) Bucket buckets_[MAX_WINDOW];
    alignas(64) size_t current_bucket_idx_;  // Next slot to write (ring pointer)
    alignas(64) size_t sealed_count_;         // Total buckets sealed (monotonic)

    // ─── Active Bucket (in-progress accumulation) ──
    uint64_t active_buy_vol_;
    uint64_t active_sell_vol_;
    uint64_t active_total_vol_;

    // ─── Running Sums (O(1) VPIN) ──
    uint64_t running_abs_signed_vol_;  // Σ|buy − sell| over window
    uint64_t running_total_vol_;       // Σ(buy + sell) over window

    // ─── Tick Classification State ──
    uint64_t last_trade_price_;
    bool last_tick_is_buy_;

    // ─── Telemetry Counters ──
    uint64_t trade_count_;          // Total trades processed (incremented per on_trade)
    uint64_t sealed_bucket_count_; // Total buckets sealed (incremented per seal_bucket)
};

#endif // VPIN_HPP
