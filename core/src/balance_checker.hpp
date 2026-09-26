#ifndef BALANCE_CHECKER_HPP
#define BALANCE_CHECKER_HPP

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <optional>

#include "lightweight_client.hpp"
#include "market_config.hpp"

/**
 * BalanceChecker: Queries USDC and POL balances via Cold Path RPC.
 *
 * CRITICAL: This class does NOT run in the hot path. Balance checks
 * happen at startup and periodically (every N seconds). The hot path
 * reads from the cached balance atomically.
 *
 * The balances are queried via the CLOB API (GET /v2/balance or equivalent).
 * In production, this uses the same authenticated client but a different endpoint.
 */
class BalanceChecker {
public:
    struct Balance {
        double usdc_balance;
        double pol_balance;
        std::chrono::steady_clock::time_point last_update;
    };

    BalanceChecker(LightweightCLOBClient& client, const RiskConfig& config)
        : client_(client), config_(config),
          cached_balance_{0.0, 0.0, std::chrono::steady_clock::now()},
          refresh_interval_(std::chrono::seconds(30)) {}

    /**
     * Fetch balances from CLOB API (cold path, network I/O).
     * Called at startup and periodically by a background thread.
     */
    Balance fetch_balances() {
        // In production: GET https://api.polymarket.com/v2/balance
        // Returns JSON: {"USDC": 1234.56, "POL": 78.90}
        //
        // Cold path — does network I/O, safe to block.
        double usdc = 0.0;
        double pol = 0.0;

        // Query the CLOB API for token balances
        // This uses the authenticated client (HMAC) to call:
        // GET /v2/balance?tokens=USDC,POL
        auto response = client_.get_balances();

        if (response) {
            usdc = response->usdc_balance;
            pol = response->pol_balance;
        }

        Balance result{usdc, pol, std::chrono::steady_clock::now()};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cached_balance_ = result;
        }
        return result;
    }

    /**
     * Get cached balances (hot path — O(1), thread-safe, no I/O).
     */
    Balance get_cached_balances() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cached_balance_;
    }

    /**
     * Get cached balances (alias for get_cached_balances — hot path, O(1)).
     * Thread-safe, no network I/O. Returns the most recent balance snapshot.
     */
    Balance get_balances() const {
        return get_cached_balances();
    }

    /**
     * Check if balances meet minimum thresholds.
     * Returns true if sufficient, false if below minimum.
     */
    bool check_min_balance() const {
        Balance bal = get_cached_balances();
        return bal.usdc_balance >= config_.min_usdc_balance &&
               bal.pol_balance >= config_.min_pol_balance;
    }

    /**
     * Start background refresh thread (cold path).
     * Call once at startup.
     */
    void start_background_refresh() {
        if (bg_thread_.joinable()) return;
        running_ = true;
        bg_thread_ = std::thread(&BalanceChecker::refresh_loop, this);
    }

    void stop_background_refresh() {
        running_ = false;
        if (bg_thread_.joinable()) {
            bg_thread_.join();
        }
    }

private:
    LightweightCLOBClient& client_;
    const RiskConfig& config_;
    mutable std::mutex mutex_;
    Balance cached_balance_;
    std::chrono::seconds refresh_interval_;
    std::atomic<bool> running_{false};
    std::thread bg_thread_;

    void refresh_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            fetch_balances();
            std::this_thread::sleep_for(refresh_interval_);
        }
    }
};

#endif // BALANCE_CHECKER_HPP
