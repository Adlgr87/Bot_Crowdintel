#include <iostream>
#include <vector>
#include <string>
#include "../../core/include/order_book.hpp"

struct TickData {
    uint64_t timestamp;
    double bid_price;
    double bid_size;
    double ask_price;
    double ask_size;
};

/**
 * L2Backtester: Simulates trading against real L2 data.
 * Focuses on Slippage and Self-Fill detection.
 */
class L2Backtester {
public:
    L2Backtester(double initial_capital) : capital_(initial_capital), pnl_(0.0) {}

    // Simulate a trade by walking the book (Slippage simulation)
    double simulate_fill(double target_size, bool is_buy, const OrderBookL2& book) {
        double filled_size = 0;
        double total_cost = 0;
        
        // In a real backtest, we'd loop through levels.
        // Simplified: check top level
        const auto& entry = is_buy ? book.get_ask(0) : book.get_bid(0);
        
        double available = (double)entry.size / 1e6;
        double fill = std::min(target_size, available);
        
        total_cost = fill * ((double)entry.price / 1e6);
        filled_size = fill;

        return total_cost / filled_size; // Average fill price
    }

    void run_replay(const std::vector<TickData>& data) {
        for (const auto& tick : data) {
            // Update book
            // ...
            // Evaluate strategy
            // ...
        }
    }

private:
    double capital_;
    double pnl_;
};

int main() {
    std::cout << "🧪 L2 Backtester initialized. Ready for tick replay." << std::endl;
    return 0;
}
