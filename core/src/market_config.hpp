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
    int    feed_dead_timeout_ms        = 5000;    // RISK_FEED_DEAD_TIMEOUT_MS
    int    max_consecutive_rejects     = 5;       // RISK_MAX_CONSECUTIVE_REJECTS
    int    cancellation_window_seconds = 300;     // RISK_CANCELLATION_WINDOW_SECONDS

    // --- Compliance ---
    bool   jurisdiction_check_enabled  = true;    // COMPLIANCE_JURISDICTION_ENABLED
    std::string allowed_jurisdiction    = "US";   // COMPLIANCE_ALLOWED_JURISDICTION
    int    resolution_warning_hours    = 24;      // COMPLIANCE_RESOLUTION_WARNING_HOURS

    // --- Entry Band (adapted from Polywhales policy.ts) ---
    uint64_t min_price_micros          = 350000;  // COMPLIANCE_MIN_PRICE_MICROS (35¢)
    uint64_t max_price_micros          = 700000;  // COMPLIANCE_MAX_PRICE_MICROS (70¢)
    double   max_total_exposure_usd    = 10000.0; // RISK_MAX_TOTAL_EXPOSURE_USD
    double   max_event_cluster_exposure = 5000.0; // RISK_MAX_EVENT_CLUSTER_EXPOSURE
    uint64_t max_daily_notional_usd     = 10000;  // RISK_MAX_DAILY_NOTIONAL_USD

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

    // --- Staleness Thresholds (P0 fixes) ---
    int    max_signal_age_ms           = 500;     // BOT_MAX_SIGNAL_AGE_MS (P0.5: reject stale alpha signals)
    int    max_book_age_ms             = 250;     // BOT_MAX_BOOK_AGE_MS (P0.4: reject stale market data)

    // validate() — Strict range checks for all config fields.
    void validate() const {
        if (max_daily_loss_usd < 0.0) throw std::invalid_argument("max_daily_loss_usd must be >= 0");
        if (max_exposure_per_market < 0.0) throw std::invalid_argument("max_exposure_per_market must be >= 0");
        if (max_exposure_per_side < 0.0) throw std::invalid_argument("max_exposure_per_side must be >= 0");
        if (max_order_usd < 0.0) throw std::invalid_argument("max_order_usd must be >= 0");
        if (max_open_orders < 0) throw std::invalid_argument("max_open_orders must be >= 0");
        if (max_orders_per_min < 0) throw std::invalid_argument("max_orders_per_min must be >= 0");
        if (max_cancels_per_min < 0) throw std::invalid_argument("max_cancels_per_min must be >= 0");
        if (max_price_deviation_bps < 0) throw std::invalid_argument("max_price_deviation_bps must be >= 0");
        if (min_usdc_balance < 0.0) throw std::invalid_argument("min_usdc_balance must be >= 0");
        if (min_pol_balance < 0.0) throw std::invalid_argument("min_pol_balance must be >= 0");
        if (max_position_divergence < 0.0) throw std::invalid_argument("max_position_divergence must be >= 0");
        if (feed_dead_timeout_ms < 0) throw std::invalid_argument("feed_dead_timeout_ms must be >= 0");
        if (max_consecutive_rejects < 0) throw std::invalid_argument("max_consecutive_rejects must be >= 0");
        if (cancellation_window_seconds < 0) throw std::invalid_argument("cancellation_window_seconds must be >= 0");
        if (resolution_warning_hours < 0) throw std::invalid_argument("resolution_warning_hours must be >= 0");
        if (min_price_micros < 1) throw std::invalid_argument("min_price_micros must be >= 1");
        if (max_price_micros < min_price_micros) throw std::invalid_argument("max_price_micros must be >= min_price_micros");
        if (max_total_exposure_usd < 0.0) throw std::invalid_argument("max_total_exposure_usd must be >= 0");
        if (max_event_cluster_exposure < 0.0) throw std::invalid_argument("max_event_cluster_exposure must be >= 0");
        if (max_daily_notional_usd < 0) throw std::invalid_argument("max_daily_notional_usd must be >= 0");
        if (maker_fee_rate < 0.0 || maker_fee_rate > 0.5) throw std::invalid_argument("maker_fee_rate must be in [0, 0.5]");
        if (taker_fee_rate < 0.0 || taker_fee_rate > 0.5) throw std::invalid_argument("taker_fee_rate must be in [0, 0.5]");
        if (base_commission_usd < 0.0) throw std::invalid_argument("base_commission_usd must be >= 0");
        if (gas_cost_usd < 0.0) throw std::invalid_argument("gas_cost_usd must be >= 0");
        if (min_net_ev_usd < 0.0) throw std::invalid_argument("min_net_ev_usd must be >= 0");
        if (clob_rate_limit_per_sec <= 0.0) throw std::invalid_argument("clob_rate_limit_per_sec must be > 0");
        if (clob_burst < 0.0) throw std::invalid_argument("clob_burst must be >= 0");
        if (max_signal_age_ms < 1) throw std::invalid_argument("max_signal_age_ms must be >= 1");
        if (max_book_age_ms < 1) throw std::invalid_argument("max_book_age_ms must be >= 1");
        if (max_signal_age_ms < max_book_age_ms) throw std::invalid_argument("max_signal_age_ms must be >= max_book_age_ms");
    }

    static void validate_bot_config(int bot_mode, double clob_rate, double clob_burst, int signal_age, int book_age) {
        if (bot_mode < 0 || bot_mode > 3) throw std::invalid_argument("BOT_MODE must be 0-3");
        if (clob_rate < 0.1 || clob_rate > 1000.0) throw std::invalid_argument("CLOB_RATE_LIMIT_PER_SEC out of range [0.1, 1000]");
        if (clob_burst < 0.0 || clob_burst > 10000.0) throw std::invalid_argument("CLOB_BURST out of range [0, 10000]");
        if (signal_age < 1 || signal_age > 60000) throw std::invalid_argument("BOT_MAX_SIGNAL_AGE_MS out of range [1, 60000]");
        if (book_age < 1 || book_age > 60000) throw std::invalid_argument("BOT_MAX_BOOK_AGE_MS out of range [1, 60000]");
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

        // Staleness thresholds (P0.4/P0.5)
        cfg.max_signal_age_ms          = get_env_int("BOT_MAX_SIGNAL_AGE_MS", 500);
        cfg.max_book_age_ms            = get_env_int("BOT_MAX_BOOK_AGE_MS", 250);

        // Entry Band
        cfg.min_price_micros           = static_cast<uint64_t>(get_env_double("COMPLIANCE_MIN_PRICE_MICROS", 350000.0));
        cfg.max_price_micros           = static_cast<uint64_t>(get_env_double("COMPLIANCE_MAX_PRICE_MICROS", 700000.0));
        cfg.max_total_exposure_usd     = get_env_double("RISK_MAX_TOTAL_EXPOSURE_USD", 10000.0);
        cfg.max_event_cluster_exposure = get_env_double("RISK_MAX_EVENT_CLUSTER_EXPOSURE", 5000.0);
        cfg.max_daily_notional_usd     = static_cast<uint64_t>(get_env_double("RISK_MAX_DAILY_NOTIONAL_USD", 10000.0));

        return cfg;
    }
};

#endif // MARKET_CONFIG_HPP
