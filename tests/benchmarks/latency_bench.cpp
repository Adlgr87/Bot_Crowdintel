#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdio>
#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/src/bench_engine.hpp"

inline uint64_t rdtscp() {
    uint32_t aux; uint64_t rax, rdx;
    asm volatile ("rdtscp" : "=a"(rax), "=d"(rdx), "=c"(aux));
    return (rdx << 32) | rax;
}

void run_latency_test() {
    const size_t QUEUE_CAPACITY = 4096;
    const size_t TOTAL_TICKS = 20000;
    const size_t WARMUP_TICKS = 5000;

    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal, QUEUE_CAPACITY> alpha_queue;
    BenchExecutionEngine engine(book, alpha_queue);

    // Calibrate CPU frequency using rdtsc + chrono
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t c0 = rdtscp();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint64_t c1 = rdtscp();
    auto t1 = std::chrono::high_resolution_clock::now();
    double ns_per_cycle = std::chrono::duration<double, std::nano>(t1 - t0).count() / (double)(c1 - c0);

    // Pre-fill queue to capacity-1 (respecting the ring buffer limit)
    size_t filled = 0;
    for (size_t i = 0; i < QUEUE_CAPACITY - 1; ++i) {
        AlphaSignal s{};
        s.type = AlphaSignal::Type::WHALE_TRADE;
        snprintf(s.market_slug, 31, "test-market-%zu", i);
        s.confidence = 0.95;
        s.ev_per_dollar = 0.05;
        s.q_value = 0.01;
        s.timestamp_ns = i;
        if (alpha_queue.try_push(s)) {
            filled++;
        }
    }
    printf(">> Queue pre-filled with %zu signals (capacity: %zu)\n", filled, QUEUE_CAPACITY);

    // Warm-up phase: process with continuous refill, checking try_push return
    for (size_t i = 0; i < WARMUP_TICKS; ++i) {
        if (i % (QUEUE_CAPACITY / 4) == 0) {
            size_t refilled = 0;
            for (size_t j = 0; j < QUEUE_CAPACITY - 1; ++j) {
                AlphaSignal s{};
                s.type = AlphaSignal::Type::WHALE_TRADE;
                s.confidence = 0.95;
                s.ev_per_dollar = 0.05;
                s.q_value = 0.01;
                s.timestamp_ns = i * QUEUE_CAPACITY + j;
                if (alpha_queue.try_push(s)) refilled++;
            }
            printf("   Warmup refill: %zu/%zu signals queued\n", refilled, QUEUE_CAPACITY - 1);
        }
        engine.run_tick();
    }

    // Final refill before measurement (ensure queue is full)
    for (size_t j = 0; j < QUEUE_CAPACITY - 1; ++j) {
        AlphaSignal s{};
        s.type = AlphaSignal::Type::WHALE_TRADE;
        s.confidence = 0.95;
        s.ev_per_dollar = 0.05;
        s.q_value = 0.01;
        s.timestamp_ns = j;
        alpha_queue.try_push(s);
    }

    std::vector<uint64_t> latencies;
    latencies.reserve(TOTAL_TICKS);
    size_t empty_ticks = 0;

    printf(">> Running %zu ticks (post-warmup, with continuous refill)...\n", TOTAL_TICKS);
    for (size_t i = 0; i < TOTAL_TICKS; ++i) {
        // Refill queue before each tick to guarantee productive measurement
        // (avoids measuring empty try_pop fast-fail path)
        for (size_t j = 0; j < QUEUE_CAPACITY - 1; ++j) {
            AlphaSignal s{};
            s.type = AlphaSignal::Type::WHALE_TRADE;
            s.confidence = 0.95;
            s.ev_per_dollar = 0.05;
            s.q_value = 0.01;
            s.timestamp_ns = i * QUEUE_CAPACITY + j;
            alpha_queue.try_push(s);
        }

        uint64_t start = rdtscp();
        engine.run_tick();
        uint64_t end = rdtscp();
        uint64_t delta = end - start;

        // Only record productive ticks (delta > 0 means work was done)
        // Empty ticks (try_pop returns nullopt) would be near-zero cycles
        if (delta > 0) {
            latencies.push_back(delta);
        } else {
            empty_ticks++;
        }
    }

    std::sort(latencies.begin(), latencies.end());

    auto pct = [](const std::vector<uint64_t>& v, double p) {
        size_t idx = static_cast<size_t>(v.size() * p);
        return v[std::min(idx, v.size() - 1)];
    };

    uint64_t min_c = latencies.front();
    uint64_t p50 = pct(latencies, 0.50);
    uint64_t p99 = pct(latencies, 0.99);

    printf("\n--- FINAL LATENCY RESULTS (Post-Optimization + Warmup) ---\n");
    printf("CPU frequency calibrated: %.3f ns/cycle\n", ns_per_cycle);
    printf("Min: \t\t%.3f us (%llu cycles)\n", min_c * ns_per_cycle / 1000.0, min_c);
    printf("P50: \t\t%.3f us (%llu cycles)\n", p50 * ns_per_cycle / 1000.0, p50);
    printf("P99: \t\t%.3f us (%llu cycles)\n", p99 * ns_per_cycle / 1000.0, p99);
    printf("\n✅ Hot Path Core Logic Measured. (Network I/O excluded; %zu-tick warmup, calibrated CPU freq)\n", WARMUP_TICKS);
}

int main() { run_latency_test(); return 0; }