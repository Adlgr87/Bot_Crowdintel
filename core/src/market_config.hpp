#ifndef MARKET_CONFIG_HPP
#define MARKET_CONFIG_HPP

#include <string>
#include <cstdlib>
#include <stdexcept>
#include <cstdint>

/**
 * MarketConfig / RiskConfig: All risk limits are loaded from environment variables
 * with conservative defaults. Every constant is overridable by the operator.
 *
 * Cold path: loaded once at startup, not in the hot path loop.
 * Hot path reads atomic pointers / const references to these values.
 */
struct RiskConfig {
    // --- Financial Limits ---
    double max_daily_loss_usd          = 500.0;   // RISK_MAX_DAILY_LOSS_USD
    double max_exposure_per_market     = 5000.0;  // RISK_MAX_EXPOSURE_PER_MARKET
    double max_exposure_per_side       = 2000.0;  // RISK_MAX_EXPOSURE_PER_SIDE
    double max_order_usd               = 500.0;   // RISK_MAX_ORDER_USD
    int    max_open_orders             = 5;       // RISK_MAX_OPEN_ORDERS
    int    max_orders_per_min          = 10;      // RISK_MAX_ORDERS_PER_MIN
    int    max_cancels_per_min         = 20;      // RISK_MAX_CANCELS_PER_MIN
    int    max_price_deviation_bps     = 500;     // RISK_MAX_PRICE_DEVIATION_BPS
    double min_usdc_balance            = 100.0;   // RISK_MIN_USDC_BALANCE
    double min_pol_balance             = 10.0;    // RISK_MIN_POL_BALANCE
    double max_position_divergence     = 0.01;    // RISK_MAX_POSITION_DIVERGENCE

    // --- Operational Flags ---
    bool   kill_switch_enabled         = true;    // RISK_KILL_SWITCH_ENABLED
    std::string bot_mode               = "live"; // BOT_MODE (live, paper, backtest, shadow)
    bool   shadow_mode                 = false;   // true when bot_mode == "shadow" (P1.1)
    int    feed_dead_timeout_ms        = 5000;    // RISK_FEED_DEAD_TIMEOUT_MS
    int    max_consecutive_rejects     = 5;       // RISK_MAX_CONSECUTIVE_REJECTS
    int    cancellation_window_seconds = 300;     // RISK_CANCELLATION_WINDOW_SECONDS

    // --- Compliance ---
    bool   jurisdiction_check_enabled  = true;    // COMPLIANCE_JURISDICTION_ENABLED
    std::string allowed_jurisdiction    = "US";   // COMPLIANCE_ALLOWED_JURISDICTION
    int    resolution_warning_hours    = 24;      // COMPLIANCE_RESOLUTION_WARNING_HOURS

    // --- Fee Model ---
    double maker_fee_rate              = 0.020;   // FEE_MAKER_RATE (2.0%)
    double taker_fee_rate              = 0.035;   // FEE_TAKER_RATE (3.5%)
    double base_commission_usd         = 0.10;    // FEE_BASE_USD
    bool   dynamic_fees_enabled        = false;   // FEE_DYNAMIC_ENABLED
    double dynamic_C                   = 0.075;   // FEE_DYNAMIC_C
    double gas_cost_usd                = 0.005;    // GAS_COST_USD (Polygon)
    double min_net_ev_usd              = 0.50;    // FEE_MIN_NET_EV_USD

    // --- Rate Limits (per endpoint) ---
    double clob_rate_limit_per_sec     = 1.0;     // CLOB_RATE_LIMIT_PER_SEC
    double clob_burst                  = 2.0;     // CLOB_BURST

    // --- P0.5: Staleness Thresholds ---
    // Signals older than max_signal_age_ms are rejected (P0.5 stale check).
    // Books older than max_book_age_ms are rejected (stale data).
    // Both defaults tuned for Polymarket CLOB V2 latency profile.
    uint64_t max_signal_age_ms         = 500;     // BOT_MAX_SIGNAL_AGE_MS
    uint64_t max_book_age_ms           = 250;     // BOT_MAX_BOOK_AGE_MS

    /**
     * validate() — Runtime validation of config ranges and enums.
     * Returns true if all fields are within valid ranges.
     * Error message written to 'error' parameter if validation fails.
     *
     * P0.6: Strict validation to prevent misconfiguration.
     */
    bool validate(std::string& error) const {
        if (max_daily_loss_usd < 0) {
            error = "max_daily_loss_usd must be >= 0, got " + std::to_string(max_daily_loss_usd);
            return false;
        }
        if (max_exposure_per_market < 0) {
            error = "max_exposure_per_market must be >= 0";
            return false;
        }
        if (max_order_usd <= 0) {
            error = "max_order_usd must be > 0";
            return false;
        }
        if (min_usdc_balance < 0) {
            error = "min_usdc_balance must be >= 0";
            return false;
        }
        if (min_pol_balance < 0) {
            error = "min_pol_balance must be >= 0";
            return false;
        }
        if (min_net_ev_usd < 0) {
            error = "min_net_ev_usd must be >= 0";
            return false;
        }
        if (max_price_deviation_bps <= 0) {
            error = "max_price_deviation_bps must be > 0";
            return false;
        }
        if (max_position_divergence < 0) {
            error = "max_position_divergence must be >= 0";
            return false;
        }
        if (maker_fee_rate < 0 || maker_fee_rate > 1) {
            error = "maker_fee_rate must be in [0, 1]";
            return false;
        }
        if (taker_fee_rate < 0 || taker_fee_rate > 1) {
            error = "taker_fee_rate must be in [0, 1]";
            return false;
        }
        if (max_open_orders < 0) {
            error = "max_open_orders must be >= 0";
            return false;
        }
        if (max_orders_per_min < 0) {
            error = "max_orders_per_min must be >= 0";
            return false;
        }
        if (max_cancels_per_min < 0) {
            error = "max_cancels_per_min must be >= 0";
            return false;
        }
        if (max_signal_age_ms < 1) {
            error = "max_signal_age_ms must be >= 1";
            return false;
        }
        if (max_book_age_ms < 1) {
            error = "max_book_age_ms must be >= 1";
            return false;
        }
        return true;
    }

    /**
     * validate_bot_config() — Static validation of environment-driven bot config.
     * Used at startup to fail fast on misconfiguration.
     *
     * P0.6: Checks BOT_* env vars including:
     *   - BOT_TICK_SIZE: must be > 0
     *   - BOT_KELLY_FRACTION: must be in [0, 1]
     *   - BOT_ORDER_TYPE: must be GTC, GTD, FOK, or FAK
     */
    static bool validate_bot_config(std::string& error) {
        const char* tick_size_str = std::getenv("BOT_TICK_SIZE");
        if (tick_size_str != nullptr) {
            try {
                int ts = std::stoi(tick_size_str);
                if (ts < 0) {
                    error = "BOT_TICK_SIZE must be >= 0";
                    return false;
                }
            } catch (...) {
                error = "BOT_TICK_SIZE must be a valid integer";
                return false;
            }
        }

        const char* kelly_str = std::getenv("BOT_KELLY_FRACTION");
        if (kelly_str != nullptr) {
            try {
                double kf = std::stod(kelly_str);
                if (kf < 0.0 || kf > 1.0) {
                    error = "BOT_KELLY_FRACTION must be in [0, 1]";
                    return false;
                }
            } catch (...) {
                error = "BOT_KELLY_FRACTION must be a valid float";
                return false;
            }
        }

        const char* order_type_str = std::getenv("BOT_ORDER_TYPE");
        if (order_type_str != nullptr) {
            std::string ot(order_type_str);
            if (ot != "GTC" && ot != "GTD" && ot != "FOK" && ot != "FAK") {
                error = "BOT_ORDER_TYPE must be one of: GTC, GTD, FOK, FAK";
                return false;
            }
        }

        const char* mode_str = std::getenv("BOT_MODE");
        if (mode_str != nullptr) {
            std::string mode(mode_str);
            if (mode != "live" && mode != "paper" && mode != "backtest" && mode != "shadow") {
                error = "BOT_MODE must be one of: live, paper, backtest, shadow";
                return false;
            }
        }

        const char* signal_age_str = std::getenv("BOT_MAX_SIGNAL_AGE_MS");
        if (signal_age_str != nullptr) {
            try {
                long sa = std::stol(signal_age_str);
                if (sa < 1) {
                    error = "BOT_MAX_SIGNAL_AGE_MS must be >= 1";
                    return false;
                }
            } catch (...) {
                error = "BOT_MAX_SIGNAL_AGE_MS must be a valid integer";
                return false;
            }
        }

        return true;
    }

    static RiskConfig load_from_env() {
        RiskConfig cfg;

        // Helper lambda — reads env with fallback
        auto get_env = [](const char* name, const char* fallback) -> std::string {
            const char* val = std::getenv(name);
            return (val != nullptr) ? std::string(val) : std::string(fallback);
        };

        auto get_env_double = [&](const char* name, double fallback) -> double {
            std::string s = get_env(name, "");
            if (s.empty()) return fallback;
            try { return std::stod(s); } catch (...) { return fallback; }
        };

        auto get_env_int = [&](const char* name, int fallback) -> int {
            std::string s = get_env(name, "");
            if (s.empty()) return fallback;
            try { return std::stoi(s); } catch (...) { return fallback; }
        };

        auto get_env_uint64 = [&](const char* name, uint64_t fallback) -> uint64_t {
            std::string s = get_env(name, "");
            if (s.empty()) return fallback;
            try { return static_cast<uint64_t>(std::stoull(s)); } catch (...) { return fallback; }
        };

        auto get_env_bool = [&](const char* name, bool fallback) -> bool {
            std::string s = get_env(name, "");
            if (s.empty()) return fallback;
            return (s == "true" || s == "1" || s == "TRUE" || s == "True");
        };

        cfg.max_daily_loss_usd       = get_env_double("RISK_MAX_DAILY_LOSS_USD", 500.0);
        cfg.max_exposure_per_market    = get_env_double("RISK_MAX_EXPOSURE_PER_MARKET", 5000.0);
        cfg.max_exposure_per_side      = get_env_double("RISK_MAX_EXPOSURE_PER_SIDE", 2000.0);
        cfg.max_order_usd              = get_env_double("RISK_MAX_ORDER_USD", 500.0);
        cfg.max_open_orders            = get_env_int("RISK_MAX_OPEN_ORDERS", 5);
        cfg.max_orders_per_min         = get_env_int("RISK_MAX_ORDERS_PER_MIN", 10);
        cfg.max_cancels_per_min        = get_env_int("RISK_MAX_CANCELS_PER_MIN", 20);
        cfg.max_price_deviation_bps    = get_env_int("RISK_MAX_PRICE_DEVIATION_BPS", 500);
        cfg.min_usdc_balance           = get_env_double("RISK_MIN_USDC_BALANCE", 100.0);
        cfg.min_pol_balance            = get_env_double("RISK_MIN_POL_BALANCE", 10.0);
        cfg.max_position_divergence    = get_env_double("RISK_MAX_POSITION_DIVERGENCE", 0.01);

        cfg.kill_switch_enabled        = get_env_bool("RISK_KILL_SWITCH_ENABLED", true);

        // P1.1: Bot execution mode — determines whether orders are actually submitted.
        // "shadow" = evaluate signals + compute order params, but do NOT submit.
        cfg.bot_mode                   = get_env("BOT_MODE", "live");
        cfg.shadow_mode                = (cfg.bot_mode == "shadow");
        cfg.feed_dead_timeout_ms       = get_env_int("RISK_FEED_DEAD_TIMEOUT_MS", 5000);
        cfg.max_consecutive_rejects    = get_env_int("RISK_MAX_CONSECUTIVE_REJECTS", 5);
        cfg.cancellation_window_seconds = get_env_int("RISK_CANCELLATION_WINDOW_SECONDS", 300);

        cfg.jurisdiction_check_enabled = get_env_bool("COMPLIANCE_JURISDICTION_ENABLED", true);
        cfg.allowed_jurisdiction       = get_env("COMPLIANCE_ALLOWED_JURISDICTION", "US");
        cfg.resolution_warning_hours   = get_env_int("COMPLIANCE_RESOLUTION_WARNING_HOURS", 24);

        cfg.maker_fee_rate             = get_env_double("FEE_MAKER_RATE", 0.020);
        cfg.taker_fee_rate             = get_env_double("FEE_TAKER_RATE", 0.035);
        cfg.base_commission_usd        = get_env_double("FEE_BASE_USD", 0.10);
        cfg.dynamic_fees_enabled       = get_env_bool("FEE_DYNAMIC_ENABLED", false);
        cfg.dynamic_C                  = get_env_double("FEE_DYNAMIC_C", 0.075);
        cfg.gas_cost_usd               = get_env_double("GAS_COST_USD", 0.005);
        cfg.min_net_ev_usd             = get_env_double("FEE_MIN_NET_EV_USD", 0.50);

        cfg.clob_rate_limit_per_sec    = get_env_double("CLOB_RATE_LIMIT_PER_SEC", 1.0);
        cfg.clob_burst                 = get_env_double("CLOB_BURST", 2.0);

<<<<<<< Updated upstream
=======
        // Entry Band
        cfg.min_price_micros           = static_cast<uint64_t>(get_env_double("COMPLIANCE_MIN_PRICE_MICROS", 350000.0));
        cfg.max_price_micros           = static_cast<uint64_t>(get_env_double("COMPLIANCE_MAX_PRICE_MICROS", 700000.0));
        cfg.max_total_exposure_usd     = get_env_double("RISK_MAX_TOTAL_EXPOSURE_USD", 10000.0);
        cfg.max_event_cluster_exposure = get_env_double("RISK_MAX_EVENT_CLUSTER_EXPOSURE", 5000.0);
        cfg.max_daily_notional_usd     = static_cast<uint64_t>(get_env_double("RISK_MAX_DAILY_NOTIONAL_USD", 10000.0));

        // P0.5: Staleness thresholds (BOT_MAX_SIGNAL_AGE_MS, BOT_MAX_BOOK_AGE_MS)
        cfg.max_signal_age_ms          = get_env_uint64("BOT_MAX_SIGNAL_AGE_MS", 500);
        cfg.max_book_age_ms            = get_env_uint64("BOT_MAX_BOOK_AGE_MS", 250);

>>>>>>> Stashed changes
        return cfg;
    }
};

#endif // MARKET_CONFIG_HPP
