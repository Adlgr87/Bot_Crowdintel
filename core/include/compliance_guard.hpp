#ifndef COMPLIANCE_GUARD_HPP
#define COMPLIANCE_GUARD_HPP

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

#include "market_metadata.hpp"
#include "market_config.hpp"
#include "tick_result.hpp"
#include "transparent_string_hash.hpp"

/**
 * ComplianceGuard: Verifies market and jurisdiction compliance before trading.
 *
 * CRITICAL: The jurisdiction check is CONFIGURED BY THE OPERATOR via env vars.
 * The code NEVER hardcodes "allow all" — if jurisdiction_check_enabled is true
 * and the country is not in allowed_jurisdictions, the order is blocked.
 * If the operator disables the check, ALL jurisdictions are allowed (operator's
 * responsibility).
 *
 * Cold path: configured once at startup.
 * Hot path: is_token_allowed() and verify_jurisdiction() are O(1).
 */
struct ComplianceConfig {
    bool jurisdiction_check_enabled;
    std::unordered_set<std::string, TransparentStrHash, TransparentStrEq> allowed_jurisdictions;
    std::unordered_set<std::string, TransparentStrHash, TransparentStrEq> restricted_tokens;
    int resolution_warning_hours;

    static ComplianceConfig load_from_env() {
        ComplianceConfig cfg;

        // Jurisdiction check — must be explicitly enabled
        // COMPLIANCE_JURISDICTION_ENABLED (default: true)
        // If true, only allowed jurisdictions can trade.
        // If false, ALL jurisdictions allowed (operator's responsibility).
        const char* env_juris = std::getenv("COMPLIANCE_JURISDICTION_ENABLED");
        cfg.jurisdiction_check_enabled =
            (env_juris == nullptr || std::string(env_juris) == "true" ||
             std::string(env_juris) == "1");

        // Allowed jurisdictions — comma-separated list
        // COMPLIANCE_ALLOWED_JURISDICTION="US,CA,GB" (default: "US")
        const char* env_allowed = std::getenv("COMPLIANCE_ALLOWED_JURISDICTION");
        std::string allowed_str = env_allowed ? env_allowed : "US";
        size_t start = 0, end;
        while ((end = allowed_str.find(',', start)) != std::string::npos) {
            std::string token = allowed_str.substr(start, end - start);
            // Trim whitespace
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            cfg.allowed_jurisdictions.insert(token);
            start = end + 1;
        }
        {
            std::string token = allowed_str.substr(start);
            token.erase(0, token.find_first_not_of(" \t"));
            token.erase(token.find_last_not_of(" \t") + 1);
            cfg.allowed_jurisdictions.insert(token);
        }

        // Restricted tokens — comma-separated list
        // COMPLIANCE_RESTRICTED_TOKENS="token1,token2" (default: empty)
        const char* env_restricted = std::getenv("COMPLIANCE_RESTRICTED_TOKENS");
        if (env_restricted) {
            std::string restricted_str = env_restricted;
            start = 0;
            while ((end = restricted_str.find(',', start)) != std::string::npos) {
                std::string token = restricted_str.substr(start, end - start);
                cfg.restricted_tokens.insert(token);
                start = end + 1;
            }
            {
                std::string token = restricted_str.substr(start);
                cfg.restricted_tokens.insert(token);
            }
        }

        // Resolution warning hours
        // COMPLIANCE_RESOLUTION_WARNING_HOURS (default: 24)
        const char* env_warning = std::getenv("COMPLIANCE_RESOLUTION_WARNING_HOURS");
        cfg.resolution_warning_hours = env_warning ? std::stoi(env_warning) : 24;

        return cfg;
    }
};

/**
 * ComplianceGuard: Enforces market and jurisdiction compliance.
 *
 * Hot path methods (is_token_allowed, verify_jurisdiction) are O(1)
 * using hash sets with transparent lookup (no allocation). Configuration
 * is set at startup (cold path).
 */
class ComplianceGuard {
public:
    explicit ComplianceGuard(const ComplianceConfig& config)
        : config_(config) {}

    /**
     * Check if a token is allowed for trading.
     * Hot path — O(1) hash lookup (transparent, no allocation).
     *
     * @param token_id  The Polymarket token ID (string_view — no heap alloc)
     * @return true if the token is NOT in the restricted list
     */
    bool is_token_allowed(std::string_view token_id) const {
        // Branch-predicted: most tokens are allowed (not in restricted list)
        if (__builtin_expect(config_.restricted_tokens.empty(), 0)) {
            return true;
        }
        return config_.restricted_tokens.find(token_id) ==
               config_.restricted_tokens.end();
    }

    /**
     * Verify that the operator's jurisdiction is allowed.
     * Hot path — O(1) hash lookup (transparent, no allocation).
     *
     * @param country_code  ISO 3166-1 alpha-2 country code (string_view — no heap alloc)
     * @return true if jurisdiction is allowed
     */
    bool verify_jurisdiction(std::string_view country_code) const {
        if (!config_.jurisdiction_check_enabled) {
            // Operator explicitly disabled jurisdiction check
            // This is the operator's responsibility — we warn in logs
            return true;
        }
        if (config_.allowed_jurisdictions.empty()) {
            // Safety: if no jurisdictions configured and check is enabled,
            // block everything (fail-closed)
            return false;
        }
        return config_.allowed_jurisdictions.find(country_code) !=
               config_.allowed_jurisdictions.end();
    }

    /**
     * Check if a market is tradable (active, not resolving soon).
     * Cold path — queries the metadata cache.
     */
    bool verify_market_active(std::string_view token_id,
                               const MarketMetadataCache& cache) const {
        return cache.is_market_tradable(token_id, config_.resolution_warning_hours);
    }

    /**
     * Comprehensive compliance check (hot path — O(1), zero allocation).
     * All parameters are string_view to avoid heap allocation in the hot path.
     */
    TickResult check_all(std::string_view token_id,
                           std::string_view country_code,
                           const MarketMetadataCache& cache) const {
        // 1. Token not restricted — O(1) transparent hash lookup, no alloc
        if (!is_token_allowed(token_id)) {
            return TickResult::MARKET_NOT_TRADABLE;
        }

        // 2. Jurisdiction allowed — O(1) transparent hash lookup, no alloc
        if (!verify_jurisdiction(country_code)) {
            return TickResult::MARKET_NOT_TRADABLE;
        }

        // 3. Market active and not resolving soon — O(1) cache lookup
        if (!cache.is_market_tradable(token_id, config_.resolution_warning_hours)) {
            return TickResult::MARKET_NOT_TRADABLE;
        }

        return TickResult::OK;
    }

    const ComplianceConfig& get_config() const { return config_; }

    std::string get_jurisdiction_status() const {
        if (!config_.jurisdiction_check_enabled) {
            return "disabled (operator responsibility)";
        }
        std::string result = "enabled, allowed: [";
        for (const auto& j : config_.allowed_jurisdictions) {
            result += j + ",";
        }
        if (!config_.allowed_jurisdictions.empty()) result.pop_back();
        result += "]";
        return result;
    }

private:
    const ComplianceConfig config_;
};

#endif // COMPLIANCE_GUARD_HPP
