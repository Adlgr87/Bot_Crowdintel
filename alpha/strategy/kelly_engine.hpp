#ifndef KELLY_ENGINE_HPP
#define KELLY_ENGINE_HPP

#include <algorithm>
#include <cmath>

/**
 * KellyEngine: Calculates the optimal position size based on the Kelly Criterion.
 * Implementation is deterministic and avoids floating point traps.
 */
class KellyEngine {
public:
    // Calculate fractional Kelly to reduce volatility (e.g., 0.1 for 10% Kelly)
    static double calculate_fractional_kelly(double ev, double confidence, double fraction = 0.1) {
        if (ev <= 0) return 0.0;
        
        // Simplified Kelly for binary outcomes: f* = (bp - q) / b
        // where b is the odds, p is probability of win, q is probability of loss
        // Here we approximate using EV and Confidence
        double optimal_f = ev * confidence;
        
        return std::max(0.0, std::min(1.0, optimal_f * fraction));
    }

    // Convert Kelly fraction to USD size
    static double calculate_position_size(double kelly_f, double max_position_usd) {
        return kelly_f * max_position_usd;
    }
};

#endif // KELLY_ENGINE_HPP
