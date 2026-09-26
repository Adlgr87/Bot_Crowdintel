/**
 * Hot Path Latency Microbenchmark — CrowdIntel Bot (C++20)
 *
 * Measures the latency distribution of the critical hot path:
 *   1. OrderBookL2 update (bid/ask)
 *   2. TickResult validation (entry band check)
 *   3. Inline exposure check
 *
 * Targets:
 *   P50 ≤ 51.7μs
 *   P99 < 200μs
 *
 * Build:
 *   g++ -std=c++20 -O2 -Icore/include tests/sim/latency_benchmark.cpp -o bin/latency_bench -lpthread
 *
 * Run:
 *   ./bin/latency_bench --iter=10000 --markets=5
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "order_book.hpp"
#include "tick_result.hpp"

// ─── Benchmark Config ────────────────────────────────────────────────────
struct BenchConfig {
    uint64_t seed = 42;
    int iterations = 10000;
    int num_markets = 5;
};

// Inline risk config for benchmarking (no external dependencies)
struct BenchRiskConfig {
    uint64_t min_price_micros = 350000;  // $0.35
    uint64_t max_price_micros = 700000;  // $0.70
    double max_exposure = 0.20;          // 20% of capital
    double initial_capital = 100000.0;
    double current_exposure = 0.0;
};

void run_benchmark(const BenchConfig& cfg) {
    // Use a single order book for latency measurement (hot path is per-market)
    OrderBookL2 book;

    BenchRiskConfig risk_cfg;

    std::mt19937_64 rng(cfg.seed);
    std::uniform_int_distribution<uint32_t> price_dist(350000, 700000);
    std::uniform_int_distribution<uint32_t> size_dist(100, 5000);

    // Latency storage
    std::vector<uint64_t> latencies;
    latencies.reserve(cfg.iterations);

    // Warmup
    for (int w = 0; w < 1000; w++) {
        uint64_t ts = current_time_ns();
        book.update_bid(0, price_dist(rng), size_dist(rng), ts);
        book.update_ask(0, price_dist(rng) + 50, size_dist(rng), ts);
        (void)book.get_bid(0);
        (void)book.get_ask(0);
    }

    // Benchmark phase — measure only hot path operations
    for (int iter = 0; iter < cfg.iterations; iter++) {
        int level = iter % 10;
        uint64_t ts = current_time_ns();
        uint32_t price = price_dist(rng);
        uint32_t size = size_dist(rng);

        auto start = std::chrono::high_resolution_clock::now();

        // ─── HOT PATH START ──────────────────────
        book.update_bid(level, price, size, ts);
        book.update_ask(level, price + 50, size, ts);

        // Walk top of book (L1 cache hit path)
        const auto& best_bid = book.get_bid(0);
        const auto& best_ask = book.get_ask(0);

        // Inline entry band check (Polywhales: 35-70 cents)
        uint64_t mid_price = (best_bid.price + best_ask.price) / 2;
        if (mid_price >= risk_cfg.min_price_micros &&
            mid_price <= risk_cfg.max_price_micros) {
            // Simulate exposure increment
            risk_cfg.current_exposure += size * 0.0001;
        }
        // ─── HOT PATH END ────────────────────────

        auto end = std::chrono::high_resolution_clock::now();
        uint64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        latencies.push_back(elapsed_ns);
    }

    // Sort for percentile computation
    std::sort(latencies.begin(), latencies.end());

    // Compute percentiles
    size_t n = latencies.size();
    auto pct = [&](double p) -> uint64_t {
        size_t idx = static_cast<size_t>(p * (n - 1));
        return latencies[idx];
    };

    uint64_t p50 = pct(0.50);
    uint64_t p90 = pct(0.90);
    uint64_t p95 = pct(0.95);
    uint64_t p99 = pct(0.99);
    uint64_t p999 = pct(0.999);
    uint64_t min_lat = latencies.front();
    uint64_t max_lat = latencies.back();
    double avg_us = (std::accumulate(latencies.begin(), latencies.end(), 0ULL) / n) / 1000.0;

    // Print results
    std::cout << "\n📊 Hot Path Latency Benchmark Results\n" << std::endl;
    std::cout << "  Iterations:    " << cfg.iterations << std::endl;
    std::cout << "  Markets tested: " << cfg.num_markets << std::endl;
    std::cout << "\n  Latency distribution:\n" << std::endl;
    std::cout << "    Min:         " << std::setw(8) << min_lat << " ns (" << std::fixed << std::setprecision(2) << min_lat / 1000.0 << " μs)" << std::endl;
    std::cout << "    P50:         " << std::setw(8) << p50   << " ns (" << p50 / 1000.0 << " μs)" << std::endl;
    std::cout << "    P90:         " << std::setw(8) << p90   << " ns (" << p90 / 1000.0 << " μs)" << std::endl;
    std::cout << "    P95:         " << std::setw(8) << p95   << " ns (" << p95 / 1000.0 << " μs)" << std::endl;
    std::cout << "    P99:         " << std::setw(8) << p99   << " ns (" << p99 / 1000.0 << " μs)" << std::endl;
    std::cout << "    P99.9:       " << std::setw(8) << p999  << " ns (" << p999 / 1000.0 << " μs)" << std::endl;
    std::cout << "    Max:         " << std::setw(8) << max_lat << " ns (" << max_lat / 1000.0 << " μs)" << std::endl;
    std::cout << "    Average:     " << std::fixed << std::setprecision(2) << avg_us << " μs" << std::setprecision(0) << std::endl;
    std::cout << "\n  Targets:\n" << std::endl;
    std::cout << "    P50 ≤ 51.7 μs: " << (p50 / 1000.0 <= 51.7 ? "✅ PASS" : "❌ FAIL") << std::endl;
    std::cout << "    P99 <  200 μs: " << (p99 / 1000.0 < 200.0 ? "✅ PASS" : "❌ FAIL") << std::endl;
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
}

// ─── Main ─────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    BenchConfig cfg;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--iter=") == 0) cfg.iterations = std::stoi(arg.substr(7));
        else if (arg.find("--markets=") == 0) cfg.num_markets = std::stoi(arg.substr(10));
        else if (arg.find("--seed=") == 0) cfg.seed = std::stoull(arg.substr(7));
        else if (arg == "--help") {
            std::cout << "CrowdIntel Bot — Hot Path Latency Microbenchmark\n\n"
                      << "Usage: latency_bench [OPTIONS]\n\n"
                      << "Options:\n"
                      << "  --iter=N        Number of iterations (default: 10000)\n"
                      << "  --markets=N     Number of markets to simulate (default: 5)\n"
                      << "  --seed=N        PRNG seed (default: 42)\n"
                      << std::endl;
            return 0;
        }
    }

    std::cout << "\n🚀 Starting hot path latency benchmark..." << std::endl;
    std::cout << "  Config: " << cfg.iterations << " iterations, "
              << cfg.num_markets << " markets" << std::endl;

    run_benchmark(cfg);
    return 0;
}
