// ─────────────────────────────────────────────────────────────────────────────
// l2_backtester: replay L2 tick data through the book + Kelly take-strategy.
//
// Input: CSV lines  bid_price,bid_size,ask_price,ask_size,p_win[,confidence,q]
//         (prices/sizes in decimal units, ×1e6 fixed internally; p_win the
//          CrowdIntel posterior for that tick)
//         fed on stdin or via file argument. Empty lines and #-comments skip.
//
// Simulates: signal edge vs the live book, exact Kelly sizing, marketable
// take at the opposing level (up to visible size), fee-less PnL marked at
// the opposite side mid, slippage vs mid, hit rate and max drawdown.
// This replaces the previous `run_replay` skeleton (an empty loop body).
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../../core/include/order_book.hpp"
#include "kelly_engine.hpp"

struct ReplayTick {
    double bid, bid_sz, ask, ask_sz;
    double p_win;
    double confidence = 0.95;
    double q_value = 0.01;
};

class L2Backtester {
public:
    explicit L2Backtester(double bankroll_usd, double kelly_fraction = 0.25,
                          double min_edge = 0.02)
        : bankroll_(bankroll_usd), kelly_cap_(kelly_fraction), min_edge_(min_edge) {}

    struct Trade {
        uint64_t idx;
        uint8_t side;        // 0 buy 1 sell
        double price, size_shares, mid_after;
    };

    // Feed one tick; returns the trade taken (if any).
    bool on_tick(uint64_t idx, const ReplayTick& t, OrderBookL2& book) {
        Level2Entry b{(uint64_t)(t.bid * 1e6 + 0.5), (uint64_t)(t.bid_sz * 1e6 + 0.5)};
        Level2Entry a{(uint64_t)(t.ask * 1e6 + 0.5), (uint64_t)(t.ask_sz * 1e6 + 0.5)};
        book.set_bids(&b, 1);
        book.set_asks(&a, 1);

        const double buy_edge = t.p_win - t.ask;
        const double sell_edge = t.bid - t.p_win;
        if (buy_edge < min_edge_ && sell_edge < min_edge_) return false;
        if (t.confidence < 0.85 || t.q_value > 0.05) return false;

        Trade tr{};
        tr.idx = idx;
        if (buy_edge >= sell_edge) {
            tr.side = 0;
            tr.price = t.ask;
            const double k = KellyEngine::kelly_buy(t.p_win, t.ask);
            const double usd = KellyEngine::position_usd(k, kelly_cap_, bankroll_);
            tr.size_shares = usd / t.ask;
            const double avail = t.ask_sz;
            tr.size_shares = std::min(tr.size_shares, avail);
        } else {
            tr.side = 1;
            tr.price = t.bid;
            const double k = KellyEngine::kelly_sell(t.p_win, t.bid);
            const double usd = KellyEngine::position_usd(k, kelly_cap_, bankroll_);
            tr.size_shares = usd / t.bid;
            const double avail = t.bid_sz;
            tr.size_shares = std::min(tr.size_shares, avail);
        }
        if (tr.size_shares <= 0.0) return false;

        const double mid = 0.5 * (t.bid + t.ask);
        tr.mid_after = mid;
        // Mark-to-mid PnL: buy wins if price moves up (proxy for resolution EV);
        // immediate mark = edge captured.
        const double mark = (tr.side == 0) ? (mid - tr.price) : (tr.price - mid);
        const double pnl = mark * tr.size_shares;
        pnl_ += pnl;
        equity_.push_back(pnl_);
        trades_.push_back(tr);
        return true;
    }

    void report() const {
        if (trades_.empty()) {
            std::printf("replay done: 0 trades\n");
            return;
        }
        double sum_slip = 0.0, peak = -1e18, max_dd = 0.0;
        size_t wins = 0;
        for (const auto& tr : trades_) {
            const double slip = (tr.side == 0) ? (tr.price - tr.mid_after)
                                               : (tr.mid_after - tr.price);
            sum_slip += std::abs(slip) * tr.size_shares;
            if (pnl_of(tr) > 0) ++wins;
        }
        for (double eq : equity_) {
            peak = std::max(peak, eq);
            max_dd = std::max(max_dd, peak - eq);
        }
        std::printf("replay done: trades=%zu win_rate=%.1f%% pnl=%.2f usd "
                    "avg_slippage=%.6f max_drawdown=%.2f\n",
                    trades_.size(),
                    100.0 * wins / trades_.size(),
                    pnl_, sum_slip / trades_.size(), max_dd);
    }

private:
    double pnl_of(const Trade& tr) const {
        const double mark = (tr.side == 0) ? (tr.mid_after - tr.price)
                                           : (tr.price - tr.mid_after);
        return mark * tr.size_shares;
    }

    double bankroll_, kelly_cap_, min_edge_;
    double pnl_ = 0.0;
    std::vector<double> equity_;
    std::vector<Trade> trades_;
};

int main(int argc, char** argv) {
    std::printf("CROWDINTEL L2 backtester — replays bid,ask,p_win CSV through the book\n");
    std::ifstream file;
    std::istream* in = &std::cin;
    if (argc > 1) {
        file.open(argv[1]);
        if (!file) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        in = &file;
    }

    L2Backtester bt(10000.0);
    OrderBookL2 book;
    std::string line;
    uint64_t idx = 0, parsed = 0;
    while (std::getline(*in, line)) {
        if (line.empty() || line[0] == '#') continue;
        ReplayTick t;
        double conf = 0.95, q = 0.01;
        if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                        &t.bid, &t.bid_sz, &t.ask, &t.ask_sz,
                        &t.p_win, &conf, &q) >= 5) {
            t.confidence = conf;
            t.q_value = q;
            bt.on_tick(idx++, t, book);
            ++parsed;
        }
    }
    std::printf("parsed %llu ticks\n", (unsigned long long)parsed);
    bt.report();
    return 0;
}
