#include <iostream>
#include <vector>
#include <algorithm>
#include <optional>
#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/src/execution_engine.cpp"
#include "../../core/src/lightweight_client.hpp" // For the updated client

inline uint64_t rdtscp() {
    uint32_t aux; uint64_t rax, rdx;
    asm volatile ("rdtscp" : "=a"(rax), "=d"(rdx), "=c"(aux));
    return (rdx << 32) | rax;
}

void run_latency_test() {
    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal> alpha_queue;
    LightweightCLOBClient client("127.0.0.1", 8080, "BENCH_API_KEY"); // Mock client
    ExecutionEngine engine(book, alpha_queue, client); // Corrected constructor

    std::vector<uint64_t> latencies;
    latencies.reserve(10000);

    for (int i = 0; i < 10000; ++i) {
        AlphaSignal s{};
        strncpy(s.market_slug, "test", 31);
        s.confidence = 0.9; s.ev_per_dollar = 0.1; s.q_value = 0.01;
        alpha_queue.try_push(s);
        
        uint64_t start = rdtscp();
        engine.run_tick();
        uint64_t end = rdtscp();
        latencies.push_back(end - start);
    }

    std::sort(latencies.begin(), latencies.end());
    double cycle_to_us = 1.0 / 3000.0;
    std::cout << "Tick-to-Wire (10k ticks): P50 = " << latencies[5000]*cycle_to_us 
              << "us, P99 = " << latencies[9900]*cycle_to_us << "us\n";
}

int main() { run_latency_test(); return 0; }
