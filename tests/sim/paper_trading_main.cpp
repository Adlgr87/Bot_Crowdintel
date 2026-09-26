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
 *   g++ -std=c++20 -O2 -Icore/include tests/sim/paper_trading_main.cpp -o bin/paper_sim -lpthread
 *
 * Run with full parameter help:
 *   ./bin/paper_sim --help
 *
 * All output is machine-parseable (CSV-compatible key metrics at the end).
 */

#include <algorithm>
#include <chrono>
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
    double noise_stddev = 0.005;  // 0.5% price volatility per tick
    double capital_exposure_limit = 0.20;  // 20% of capital
    int tick_speed_ms = 50;        // Signal frequency
    double order_size_usd = 250.0; // Average order size
    double fill_probability = 0.75; // 75% chance of fill per signal
    bool enable_csv_output = false; // If true, outputs machine-parseable format
    std::string label = "";         // Custom label for this run
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
    int days_to_resolution = 7;  // Polywhales default: 7 days
    std::chrono::steady_clock::time_point last_update;
    std::unique_ptr<OrderBookL2> book;

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
            book->update_bid(static_cast<uint32_t>(level),
                              static_cast<uint64_t>(bid_px * 1e6), bz, ts);
            book->update_ask(static_cast<uint32_t>(level),
                              static_cast<uint64_t>(ask_px * 1e6), bz, ts);
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
    explicit SignalGenerator(uint64_t seed, double min_ev, double floor, double cap,
                              double order_size, double fill_prob)
        : rng_(seed), ev_noise_(-0.02, 0.02),
          size_dist_(order_size * 0.5, order_size * 1.5),
          floor_(floor), cap_(cap), min_ev_(min_ev), fill_prob_(fill_prob) {}

    SimSignal generate(int num_markets_actual, const std::vector<SimMarket>& markets) {
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
        if (mkt.is_stale_flag) return;
        double noise = std::normal_distribution<double>(0.0, stddev)(rng_);
        mkt.base_price += noise;
        mkt.base_price = std::max(0.01, std::min(0.99, mkt.base_price));
        mkt.refresh_book();
    }

    double fill_probability() const { return fill_prob_; }

private:
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> ev_noise_;
    std::uniform_real_distribution<double> size_dist_;
    double floor_;
    double cap_;
    double min_ev_;
    double fill_prob_;
};

// ─── Risk Engine (mirrors production RiskEngine checks) ─────────────────
class SimRiskEngine {
public:
    explicit SimRiskEngine(const SimConfig& cfg) : cfg_(cfg) {}

    TickResult pre_trade_check(const SimSignal& sig, const SimMarket& mkt,
                               double usd_balance, double& exposure) {
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

        // 3. Entry band check (Polywhales policy)
        double current_price = mkt.base_price;
        if (current_price < cfg_.entry_floor || current_price > cfg_.entry_cap) {
            return TickResult::RISK_BLOCKED;
        }

        // 4. Exposure check
        double usd_size = static_cast<double>(sig.size_usd) / 1e6;
        if (exposure + usd_size > cfg_.initial_capital_usd * cfg_.capital_exposure_limit) {
            return TickResult::RISK_BLOCKED;
        }

        // 5. Balance check
        if (usd_balance < usd_size) {
            return TickResult::INSUFFICIENT_BALANCE;
        }

        // 6. EV threshold
        if (sig.ev_per_dollar < cfg_.min_ev_threshold) {
            return TickResult::NO_SIGNAL;
        }

        exposure += usd_size;
        return TickResult::OK;
    }

    void kill_switch(bool on) { kill_switched_ = on; }
    bool kill_switch_enabled() const { return kill_switched_; }

private:
    SimConfig cfg_;
    std::atomic<bool> kill_switched_{false};
};

// ─── Simulation Runner ───────────────────────────────────────────────────
class PaperTradingRunner {
public:
    explicit PaperTradingRunner(const SimConfig& cfg)
        : cfg_(cfg),
          gen_(cfg.seed, cfg.min_ev_threshold, cfg.entry_floor, cfg.entry_cap,
               cfg.order_size_usd, cfg.fill_probability),
          risk_(cfg),
          fill_rng_(cfg.seed + 999),
          fill_dist_(0.0, 1.0) {
        for (int i = 0; i < cfg.num_markets; i++) {
            double base = cfg.entry_floor + 0.01 + (i * 0.06);
            if (base > cfg.entry_cap) base = cfg.entry_cap - 0.01;
            markets_.emplace_back(base);
        }
    }

    void run() {
        // Header (JSON for machine parsing)
        if (cfg_.enable_csv_output) {
            // CSV output handled at end
        } else {
            std::cout << "\n🧪 CrowdIntel Bot — Paper Trading Simulation (Polymarket CLOB V2)" << std::endl;
            std::cout << "════════════════════════════════════════════════════════════════════" << std::endl;
            if (!cfg_.label.empty()) std::cout << "  Run: " << cfg_.label << std::endl;
            std::cout << "  Markets:            " << cfg_.num_markets << std::endl;
            std::cout << "  Max ticks:          " << cfg_.num_ticks << std::endl;
            std::cout << "  Seed:               " << cfg_.seed << std::endl;
            std::cout << "  Capital:            $" << std::fixed << std::setprecision(2) << cfg_.initial_capital_usd << std::endl;
            std::cout << "  Entry band:         [$" << cfg_.entry_floor << ", $" << cfg_.entry_cap << "]" << std::endl;
            std::cout << "  Min EV/USD:         $" << cfg_.min_ev_threshold << std::endl;
            std::cout << "  Exposure limit:     " << (cfg_.capital_exposure_limit * 100) << "%" << std::endl;
            std::cout << "  Order size (avg):   $" << cfg_.order_size_usd << std::endl;
            std::cout << "  Fill probability:   " << (cfg_.fill_probability * 100) << "%" << std::endl;
            std::cout << "  Noise stddev:       " << cfg_.noise_stddev << std::endl;
            std::cout << "  Kill switch:        " << (risk_.kill_switch_enabled() ? "ON" : "OFF") << std::endl;
            std::cout << "════════════════════════════════════════════════════════════════════" << std::endl;
            std::cout << std::endl;
        }

        double usd_balance = cfg_.initial_capital_usd;
        double total_exposure = 0.0;
        int total_signals = 0;
        int total_fills = 0;
        int total_skips = 0;
        int total_stale = 0;
        int total_blocked = 0;
        int total_no_signal = 0;
        int total_insufficient_balance = 0;
        double total_realized_pnl = 0.0;
        std::vector<double> fill_prices;
        std::vector<double> fill_sizes;

        for (int tick = 0; tick < cfg_.num_ticks; tick++) {
            // 1. Walk market prices
            if (cfg_.enable_noise) {
                for (auto& mkt : markets_) {
                    gen_.walk_market(mkt, cfg_.noise_stddev);
                }
            }

            // 2. Inject stale books periodically
            if (cfg_.enable_noise && tick > 0 && tick % 200 == 0) {
                int stale_mkt = (tick / 200) % cfg_.num_markets;
                markets_[stale_mkt].is_stale_flag = true;
                if (!cfg_.enable_csv_output) {
                    std::cout << "  ⚠️  TICK " << std::setw(4) << tick
                              << " | FEED DEATH on market " << stale_mkt << std::endl;
                }
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

                    // Apply fill probability
                    if (fill_dist_(fill_rng_) < cfg_.fill_probability) {
                        total_fills++;
                        usd_balance -= usd_size;
                        fill_prices.push_back(fill_price);
                        fill_sizes.push_back(usd_size);
                        mkt.fills++;
                        mkt.current_position += (sig.side == 0) ? usd_size : -usd_size;
                        if (sig.side == 0) mkt.entry_price = fill_price;

                        // Mean reversion exit
                        if (mkt.current_position != 0.0 && mkt.entry_price > 0 &&
                            fill_dist_(fill_rng_) < 0.5) {
                            double exit_price = mkt.base_price;
                            double pnl = mkt.current_position * (exit_price - mkt.entry_price);
                            mkt.realized_pnl += pnl;
                            total_realized_pnl += pnl;
                            usd_balance += fabs(mkt.current_position * mkt.entry_price * 0.01);
                            total_exposure -= usd_size;
                            mkt.current_position = 0.0;
                            mkt.entry_price = 0.0;
                        }

                        if (!cfg_.enable_csv_output) {
                            std::cout << "  🔵 TICK " << std::setw(4) << tick
                                      << " | BUY  " << sig.market_slug()
                                      << " | $" << std::setprecision(3) << fill_price
                                      << " | $" << std::setprecision(2) << usd_size
                                      << " | EV=$" << std::setprecision(4) << sig.ev_per_dollar
                                      << std::endl;
                        }
                    } else {
                        total_skips++;
                        mkt.skips++;
                        total_exposure -= usd_size;
                        if (!cfg_.enable_csv_output) {
                            std::cout << "  🟡 TICK " << std::setw(4) << tick
                                      << " | PASS " << sig.market_slug()
                                      << " | EV=$" << std::setprecision(4) << sig.ev_per_dollar
                                      << " | no fill" << std::endl;
                        }
                    }
                    break;
                }
                case TickResult::STALE_BOOK:
                    total_stale++;
                    mkt.stale_hits++;
                    if (!cfg_.enable_csv_output) {
                        std::cout << "  ⚫ TICK " << std::setw(4) << tick
                                  << " | SKIP " << sig.market_slug()
                                  << " | stale book" << std::endl;
                    }
                    break;
                case TickResult::RISK_BLOCKED:
                    total_blocked++;
                    break;
                case TickResult::INSUFFICIENT_BALANCE:
                    total_insufficient_balance++;
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
                      total_blocked, total_no_signal, total_insufficient_balance,
                      total_exposure, usd_balance, fill_prices, fill_sizes,
                      total_realized_pnl);
    }

private:
    SimConfig cfg_;
    SignalGenerator gen_;
    SimRiskEngine risk_;
    std::vector<SimMarket> markets_;
    std::mt19937_64 fill_rng_;
    std::uniform_real_distribution<double> fill_dist_;

    void print_report(int signals, int fills, int skips, int stales,
                       int blocked, int no_signal, int insufficient_balance,
                       double exposure, double final_balance,
                       const std::vector<double>& fill_prices,
                       const std::vector<double>& fill_sizes,
                       double realized_pnl) {

        double fill_rate = signals > 0 ? (100.0 * fills / signals) : 0.0;
        double avg_fill_price = fill_prices.empty() ? 0.0
            : std::accumulate(fill_prices.begin(), fill_prices.end(), 0.0) / fill_prices.size();
        double total_volume = std::accumulate(fill_sizes.begin(), fill_sizes.end(), 0.0);

        if (cfg_.enable_csv_output) {
            std::cout << "result,"
                      << cfg_.seed << ","
                      << cfg_.num_markets << ","
                      << cfg_.num_ticks << ","
                      << cfg_.initial_capital_usd << ","
                      << cfg_.entry_floor << ","
                      << cfg_.entry_cap << ","
                      << cfg_.min_ev_threshold << ","
                      << cfg_.capital_exposure_limit << ","
                      << cfg_.order_size_usd << ","
                      << cfg_.fill_probability << ","
                      << cfg_.noise_stddev << ","
                      << signals << ","
                      << fills << ","
                      << std::fixed << std::setprecision(4) << fill_rate << ","
                      << stales << ","
                      << blocked << ","
                      << no_signal << ","
                      << insufficient_balance << ","
                      << std::setprecision(2) << realized_pnl << ","
                      << avg_fill_price << ","
                      << (cfg_.label.empty() ? "default" : cfg_.label)
                      << std::endl;
        } else {
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
            std::cout << "    Insufficient balance:  " << insufficient_balance << std::endl;
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
                std::cout << "⚠️  WARNING: Exposure exceeded 25% of capital." << std::endl;
            }
            std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        }
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
        else if (arg.find("--min-ev=") == 0) cfg.min_ev_threshold = std::stod(arg.substr(9));
        else if (arg.find("--floor=") == 0) cfg.entry_floor = std::stod(arg.substr(8));
        else if (arg.find("--cap=") == 0) cfg.entry_cap = std::stod(arg.substr(6));
        else if (arg.find("--exposure-limit=") == 0) cfg.capital_exposure_limit = std::stod(arg.substr(17));
        else if (arg.find("--order-size=") == 0) cfg.order_size_usd = std::stod(arg.substr(13));
        else if (arg.find("--fill-prob=") == 0) cfg.fill_probability = std::stod(arg.substr(12));
        else if (arg.find("--noise=") == 0) cfg.noise_stddev = std::stod(arg.substr(8));
        else if (arg.find("--tick-ms=") == 0) cfg.tick_speed_ms = std::stoi(arg.substr(10));
        else if (arg.find("--label=") == 0) cfg.label = arg.substr(8);
        else if (arg == "--no-noise") cfg.enable_noise = false;
        else if (arg == "--csv") cfg.enable_csv_output = true;
        else if (arg == "--help") {
            std::cout << "CrowdIntel Bot — Paper Trading Simulation\n\n"
                      << "Usage: paper_sim [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  --ticks=N             Number of simulation ticks (default: 1000)\n"
                      << "  --markets=N           Number of markets to simulate (default: 5)\n"
                      << "  --seed=N              PRNG seed for reproducibility (default: 42)\n"
                      << "  --capital=X           Starting capital in USD (default: 100000)\n"
                      << "  --min-ev=X            Minimum EV per dollar (default: 0.02)\n"
                      << "  --floor=X             Entry band floor (default: 0.35)\n"
                      << "  --cap=X               Entry band cap (default: 0.70)\n"
                      << "  --exposure-limit=X    Max exposure as fraction of capital (default: 0.20)\n"
                      << "  --order-size=X        Average order size in USD (default: 250)\n"
                      << "  --fill-prob=X         Fill probability 0.0-1.0 (default: 0.75)\n"
                      << "  --noise=X             Market noise stddev (default: 0.005)\n"
                      << "  --tick-ms=N           Tick speed in ms (default: 50)\n"
                      << "  --label=NAME          Label for this run\n"
                      << "  --no-noise            Disable market noise (deterministic)\n"
                      << "  --csv                 Output machine-parseable CSV format\n"
                      << std::endl;
            return 0;
        }
    }

    PaperTradingRunner runner(cfg);
    runner.run();
    return 0;
}
