#ifndef TICK_RESULT_HPP
#define TICK_RESULT_HPP

/**
 * TickResult: Result of evaluating a market signal in the hot path.
 *
 * Extended from the original OK/NO_SIGNAL/NO_EDGE with risk/compliance blocks.
 * Defined in a separate header to avoid circular dependencies between
 * risk_engine.hpp and telemetry.hpp.
 */
enum class TickResult {
    OK,
    NO_SIGNAL,
    NO_EDGE,
    NOT_PROFITABLE,          // net_ev < min_net_ev
    RISK_BLOCKED,            // Violates a risk limit (max_order_usd, rate window, etc.)
    KILL_SWITCH,             // Kill switch active — NO signing, NO submit
    MARKET_NOT_TRADABLE,     // Market closed/resolved/inactive
    DUPLICATE_ORDER,         // Already have an open order for this market/side
    INSUFFICIENT_BALANCE,    // USDC/POL below minimum
    STALE_BOOK,              // Market data too old (feed dead for >feed_dead_timeout_ms)
};

/**
 * Convert TickResult to human-readable string (for logging).
 */
inline const char* tick_result_str(TickResult r) {
    switch (r) {
        case TickResult::OK:                  return "OK";
        case TickResult::NO_SIGNAL:           return "NO_SIGNAL";
        case TickResult::NO_EDGE:             return "NO_EDGE";
        case TickResult::NOT_PROFITABLE:      return "NOT_PROFITABLE";
        case TickResult::RISK_BLOCKED:        return "RISK_BLOCKED";
        case TickResult::KILL_SWITCH:         return "KILL_SWITCH";
        case TickResult::MARKET_NOT_TRADABLE: return "MARKET_NOT_TRADABLE";
        case TickResult::DUPLICATE_ORDER:     return "DUPLICATE_ORDER";
        case TickResult::INSUFFICIENT_BALANCE: return "INSUFFICIENT_BALANCE";
        case TickResult::STALE_BOOK:          return "STALE_BOOK";
    }
    return "UNKNOWN";
}

#endif // TICK_RESULT_HPP
