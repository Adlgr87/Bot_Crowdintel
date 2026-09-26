/**
 * Memory Allocation Verification — CrowdIntel Bot Hot Path
 *
 * Verifies that the hot path (order book update + entry band check)
 * does NOT perform dynamic allocations.
 *
 * Build:
 *   g++ -std=c++20 -O2 -Icore/include tests/sim/memory_check.cpp -o bin/mem_check -lpthread
 *
 * Run:
 *   ./bin/mem_check --iter=10000
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <sys/resource.h>
#include <string>

#include "order_book.hpp"
#include "tick_result.hpp"

// Override malloc to detect allocations
static long alloc_count = 0;

void* operator new(std::size_t sz) {
    alloc_count++;
    return malloc(sz);
}

void* operator new[](std::size_t sz) {
    alloc_count++;
    return malloc(sz);
}

int main(int argc, char* argv[]) {
    int iterations = 10000;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.find("--iter=") == 0) iterations = std::stoi(arg.substr(7));
    }

    std::cout << "\n🔍 Memory Allocation Check\n" << std::endl;

    // Reset allocation counter
    alloc_count = 0;

    // Create a single order book
    OrderBookL2 book;

    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint32_t> price_dist(350000, 700000);
    std::uniform_int_distribution<uint32_t> size_dist(100, 5000);

    // Warmup
    for (int i = 0; i < 1000; i++) {
        uint64_t ts = current_time_ns();
        book.update_bid(i % 10, price_dist(rng), size_dist(rng), ts);
        book.update_ask(i % 10, price_dist(rng) + 50, size_dist(rng), ts);
        (void)book.get_bid(0);
        (void)book.get_ask(0);
    }

    // Reset counter before benchmark
    alloc_count = 0;

    // Benchmark phase — should allocate NOTHING
    for (int i = 0; i < iterations; i++) {
        int level = i % 10;
        uint64_t ts = current_time_ns();
        uint32_t price = price_dist(rng);
        uint32_t size = size_dist(rng);

        // Hot path — should allocate NOTHING
        book.update_bid(level, price, size, ts);
        book.update_ask(level, price + 50, size, ts);

        // Entry band check (inline, no allocation)
        const auto& best_bid = book.get_bid(0);
        const auto& best_ask = book.get_ask(0);
        uint64_t mid = (best_bid.price + best_ask.price) / 2;
        if (mid < 350000 || mid > 700000) {
            // Out of band
        }
    }

    long total_ops = iterations * 4;  // 4 ops per tick

    std::cout << "\n📊 Memory Allocation Results\n" << std::endl;
    std::cout << "  Iterations:      " << iterations << std::endl;
    std::cout << "  Hot path ops:    " << total_ops << std::endl;
    std::cout << "  Dynamic allocs:  " << alloc_count << std::endl;
    std::cout << "  Allocs per op:   " << (alloc_count > 0 ? (double)alloc_count / total_ops : 0.0) << std::endl;
    std::cout << "\n  Verdict: " << (alloc_count == 0 ? "✅ PASS — Hot path is zero-allocation" : "❌ FAIL — Allocation detected") << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    return alloc_count == 0 ? 0 : 1;
}
