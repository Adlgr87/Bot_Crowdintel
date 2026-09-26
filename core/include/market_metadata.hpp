#ifndef MARKET_METADATA_HPP
#define MARKET_METADATA_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <vector>

/**
 * MarketMetadata: Metadata fetched from Polymarket CLOB API at startup (cold path).
 *
 * Contains: tick size, market status, resolution time, and dynamic fee flag.
 * Stored in a cache, accessed by ComplianceGuard and ExecutionEngine.
 *
 * API: GET https://api.polymarket.com/v2/market/{token_id}
 * Response: { "token_id": "...", "question": "...", "tick_size": 100,
 *             "status": "active", "resolution_time": "2025-...", ... }
 */
struct MarketMetadata {
    std::string token_id;
    std::string question;
    std::string outcome_token;       // "Yes" / "No"
    int tick_size;                   // in micro-units (e.g., 100 = 0.0001 price)
    std::string status;              // "active", "closed", "resolved"
    std::chrono::system_clock::time_point resolution_time;
    std::optional<std::string> resolution_src;
    std::optional<std::string> resolution_criteria;
    bool enable_dynamic_fees;      // per-market flag for FeeModel T4-1

    bool is_active() const {
        return status == "active";
    }

    bool is_resolving_soon(int warning_hours) const {
        auto now = std::chrono::system_clock::now();
        auto hours_until_resolution = std::chrono::duration<double, std::ratio<3600>>(
            resolution_time - now).count();
        return hours_until_resolution < static_cast<double>(warning_hours);
    }
};

/**
 * MarketMetadataCache: Thread-safe cache of market metadata.
 * Cold path: fetched at startup and updated periodically.
 * Hot path: O(1) lookups via unordered_map.
 */
class MarketMetadataCache {
public:
    MarketMetadataCache() = default;

    /**
     * Fetch metadata for a market (cold path, network I/O).
     * In production: GET /v2/market/{token_id}
     */
    void fetch_metadata(const std::string& token_id) {
        // In production, this calls the CLOB API.
        // For testing: loads from a config file or mock.
        MarketMetadata metadata;
        metadata.token_id = token_id;
        metadata.tick_size = 100;           // 0.0001
        metadata.status = "active";
        metadata.enable_dynamic_fees = false;
        metadata.resolution_time = std::chrono::system_clock::time_point::max();
        metadata.resolution_src = std::nullopt;
        metadata.resolution_criteria = std::nullopt;

        std::unique_lock<std::shared_mutex> lock(mutex_);
        cache_[token_id] = metadata;
    }

    /**
     * Set/inject metadata directly (cold path).
     * Used by tests and initial config loading to populate the cache
     * with custom market states (e.g. "closed", custom tick_size).
     */
    void set_metadata(const MarketMetadata& metadata) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        cache_[metadata.token_id] = metadata;
    }

    /**
     * Get metadata for a market (hot path — O(1) lookup).
     * Returns nullptr if not found.
     */
    const MarketMetadata* get(const std::string& token_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = cache_.find(token_id);
        if (it != cache_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * Apply tick size to a raw price (snaps to tick).
     * Hot path — O(1).
     */
    static uint64_t apply_tick_size(uint64_t raw_price, int tick_size) {
        if (tick_size <= 0) return raw_price;
        return (raw_price / tick_size) * tick_size;
    }

    /**
     * Check if a market is tradable (not closed/resolved, not resolving soon).
     * Hot path — O(1) lookup + comparisons.
     */
    bool is_market_tradable(const std::string& token_id, int warning_hours) const {
        const MarketMetadata* meta = get(token_id);
        if (!meta) return false;  // Unknown market = not tradable (safe default)
        if (!meta->is_active()) return false;
        if (meta->is_resolving_soon(warning_hours)) return false;
        return true;
    }

    /**
     * Get tick size for a market (hot path).
     * Returns 1 (no snapping) if unknown.
     */
    int get_tick_size(const std::string& token_id) const {
        const MarketMetadata* meta = get(token_id);
        return meta ? meta->tick_size : 1;
    }

    /**
     * Get dynamic fees flag for a market.
     * Hot path.
     */
    bool get_dynamic_fees(const std::string& token_id) const {
        const MarketMetadata* meta = get(token_id);
        return meta ? meta->enable_dynamic_fees : false;
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return cache_.size();
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, MarketMetadata> cache_;
};

#endif // MARKET_METADATA_HPP
