#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/src/bench_engine.hpp"
#include <cstring>

inline uint64_t rdtscp() {
    uint32_t aux; uint64_t rax, rdx;
    asm volatile ("rdtscp" : "=a"(rax), "=d"(rdx), "=c"(aux));
    return (rdx << 32) | rax;
}

void run_latency_test() {
    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal> alpha_queue;
    BenchExecutionEngine engine(book, alpha_queue);

    // Pre-fill the queue to avoid starvation
    for (int i = 0; i < 20000; ++i) {
        AlphaSignal s{};
        s.type = AlphaSignal::Type::WHALE_TRADE;
        strncpy(s.market_slug, "test", 31);
        s.confidence = 0.95;
        s.ev_per_dollar = 0.05;
        s.q_value = 0.01;
        alpha_queue.try_push(s);
    }

    // Warm-up phase (5K iterations)
    for (int i = 0; i < 5000; ++i) engine.run_tick();

    std::vector<uint64_t> latencies;
    latencies.reserve(20000);

    std::cout << ">> Running 20,000 ticks (post-warmup)..." << std::endl;
    for (int i = 0; i < 20000; ++i) {
        uint64_t start = rdtscp();
        engine.run_tick();
        uint64_t end = rdtscp();
        latencies.push_back(end - start);
    }

    std::sort(latencies.begin(), latencies.end());
    
    auto pct = [](const std::vector<uint64_t>& v, double p) {
        size_t idx = static_cast<size_t>(v.size() * p);
        return v[std::min(idx, v.size() - 1)];
    };

    double cycle_to_us = 1.0 / 3000.0; // Assuming 3.0 GHz CPU
    uint64_t min_c = latencies.front();
    uint64_t p50 = pct(latencies, 0.50);
    uint64_t p99 = pct(latencies, 0.99);

    std::cout << "\n--- FINAL LATENCY RESULTS (Post-MutaLambda + Warmup) ---" << std::endl;
    std::cout << "Min: \t\t" << min_c * cycle_to_us << " us (" << min_c << " cycles)" << std::endl;
    std::cout << "P50: \t\t" << p50 * cycle_to_us << " us (" << p50 << " cycles)" << std::endl;
    std::cout << "P99: \t\t" << p99 * cycle_to_us << " us (" << p99 << " cycles)" << std::endl;
    std::cout << "\n✅ Hot Path Core Logic Measured. (Network I/O excluded; 5K-tick warmup applied)" << std::endl;
}

int main() { run_latency_test(); return 0; }
