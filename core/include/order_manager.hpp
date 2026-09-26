#ifndef ORDER_MANAGER_HPP
#define ORDER_MANAGER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "eip712_signer.hpp"
#include "market_config.hpp"
#include "lightweight_client.hpp"

/**
 * OrderStatus: Estado del ciclo de vida de una orden.
 */
enum class OrderStatus {
    PENDING,    // Registered, not yet submitted
    OPEN,       // Submitted, waiting for fills
    PARTIAL,    // Partially filled
    FILLED,     // Fully filled
    CANCELLED,  // Cancelled (by us or by exchange)
    REJECTED,   // Rejected by exchange
    EXPIRED,    // Expired by time-in-force
    UNKNOWN,    // Status could not be determined
};

/**
 * ManagedOrder: Orden con seguimiento completo del ciclo de vida.
 */
struct ManagedOrder {
    std::string client_order_id;           // Unique ID: salt_hex + timestamp + counter
    uint64_t nonce;
    OrderParams params;
    OrderStatus status;
    uint64_t fills_quantity;               // * 1e6 units
    uint64_t fills_cash_value;             // * 1e6 USD
    std::string market_slug;              // Market identifier for this order
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_update;

    bool is_open() const {
        return status == OrderStatus::OPEN || status == OrderStatus::PARTIAL;
    }

    bool is_terminal() const {
        return status == OrderStatus::FILLED || status == OrderStatus::CANCELLED ||
               status == OrderStatus::REJECTED || status == OrderStatus::EXPIRED;
    }
};

/**
 * OrderManager: Tracks every submitted order by client_order_id.
 *
 * CRITICAL: Implements anti-blind-retry — before re-submitting after a
 * network timeout, the manager queries the exchange for the order's
 * actual status. If already filled, it does NOT re-submit.
 *
 * Thread-safe: uses shared_mutex for concurrent access.
 * Hot path: register_order() and generate_client_order_id() are O(1) hash operations.
 */
class OrderManager {
public:
    explicit OrderManager(LightweightCLOBClient& client)
        : client_(client), order_counter_(0) {}

    /**
     * Generate a unique client_order_id.
     * Uses salt + timestamp + atomic counter for global uniqueness.
     * O(1), thread-safe.
     */
    std::string generate_client_order_id(uint64_t salt) {
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t counter = order_counter_.fetch_add(1, std::memory_order_relaxed);

        char buf[64];
        snprintf(buf, sizeof(buf), "%016llx-%016llx-%08llx",
                 (unsigned long long)salt,
                 (unsigned long long)now_ns,
                 (unsigned long long)counter);
        return std::string(buf);
    }

    /**
     * Register an order as PENDING (before submission).
     * Returns the generated client_order_id so the caller can track it.
     * O(1) — hash insert.
     */
    std::string register_order(const OrderParams& params, uint64_t nonce,
                                const std::string& market_slug) {
        std::string client_order_id = generate_client_order_id(params.salt);
        auto now = std::chrono::steady_clock::now();

        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto [it, inserted] = orders_.try_emplace(client_order_id);
        auto& order = it->second;
        order.client_order_id = client_order_id;
        order.nonce = nonce;
        order.params = params;
        order.status = OrderStatus::PENDING;
        order.fills_quantity = 0;
        order.fills_cash_value = 0;
        order.created_at = now;
        order.last_update = now;
        order.market_slug = market_slug;
        return client_order_id;
    }

    /**
     * Update order status (called from fill handler, cancel handler, etc.).
     * O(1) — hash lookup.
     */
    void update_status(const std::string& client_order_id, OrderStatus new_status) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = orders_.find(client_order_id);
        if (it != orders_.end()) {
            it->second.status = new_status;
            it->second.last_update = std::chrono::steady_clock::now();
        }
    }

    /**
     * Apply a fill to an order (update quantity and cash value).
     * O(1).
     */
    void apply_fill(const std::string& client_order_id,
                     uint64_t fill_quantity,  // * 1e6
                     uint64_t fill_cash_value) {  // * 1e6
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = orders_.find(client_order_id);
        if (it != orders_.end()) {
            auto& order = it->second;
            order.fills_quantity += fill_quantity;
            order.fills_cash_value += fill_cash_value;
            if (order.fills_quantity >= order.params.size) {
                order.status = OrderStatus::FILLED;
            } else if (order.fills_quantity > 0) {
                order.status = OrderStatus::PARTIAL;
            }
            order.last_update = std::chrono::steady_clock::now();
        }
    }

    /**
     * Check if a duplicate order exists for this market.
     * Self-trade prevention: if we have an open order for the same market
     * on the same side, the new order should be blocked or the old cancelled.
     */
    bool has_open_order(const std::string& market_slug, bool is_buy) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& [id, order] : orders_) {
            if (order.market_slug == market_slug &&
                order.is_open() &&
                (order.params.side == (is_buy ? 0 : 1))) {
                return true;
            }
        }
        return false;
    }

    /**
     * Get all open orders (for cancellation).
     */
    std::vector<ManagedOrder*> get_open_orders() {
        std::vector<ManagedOrder*> result;
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (auto& [id, order] : orders_) {
            if (order.is_open()) {
                result.push_back(&order);
            }
        }
        return result;
    }

    /**
     * Get order by client_order_id. Returns nullptr if not found.
     * Thread-safe: the pointer is only valid within the lock scope.
     * Hot path callers should use find_order_copy() instead.
     */
    std::optional<ManagedOrder> find_order_copy(const std::string& client_order_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = orders_.find(client_order_id);
        if (it != orders_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /**
     * Anti-blind-retry: Check if an order should be retried after a timeout.
     * Queries the exchange for the order's actual status BEFORE re-submitting.
     *
     * Returns:
     *   - RETRY: order not found on exchange (safe to submit)
     *   - SKIP: order already exists on exchange (do not resubmit)
     *   - DUPLICATE: order found and filled/partial (mark locally, do not resubmit)
     */
    enum class RetryDecision { RETRY, SKIP, DUPLICATE };

    /**
     * Parse exchange order status from a JSON response body.
     * Extracted as a static method for unit testability (T3-4).
     * Looks for "status":"<value>" patterns in the JSON body.
     *
     * @param body Raw JSON response from the exchange query_order_status endpoint
     * @return Parsed OrderStatus, or UNKNOWN if not recognized
     */
    static OrderStatus parse_exchange_status(const std::string& body) {
        if (body.find("\"status\":\"filled\"") != std::string::npos) {
            return OrderStatus::FILLED;
        } else if (body.find("\"status\":\"partially_filled\"") != std::string::npos) {
            return OrderStatus::PARTIAL;
        } else if (body.find("\"status\":\"open\"") != std::string::npos) {
            return OrderStatus::OPEN;
        } else if (body.find("\"status\":\"canceled\"") != std::string::npos) {
            return OrderStatus::CANCELLED;
        } else if (body.find("\"status\":\"rejected\"") != std::string::npos ||
                   body.find("\"status\":\"error\"") != std::string::npos) {
            return OrderStatus::REJECTED;
        }
        return OrderStatus::UNKNOWN;
    }

    RetryDecision should_retry(const std::string& client_order_id) {
        // Query the exchange for the order's status
        auto exchange_status_str = client_.query_order_status(client_order_id);

        // Parse the response using the testable static parser (T3-4)
        auto exchange_status = OrderStatus::UNKNOWN;
        if (exchange_status_str) {
            exchange_status = parse_exchange_status(*exchange_status_str);
        }

        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = orders_.find(client_order_id);
        if (it == orders_.end()) {
            // Not in our local tracker — query result is authoritative
            if (exchange_status == OrderStatus::UNKNOWN) {
                return RetryDecision::RETRY;  // Not found anywhere → safe to retry
            }
            // Found on exchange but not locally — this shouldn't happen
            // Record it and skip
            return RetryDecision::SKIP;
        }

        auto& order = it->second;
        if (exchange_status == OrderStatus::UNKNOWN) {
            // Not found on exchange — could be a network partition
            // Check if local is already FILLED (in case we got a fill after timeout)
            if (order.status == OrderStatus::FILLED) {
                return RetryDecision::DUPLICATE;
            }
            return RetryDecision::RETRY;
        }

        // Exchange has the order — sync our state
        order.status = exchange_status;
        order.last_update = std::chrono::steady_clock::now();

        if (order.is_terminal() && order.fills_quantity > 0) {
            return RetryDecision::DUPLICATE;  // Already filled/partial
        }
        if (order.status == OrderStatus::FILLED || order.status == OrderStatus::CANCELLED) {
            return RetryDecision::DUPLICATE;
        }
        return RetryDecision::SKIP;  // Already on exchange, don't resubmit
    }

    size_t get_open_order_count() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& [id, order] : orders_) {
            if (order.is_open()) count++;
        }
        return count;
    }

private:
    LightweightCLOBClient& client_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ManagedOrder> orders_;
    std::atomic<uint64_t> order_counter_{0};
};

#endif // ORDER_MANAGER_HPP
