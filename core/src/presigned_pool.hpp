#ifndef PRESIGNED_POOL_HPP
#define PRESIGNED_POOL_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "eip712_signer.hpp"
#include "order_book.hpp"

/**
 * PresignedOrderPool: Pool of pre-signed orders with validity tracking.
 *
 * When the order book moves more than a threshold (max_price_deviation_bps),
 * all orders in the pool are marked INVALID and should be cancelled/replaced.
 *
 * This implements T3-5: Cancelación de órdenes obsoletas.
 */
struct PresignedOrder {
    uint64_t nonce;
    uint64_t salt;
    std::array<uint8_t, 65> signature;
    std::string payload;
    uint64_t price;              // Price at which this order was signed (* 1e6)
    uint64_t size;               // Size (* 1e6)
    uint8_t side;                // 0 = buy, 1 = sell
    std::chrono::steady_clock::time_point created_at;
    bool valid;
    std::string client_order_id;
};

class PresignedOrderPool {
public:
    explicit PresignedOrderPool(int max_price_deviation_bps = 500)
        : max_price_deviation_bps_(max_price_deviation_bps),
          last_price_move_check_(std::chrono::steady_clock::now()) {}

    /**
     * Check if the order book has moved beyond the threshold since
     * the order was created. If so, mark the pool as invalid.
     */
    void check_and_invalidate(const Level2Entry& current_best_bid,
                               const Level2Entry& current_best_ask) {
        uint64_t bid_price = current_best_bid.price;
        uint64_t ask_price = current_best_ask.price;

        // Check all orders in the pool
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto& [id, order] : pool_) {
            if (!order.valid) continue;

            uint64_t current_ref = order.side == 0 ? ask_price : bid_price;
            uint64_t order_ref = order.price;

            if (order_ref > 0 && current_ref > 0) {
                double deviation_bps = std::abs(static_cast<double>(current_ref) -
                                                static_cast<double>(order_ref)) /
                                       static_cast<double>(order_ref) * 10000.0;
                if (deviation_bps > max_price_deviation_bps_) {
                    order.valid = false;
                }
            }
        }
        last_price_move_check_.store(std::chrono::steady_clock::now(),
                                      std::memory_order_release);
    }

    /**
     * Add a presigned order to the pool.
     */
    void add_order(const std::string& client_order_id,
                    const PresignedOrder& order) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        pool_[client_order_id] = order;
    }

    /**
     * Get all valid orders (for cancellation when pool is invalidated).
     */
    std::vector<std::string> get_valid_order_ids() const {
        std::vector<std::string> result;
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& [id, order] : pool_) {
            if (order.valid) {
                result.push_back(id);
            }
        }
        return result;
    }

    /**
     * Invalidate all orders (emergency cancel).
     */
    void invalidate_all() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto& [id, order] : pool_) {
            order.valid = false;
        }
    }

    /**
     * Remove a completed/cancelled order from the pool.
     */
    void remove_order(const std::string& client_order_id) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        pool_.erase(client_order_id);
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return pool_.size();
    }

    size_t valid_count() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& [id, order] : pool_) {
            if (order.valid) count++;
        }
        return count;
    }

private:
    int max_price_deviation_bps_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, PresignedOrder> pool_;
    std::atomic<std::chrono::steady_clock::time_point> last_price_move_check_;
};

#endif // PRESIGNED_POOL_HPP
