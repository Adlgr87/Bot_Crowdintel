#ifndef POSITION_TRACKER_HPP
#define POSITION_TRACKER_HPP

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

/**
 * FillEvent: A single fill from the user-channel WSS feed or REST polling.
 */
struct FillEvent {
    std::string order_id;          // client_order_id or exchange order ID
    std::string market_slug;
    bool is_buy;
    uint64_t price;                // * 1e6 (fixed point)
    uint64_t size;                 // * 1e6 (units)
    std::chrono::steady_clock::time_point timestamp;
};

/**
 * Position: Internal position tracking per market.
 *
 * Quantities are tracked in "units" (e.g., outcome tokens).
 * Cash flows are tracked in USD (fixed point * 1e6 for precision).
 */
struct Position {
    double quantity = 0.0;           // Net position (positive = long)
    double cash = 0.0;               // Net cash flow (positive = received)
    double realized_pnl = 0.0;      // Realized PnL from closed positions
    double avg_entry_price = 0.0;    // Average entry price (for unrealized PnL)
    std::chrono::steady_clock::time_point last_update;
};

/**
 * PositionTracker: Tracks internal positions from fills.
 *
 * Cold path updates (from user-channel fills feed).
 * Reconciliation compares internal vs API positions periodically.
 * If divergence > max_position_divergence, triggers kill switch.
 */
class PositionTracker {
public:
    explicit PositionTracker(double initial_capital_usd = 10000.0)
        : total_capital_(initial_capital_usd), pnl_(0.0) {}

    /**
     * Apply a fill event to update internal position.
     * Called from user-channel fills handler.
     *
     * NOTE: FillEvent.size is in UNITS (* 1e6 fixed point), not USD.
     * The cash value (USD) is computed as: fill_size_units * price_per_unit.
     */
    void apply_fill(const FillEvent& fill) {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        auto& pos = positions_[fill.market_slug];
        double fill_size = static_cast<double>(fill.size) / 1e6;     // units
        double price = static_cast<double>(fill.price) / 1e6;       // USD per unit
        double cash_value = fill_size * price;                       // USD total

        if (fill.is_buy) {
            // Buying: increase position (units), decrease cash (pay USD)
            double new_qty = pos.quantity + fill_size;
            pos.cash -= cash_value;

            if (pos.quantity < 0.0 && new_qty < 0.0) {
                // Short position, reducing (buying to cover) — realize PnL on covered portion
                double pnl_from_close = fill_size * (pos.avg_entry_price - price);
                pos.realized_pnl += pnl_from_close;
            } else if (pos.quantity != 0.0 && (new_qty == 0.0 || (new_qty > 0) != (pos.quantity > 0))) {
                // Crossing zero — realize PnL on full old position
                double closed_qty = std::abs(pos.quantity);
                double pnl_from_close = closed_qty * (price - pos.avg_entry_price);
                pos.realized_pnl += pnl_from_close;
                pos.avg_entry_price = price;  // Reset for remaining position
            }

            // Update average entry price (weighted by cash value)
            if (new_qty != 0.0) {
                double total_value = pos.avg_entry_price * pos.quantity + cash_value;
                pos.avg_entry_price = total_value / new_qty;
            }
            pos.quantity = new_qty;
        } else {
            // Selling: decrease position (units), increase cash (receive USD)
            double new_qty = pos.quantity - fill_size;
            pos.cash += cash_value;

            if (pos.quantity > 0.0 && new_qty > 0.0) {
                // Long position, reducing but not closing — realize PnL on sold portion
                double pnl_from_close = fill_size * (price - pos.avg_entry_price);
                pos.realized_pnl += pnl_from_close;
            } else if (pos.quantity < 0.0 && new_qty < 0.0) {
                // Short position, reducing but not closing — realize PnL on covered portion
                double pnl_from_close = fill_size * (pos.avg_entry_price - price);
                pos.realized_pnl += pnl_from_close;
            } else if (pos.quantity != 0.0 && (new_qty == 0.0 || (new_qty > 0) != (pos.quantity > 0))) {
                // Crossing zero — realize PnL on full old position, reset avg_entry for remaining
                double closed_qty = std::abs(pos.quantity);
                double pnl_from_close = closed_qty * (price - pos.avg_entry_price);
                pos.realized_pnl += pnl_from_close;
                pos.avg_entry_price = price;  // Reset for remaining position
            }
            pos.quantity = new_qty;
        }

        pos.last_update = std::chrono::steady_clock::now();
        pnl_ = 0.0;  // Invalidate cache
    }

    /**
     * Get net position for a market (hot path — O(1) lookup).
     */
    double get_net_position(const std::string& market_slug) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = positions_.find(market_slug);
        if (it != positions_.end()) {
            return it->second.quantity;
        }
        return 0.0;
    }

    /**
     * Get realized PnL for a market.
     */
    double get_realized_pnl(const std::string& market_slug) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = positions_.find(market_slug);
        if (it != positions_.end()) {
            return it->second.realized_pnl;
        }
        return 0.0;
    }

    /**
     * Get unrealized PnL for a market (requires current mark price).
     */
    double get_unrealized_pnl(const std::string& market_slug, double mark_price) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = positions_.find(market_slug);
        if (it != positions_.end()) {
            return it->second.quantity * (mark_price - it->second.avg_entry_price);
        }
        return 0.0;
    }

    /**
     * Reconcile internal position with API position.
     * Returns true if divergence exceeds tolerance.
     * Cold path — called periodically.
     */
    bool reconcile(const std::string& market_slug, double api_position,
                   double tolerance) {
        double internal = get_net_position(market_slug);
        double divergence = std::abs(internal - api_position) /
                           (std::max)(std::abs(internal), 0.001);
        return divergence > tolerance;
    }

    /**
     * Get total PnL (realized + unrealized across all markets).
     */
    double get_total_pnl() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        double total_realized = 0.0;
        for (const auto& [slug, pos] : positions_) {
            total_realized += pos.realized_pnl;
        }
        // Unrealized requires mark prices — call separately
        return total_realized;
    }

    /**
     * Get daily loss (negative PnL since start of day).
     */
    double get_daily_loss() const {
        auto now = std::chrono::steady_clock::now();
        auto today_start = get_today_start();
        std::shared_lock<std::shared_mutex> lock(mutex_);
        double loss = 0.0;
        for (const auto& [slug, pos] : positions_) {
            // Sum realized PnL from today
            if (pos.last_update >= today_start) {
                loss += -pos.realized_pnl;  // Loss = -PnL
            }
        }
        return loss > 0.0 ? loss : 0.0;
    }

    size_t get_market_count() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return positions_.size();
    }

private:
    static std::chrono::steady_clock::time_point get_today_start() {
        // Rolling 24-hour window for daily loss tracking.
        // Uses steady_clock (monotonic, no DST issues).
        return std::chrono::steady_clock::now() - std::chrono::hours(24);
    }

    double total_capital_;
    std::unordered_map<std::string, Position> positions_;
    mutable std::shared_mutex mutex_;
    std::atomic<double> pnl_{0.0};
};

#endif // POSITION_TRACKER_HPP
