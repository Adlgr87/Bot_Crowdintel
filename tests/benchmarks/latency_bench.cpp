#include <iostream>
#include <vector>
#include <numeric>
#include <algorithm>
#include <chrono>
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/include/order_book.hpp"
#include "../../core/src/execution_engine.cpp"
#include "../../alpha/crowdintel/alpha_receiver.hpp"

// Inline assembly to read the Time Stamp Counter (TSC)
// This provides the highest precision measurement for latency (cycles)
inline uint64_t rdtscp() {
    uint32_t aux;
    uint64_t rax, rdx;
    // Use rdtscp to ensure all previous instructions have executed
    asm volatile ("rdtscp" : "=a"(rax), "=d"(rdx), "=c"(aux) : : "rcx");
    return (rdx << 32) | rax;
}

void run_latency_test() {
    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal> alpha_queue;
    ExecutionEngine engine(book, alpha_queue);

    // Mock data to avoid allocations during loop
    AlphaSignal signal;
    signal.type = AlphaSignal::Type::WHALE_TRADE;
    std::strncpy(signal.market_slug, "TEST-MKT", 31);
    signal.confidence = 0.99;
    signal.ev_per_dollar = 0.10;
    signal.q_value = 0.001;

    const int iterations = 10000;
    std::vector<uint64_t> latencies;
    latencies.reserve(iterations);

    std::cout << "🚀 Starting Tick-to-Wire Latency Benchmark (" << iterations << " iterations)..." << std::endl;

    for (int i = 0; i < iterations; ++i) {
        // Push signal to queue
        alpha_queue.try_push(signal);

        // Measure the time to process the tick
        uint64_t start = rdtscp();
        engine.run_tick();
        uint64_t end = rdtscp();

        latencies.push_back(end - start);
    }

    // Analysis
    std::sort(latencies.begin(), latencies.end());
    uint64_t p50 = latencies[iterations / 2];
    uint64_t p99 = latencies[iterations * 99 / 100];
    uint64_t min = latencies[0];

    // Convert cycles to microseconds (approx for 3.0GHz CPU)
    double cycle_to_us = 1.0 / 3000.0; 

    std::cout << "\n--- Latency Results (Cycles) ---" << std::endl;
    std::cout << "Min: " << min << " cycles" << std::endl;
    std::cout << "P50: " << p50 << " cycles" << std::endl;
    std::cout << "P99: " << p99 << " cycles" << std::endl;
    std::cout << "\n--- Latency Results (Estimated us @ 3GHz) ---" << std::endl;
    std::cout << "P50: " << p50 * cycle_to_us << " us" << std::endl;
    std::cout << "P99: " << p99 * cycle_to_us << " us" << std::endl;
}

int main() {
    run_latency_test();
    return 0;
}
