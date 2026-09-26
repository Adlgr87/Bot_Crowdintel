/**
 * Paper Trading Simulation Runner — CrowdIntel Bot (C++20)
 *
 * Self-contained simulation for Polymarket CLOB V2 paper-trading.
 * NO real credentials, NO real network I/O — all trades are simulated.
 *
 * This simulator exercises the real OrderBookL2 and TickResult types
 * from the production code, plus a SimRiskEngine that mirrors the
 * production RiskEngine's checks (entry band, staleness, exposure, rate window).
 *
 * Build:
 *   g++ -std=c++20 -O2 -I../../core/include \
 *       tests/sim/paper_trading_main.cpp -o bin/paper_sim -lpthread
 *
 * Run:
 *   ./bin/paper_sim --ticks=1000 --markets=5 --seed=42
 */

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "order_book.hpp"
#include "tick_result.hpp"

// ─── Simulation Config ────────────────────────────────────────────────────
struct SimConfig {
    uint64_t seed = 42;
    int num_markets = 5;
    int num_ticks = 1000;
    double initial_capital_usd = 100000.0;
    double min_ev_threshold = 0.02;
    double entry_floor = 0.35;   // 35 cents
    double entry_cap = 0.70;     // 70 cents
    bool enable_noise = true;
    double noise_stddev = 0.005;
};

// ─── Market State (wraps OrderBookL2 which has non-copyable atomics) ──────
struct SimMarket {
    double base_price;
    double current_position = 0.0;
    double entry_price = 0.0;
    double realized_pnl = 0.0;
    int fills = 0;
    int skips = 0;
    int stale_hits = 0;
    bool is_stale_flag = false;
    std::chrono::steady_clock::time_point last_update;
    std::unique_ptr<OrderBookL2> book;  // Use unique_ptr since OrderBookL2 has non-movable atomics

    explicit SimMarket(double price) : base_price(price), book(std::make_unique<OrderBookL2>()) {
        last_update = std::chrono::steady_clock::now();
        refresh_book();
    }

    void refresh_book() {
        uint64_t ts = current_time_ns();
        for (int level = 0; level < 10; level++) {
            double spread = 0.01 * (level + 1);
            double bid_px = base_price - spread * 0.5;
            double ask_px = base_price + spread * 0.5;
            uint64_t bz = static_cast<uint64_t>(1000 * (11 - level));
            book->update_bid(level, static_cast<uint64_t>(bid_px * 1e6), bz, ts);
            book->update_ask(level, static_cast<uint64_t>(ask_px * 1e6), bz, ts);
        }
        last_update = std::chrono::steady_clock::now();
    }
};

// ─── Signal Generator ────────────────────────────────────────────────────
struct SimSignal {
    int market_id;
    uint8_t side;        // 0=buy, 1=sell
    uint64_t size_usd;   // USD value (* 1e6 for fixed-point)
    double ev_per_dollar;

    std::string market_slug() const {
        return "SIM-" + std::to_string(market_id) + "-" + (side == 0 ? "UP" : "DOWN");
    }
};

class SignalGenerator {
public:
    explicit SignalGenerator(uint64_t seed, double min_ev, double floor, double cap)
        : rng_(seed), ev_noise_(-0.02, 0.02), size_dist_(50.0, 500.0),
          floor_(floor), cap_(cap) {}

    SimSignal generate(int num_markets_actual,
                       const std::vector<SimMarket>& markets) {
        int market_id = rng_() % num_markets_actual;
        uint8_t side = (rng_() % 2);
        double size_usd = size_dist_(rng_);

        double price = markets[market_id].base_price;
        double ev;
        if (side == 0) {
            ev = (floor_ + 0.10 - price) * 3.0 + ev_noise_(rng_);
        } else {
            ev = (price - cap_ + 0.10) * 3.0 + ev_noise_(rng_);
        }

        return SimSignal{market_id, side,
                         static_cast<uint64_t>(size_usd * 1e6), ev};
    }

    void walk_market(SimMarket& mkt, double stddev) {
        double noise = std::normal_distribution<double>(0.0, stddev)(rng_);
        mkt.base_price += noise;
        mkt.base_price = std::max(0.01, std::min(0.99, mkt.base_price));
        mkt.refresh_book();
    }

private:
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> ev_noise_;
    std::uniform_real_distribution<double> size_dist_;
    double floor_;
    double cap_;
};

// ─── Risk Engine (mirrors production RiskEngine checks) ─────────────────
class SimRiskEngine {
public:
    explicit SimRiskEngine(const SimConfig& cfg) : cfg_(cfg) {}

    TickResult pre_trade_check(const SimSignal& sig,
                                const SimMarket& mkt,
                                double /*usd_balance*/,
                                double& exposure) {
        // 1. Kill switch
        if (kill_switched_) return TickResult::RISK_BLOCKED;

        // 2. Book staleness check (mirrors OrderBookL2::is_stale)
        if (mkt.is_stale_flag) {
            return TickResult::STALE_BOOK;
        }
        const auto& best_bid = mkt.book->get_bid(0);
        const auto& best_ask = mkt.book->get_ask(0);
        uint64_t latest_ts = (best_bid.timestamp > best_ask.timestamp)
                             ? best_bid.timestamp : best_ask.timestamp;
        if ((current_time_ns() - latest_ts) > 90'000'000'000ULL) {
            return TickResult::STALE_BOOK;
        }

        // 3. Entry band check (Polywhales policy: 35-70 cents)
        double current_price = mkt.base_price;
        if (current_price < cfg_.entry_floor || current_price > cfg_.entry_cap) {
            return TickResult::RISK_BLOCKED;
        }

        // 4. Exposure check
        double usd_size = static_cast<double>(sig.size_usd) / 1e6;
        if (exposure + usd_size > cfg_.initial_capital_usd * 0.20) {
            return TickResult::RISK_BLOCKED;
        }

        // 5. Rate window check (60s sliding)
        if (!check_rate_window()) {
            return TickResult::RISK_BLOCKED;
        }

        // 6. EV threshold
        if (sig.ev_per_dollar < cfg_.min_ev_threshold) {
            return TickResult::NO_SIGNAL;
        }

        exposure += usd_size;
        record_order();
        return TickResult::OK;
    }

    void kill_switch(bool on) { kill_switched_ = on; }
    bool kill_switched() const { return kill_switched_; }

private:
    SimConfig cfg_;
    std::atomic<bool> kill_switched_{false};
    std::chrono::steady_clock::time_point window_start_{std::chrono::steady_clock::now()};
    int orders_in_window_ = 0;

    bool check_rate_window() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - window_start_);
        if (elapsed.count() > 60) {
            window_start_ = now;
            orders_in_window_ = 0;
        }
        return orders_in_window_ < 1000;
    }

    void record_order() { orders_in_window_++; }
};

// ─── Simulation Runner ───────────────────────────────────────────────────
class PaperTradingRunner {
public:
    explicit PaperTradingRunner(const SimConfig& cfg)
        : cfg_(cfg), gen_(cfg.seed, cfg.min_ev_threshold, cfg.entry_floor, cfg.entry_cap),
          risk_(cfg) {
        for (int i = 0; i < cfg.num_markets; i++) {
            // First half: below floor (mean-reversion buy signal)
            // Second half: above cap (mean-reversion sell signal)
            // Middle: inside entry band
            double base;
            if (i < cfg.num_markets / 3) {
                base = cfg.entry_floor - 0.05 - (i * 0.03);  // 30¢, 27¢, ...
            } else if (i >= 2 * cfg.num_markets / 3) {
                base = cfg.entry_cap + 0.05 + (i * 0.03);  // 75¢, 78¢, ...
            } else {
                base = cfg.entry_floor + 0.15 + (i * 0.05);  // Inside band
            }
            base = std::max(0.01, std::min(0.99, base));
            markets_.emplace_back(base);
        }
    }

    void run() {
        std::cout << "\n🧪 CrowdIntel Bot — Paper Trading Simulation (Polymarket CLOB V2)" << std::endl;
        std::cout << "════════════════════════════════════════════════════════════════════" << std::endl;
        std::cout << "  Markets:            " << cfg_.num_markets << std::endl;
        std::cout << "  Max ticks:          " << cfg_.num_ticks << std::endl;
        std::cout << "  Seed:               " << cfg_.seed << std::endl;
        std::cout << "  Capital:            $" << std::fixed << std::setprecision(2) << cfg_.initial_capital_usd << std::endl;
        std::cout << "  Entry band:         [$" << cfg_.entry_floor << ", $" << cfg_.entry_cap << "]" << std::endl;
        std::cout << "  Min EV/USD:         $" << cfg_.min_ev_threshold << std::endl;
        std::cout << "  Kill switch:        " << (risk_.kill_switched() ? "ON" : "OFF") << std::endl;
        std::cout << "════════════════════════════════════════════════════════════════════" << std::endl;
        std::cout << std::endl;

        double usd_balance = cfg_.initial_capital_usd;
        double total_exposure = 0.0;
        int total_signals = 0;
        int total_fills = 0;
        int total_skips = 0;
        int total_stale = 0;
        int total_blocked = 0;
        int total_no_signal = 0;
        double total_realized_pnl = 0.0;
        std::vector<double> fill_prices;
        std::vector<double> fill_sizes;

        std::mt19937_64 fill_rng(cfg_.seed + 999);

        for (int tick = 0; tick < cfg_.num_ticks; tick++) {
            // 1. Walk market prices
            if (cfg_.enable_noise) {
                for (auto& mkt : markets_) {
                    gen_.walk_market(mkt, cfg_.noise_stddev);
                }
            }

            // 2. Occasionally inject stale books
            if (cfg_.enable_noise && tick > 0 && tick % 200 == 0) {
                int stale_mkt = (tick / 200) % cfg_.num_markets;
                markets_[stale_mkt].is_stale_flag = true;
                std::cout << "  ⚠️  TICK " << std::setw(4) << tick
                          << " | FEED DEATH on market " << stale_mkt
                          << " (book marked stale)" << std::endl;
            }

            // 3. Generate signal
            SimSignal sig = gen_.generate(markets_.size(), markets_);
            total_signals++;

            // 4. Run risk checks
            SimMarket& mkt = markets_[sig.market_id];
            double usd_size = static_cast<double>(sig.size_usd) / 1e6;

            TickResult result = risk_.pre_trade_check(sig, mkt, usd_balance, total_exposure);

            switch (result) {
                case TickResult::OK: {
                    double fill_price = (sig.side == 0)
                        ? mkt.base_price + 0.003
                        : mkt.base_price - 0.003;
                    if (std::normal_distribution<double>(0.0, 1.0)(fill_rng) < 0.75) {
                        total_fills++;
                        usd_balance -= usd_size;
                        fill_prices.push_back(fill_price);
                        fill_sizes.push_back(usd_size);
                        mkt.fills++;
                        // Track position as quantity (units), not USD
                        double fill_qty = usd_size / fill_price;  // Convert USD to units
                        mkt.current_position += (sig.side == 0) ? fill_qty : -fill_qty;

                        // Track weighted average entry price
                        if (mkt.current_position != 0.0) {
                            double new_total_cost = mkt.realized_pnl + usd_size;
                            // Simplified: use fill_price as entry for this fill
                            if (sig.side == 0) {
                                if (mkt.entry_price == 0.0 || mkt.current_position == fill_qty) {
                                    mkt.entry_price = fill_price;
                                } else {
                                    // Weighted average
                                    double avg = (mkt.entry_price * (mkt.current_position - fill_qty) +
                                                 fill_price * fill_qty) / mkt.current_position;
                                    mkt.entry_price = avg;
                                }
                            }
                        }

                        // Simulate mean reversion exit (50% chance after each fill)
                        if (mkt.current_position != 0.0 && mkt.entry_price > 0 &&
                            std::normal_distribution<double>(0.0, 1.0)(fill_rng) < 0.5) {
                            double exit_price = mkt.base_price;
                            double pnl = mkt.current_position * (exit_price - mkt.entry_price);
                            mkt.realized_pnl += pnl;
                            total_realized_pnl += pnl;
                            usd_balance += usd_size;  // Return capital
                            total_exposure -= usd_size;
                            mkt.current_position = 0.0;
                            mkt.entry_price = 0.0;
                        }

                        std::cout << "  🔵 TICK " << std::setw(4) << tick
                                  << " | BUY  " << sig.market_slug()
                                  << " | $" << std::setprecision(3) << fill_price
                                  << " | $" << std::setprecision(2) << usd_size
                                  << " | EV=$" << std::setprecision(4) << sig.ev_per_dollar
                                  << std::endl;
                    } else {
                        total_skips++;
                        mkt.skips++;
                        total_exposure -= usd_size;
                        std::cout << "  🟡 TICK " << std::setw(4) << tick
                                  << " | PASS " << sig.market_slug()
                                  << " | EV=$" << std::setprecision(4) << sig.ev_per_dollar
                                  << " | no fill (slippage)" << std::endl;
                    }
                    break;
                }
                case TickResult::STALE_BOOK:
                    total_stale++;
                    mkt.stale_hits++;
                    std::cout << "  ⚫ TICK " << std::setw(4) << tick
                              << " | SKIP " << sig.market_slug()
                              << " | stale book (feed dead)" << std::endl;
                    break;
                case TickResult::RISK_BLOCKED:
                    total_blocked++;
                    break;
                case TickResult::INSUFFICIENT_BALANCE:
                    total_blocked++;
                    break;
                case TickResult::NO_SIGNAL:
                    total_no_signal++;
                    total_skips++;
                    break;
                default:
                    total_blocked++;
                    break;
            }

            // Clear stale flag
            if (tick > 0 && tick % 200 == 110) {
                for (auto& mkt : markets_) mkt.is_stale_flag = false;
            }
        }

        print_report(total_signals, total_fills, total_skips, total_stale,
                      total_blocked, total_no_signal, total_exposure,
                      usd_balance, fill_prices, fill_sizes, total_realized_pnl);
    }

private:
    SimConfig cfg_;
    SignalGenerator gen_;
    SimRiskEngine risk_;
    std::vector<SimMarket> markets_;

    void print_report(int signals, int fills, int skips, int stales,
                       int blocked, int no_signal, double exposure,
                       double final_balance,
                       const std::vector<double>& fill_prices,
                       const std::vector<double>& fill_sizes,
                       double realized_pnl) {

        double fill_rate = signals > 0 ? (100.0 * fills / signals) : 0.0;
        double avg_fill_price = fill_prices.empty() ? 0.0
            : std::accumulate(fill_prices.begin(), fill_prices.end(), 0.0) / fill_prices.size();
        double total_volume = std::accumulate(fill_sizes.begin(), fill_sizes.end(), 0.0);

        std::cout << "\n" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "📊 SIMULATION REPORT — CrowdIntel Bot Paper Trading (Polymarket CLOB V2)" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << std::endl;
        std::cout << "  Signal Flow:" << std::endl;
        std::cout << "    Total signals:         " << signals << std::endl;
        std::cout << "    Fills executed:        " << fills << std::endl;
        std::cout << "    Fill rate:             " << std::fixed << std::setprecision(1) << fill_rate << "%" << std::endl;
        std::cout << std::endl;
        std::cout << "  Risk Management:" << std::endl;
        std::cout << "    Stale books blocked:   " << stales << std::endl;
        std::cout << "    Risk/exposure blocked: " << blocked << std::endl;
        std::cout << "    Below EV threshold:    " << no_signal << std::endl;
        std::cout << "    Slippage/no fill:      " << skips << std::endl;
        std::cout << std::endl;
        std::cout << "  P&L Overview:" << std::endl;
        std::cout << "    Initial capital:       $" << std::setprecision(2) << cfg_.initial_capital_usd << std::endl;
        std::cout << "    Final balance:         $" << std::setprecision(2) << final_balance << std::endl;
        std::cout << "    Total exposure:        $" << std::setprecision(2) << exposure << std::endl;
        std::cout << "    Total volume traded:   $" << std::setprecision(2) << total_volume << std::endl;
        std::cout << "    Realized P&L:          $" << std::setprecision(2) << realized_pnl << std::endl;
        std::cout << "    Avg fill price:        $" << std::setprecision(3) << avg_fill_price << std::endl;
        std::cout << std::endl;

        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "✅ Simulation complete." << std::endl;

        if (exposure > cfg_.initial_capital_usd * 0.25) {
            std::cout << "⚠️  WARNING: Exposure exceeded 25% of capital — consider tightening risk limits." << std::endl;
        }
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    }
};

// ─── Main ─────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    SimConfig cfg;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--ticks=") == 0) cfg.num_ticks = std::stoi(arg.substr(8));
        else if (arg.find("--markets=") == 0) cfg.num_markets = std::stoi(arg.substr(10));
        else if (arg.find("--seed=") == 0) cfg.seed = std::stoull(arg.substr(7));
        else if (arg.find("--capital=") == 0) cfg.initial_capital_usd = std::stod(arg.substr(10));
        else if (arg == "--no-noise") cfg.enable_noise = false;
        else if (arg == "--help") {
            std::cout << "Usage: paper_sim [--ticks=N] [--markets=N] [--seed=N] [--capital=X] [--no-noise]" << std::endl;
            return 0;
        }
    }

    PaperTradingRunner runner(cfg);
    runner.run();
    return 0;
}
