#ifndef MARKET_MAKING_ENGINE_HPP
#define MARKET_MAKING_ENGINE_HPP

#include <vector>
#include <cstdint>
#include <cmath>
#include "kelly_engine.hpp"
#include "../crowdintel/alpha_receiver.hpp"

/**
 * MarketMakingEngine: Implements a TWAP (Time Weighted Average Price) 
 * mean-reversion strategy around the L2 order book.
 * This engine sits in the Cold Path and generates trading signals for the Hot Path.
 */
class MarketMakingEngine {
public:
    struct Quote {
        uint64_t bid_price;
        uint64_t ask_price;
        double bid_size;
        double ask_size;
    };

    explicit MarketMakingEngine(double initial_twel) : twap_(initial_twel), confidence_(0.90) {}

    // Generates a Quote based on the current order book and alpha signals
    Quote generate_quote(const OrderBookL2& book) {
        const auto& fair_value = book.get_bid(0).price; // Simplified: use best bid as fair price
        
        // Apply a small spread around the fair value
        double half_spread = 0.0005; // 0.05%
        uint64_t bid = static_cast<uint64_t>(fair_value * (1 - half_spread));
        uint64_t ask = static_cast<uint64_t>(fair_value * (1 + half_spread));

        // Use Kelly Criterion for sizing
        double kelly_f = KellyEngine::calculate_fractional_kelly(0.05, confidence_);
        double max_pos = 10000.0;
        
        return {bid, ask, kelly_f * max_pos, kelly_f * max_pos};
    }

    void update_twap(uint64_t new_price) {
        // Simple TWAP update
        twap_ = (twap_ * 0.99) + (static_cast<double>(new_price) * 0.01);
    }

private:
    double twap_;
    double confidence_;
};

#endif // MARKET_MAKING_ENGINE_HPP
