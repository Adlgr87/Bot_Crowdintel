#ifndef ORDER_MANAGER_HPP
#define ORDER_MANAGER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <optional>

#include "eip712_signer.hpp"
#include "market_config.hpp"
#include "lightweight_client.hpp"
#include "transparent_string_hash.hpp"

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
     * O(1) hash insert. Also populates secondary index for self-trade detection.
     */
    std::string register_order(const OrderParams& params, uint64_t nonce,
                               const std::string& market_slug) {
        std::string client_order_id = generate_client_order_id(params.salt);
        register_order_no_alloc(params, nonce, market_slug, client_order_id.c_str());
        return client_order_id;
    }

    /**
     * Register an order with a pre-generated client_order_id (hot path optimization).
     * O(1) hash insert + secondary index update.
     *
     * This version avoids the std::string allocation in generate_client_order_id
     * by accepting a caller-provided buffer. The caller (ExecutionEngine hot path)
     * already generated the ID via generate_client_order_id_fixed().
     *
     * @param params              Order parameters
     * @param nonce               Exchange nonce
     * @param market_slug         Market identifier (string_view - no copy)
     * @param client_order_id_buf Pre-generated client order ID (null-terminated)
     */
    void register_order_no_alloc(const OrderParams& params, uint64_t nonce,
                                 std::string_view market_slug,
                                 const char* client_order_id_buf) {
        std::string client_order_id(client_order_id_buf);
        auto now = std::chrono::steady_clock::now();

        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto [it, inserted] = orders_.try_emplace(client_order_id);
        auto& order = it->second;
        order.client_order_id = std::move(client_order_id);
        order.nonce = nonce;
        order.params = params;
        order.status = OrderStatus::OPEN;  // Immediately OPEN after async push
        order.fills_quantity = 0;
        order.fills_cash_value = 0;
        order.created_at = now;
        order.last_update = now;
        order.market_slug = std::string(market_slug);

        // Populate secondary index for self-trade detection
        std::string_view idx_key = build_index_key(market_slug, params.side);
        open_order_index_[std::string(idx_key)] += 1;
    }
    /**
     * Update order status (called from fill handler, cancel handler, etc.).
     * O(1) — hash lookup. Also updates secondary index for self-trade detection.
     */
    void update_status(const std::string& client_order_id, OrderStatus new_status) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = orders_.find(client_order_id);
        if (it != orders_.end()) {
            bool was_open = it->second.is_open();
            it->second.status = new_status;
            it->second.last_update = std::chrono::steady_clock::now();
            // Update secondary index when order transitions to/from open state
            if (was_open && !it->second.is_open()) {
                update_index(it->second.market_slug, it->second.params.side, -1);
            }
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

    // ─── HOT PATH: Zero-alloc O(1) lookup ─────────────────────────────────

    /**
     * Check if a duplicate order exists for this market (HOT PATH — O(1), no alloc).
     * Uses secondary index with transparent hash for string_view lookup —
     * NO std::string allocation, NO O(N) scan.
     *
     * Self-trade prevention: if we have an open order for the same market
     * on the same side, the new order should be blocked or the old cancelled.
     */
    bool has_open_order(std::string_view market_slug, bool is_buy) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        // O(1) lookup via secondary index — transparent hash allows string_view lookup
        // NO std::string allocation (transparent_string_hash.hpp provides string_view support)
        // Key format: "market_slug:side" where side is 0 (buy) or 1 (sell)
        std::string_view key = build_index_key(market_slug, is_buy ? 0 : 1);
        auto it = open_order_index_.find(key);  // Transparent lookup — no alloc!
        if (it == open_order_index_.end()) return false;
        return it->second > 0;
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
    std::unordered_map<std::string, size_t, StringHash, StringEqual> open_order_index_;  // O(1) secondary index (transparent lookup)
    std::atomic<uint64_t> order_counter_{0};

    /**
     * Build the secondary index key from market_slug + side.
     * Returns a string_view into a thread_local buffer (zero allocation on hot path).
     * Key format: "market_slug:side" where side is 0 (buy) or 1 (sell).
     *
     * NOTE: Uses thread_local static buffer. This is safe because:
     * 1. The hot path (has_open_order) is single-threaded (hot path thread)
     * 2. The cold path (register_order, update_status) is under shared_mutex lock
     * 3. Both never run concurrently on the same thread
     */
    static std::string_view build_index_key(std::string_view market_slug, uint8_t side) {
        thread_local static char buffer[64];
        size_t len = market_slug.size() < 60 ? market_slug.size() : 60;
        memcpy(buffer, market_slug.data(), len);
        buffer[len] = ':';
        buffer[len + 1] = (side == 0) ? '0' : '1';
        buffer[len + 2] = '\0';
        return std::string_view(buffer, len + 2);
    }

    /**
     * Increment/decrement the secondary index for self-trade detection.
     * Called from register_order and update_status.
     */
    void update_index(std::string_view market_slug, uint8_t side, int delta) {
        std::string_view idx_key = build_index_key(market_slug, side);
        if (delta > 0) {
            open_order_index_[std::string(idx_key)] += delta;
        } else if (delta < 0) {
            auto it = open_order_index_.find(idx_key);  // Transparent lookup
            if (it != open_order_index_.end()) {
                if (static_cast<int>(it->second) + delta <= 0) {
                    open_order_index_.erase(it);
                } else {
                    it->second += delta;
                }
            }
        }
    }
};

#endif // ORDER_MANAGER_HPP
