// ─────────────────────────────────────────────────────────────────────────────
// bench_cfc: Latency & throughput benchmark for CfC inference kernel.
//
// Requirements (from LATENCY_BUDGET.md):
//   p50  < 3μs
//   p99  < 5μs
//   IPC  > 2.0
//   Zero heap allocations
//
// Uses RDTSC for cycle-level precision.  Runs 100,000 inferences.
// ─────────────────────────────────────────────────────────────────────────────
#include <limits>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <limits>

#include "../../core/src/cfc_network.hpp"

// ── RDTSC timestamp ───────────────────────────────────────────────────────────
static inline uint64_t rdtsc() {
    unsigned int hi, lo;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// ── Custom allocator that aborts on heap use ───────────────────────────────────
static std::atomic<uint64_t> g_alloc_count{0};
void* operator new(std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n);
}
void operator delete(void* p) noexcept { std::free(p); }

// ── Latency percentiles ───────────────────────────────────────────────────────
struct LatencyStats {
    uint64_t min_cycles, p50_cycles, p99_cycles, max_cycles;
    double cycles_to_us(uint64_t cycles, double cpu_freq_mhz) {
        return static_cast<double>(cycles) / cpu_freq_mhz;
    }
};

int main() {
    std::printf("=== CfC Inference Latency Benchmark ===\n");

    // Load model
    CfCNetwork net;
    if (!net.load_weights("infra/models/cfc_btc_5m_v1.bin")) {
        std::fprintf(stderr, "FATAL: Cannot load model\n");
        return 1;
    }

    // Detect CPU frequency
    double cpu_freq_mhz = 3500.0;  // default assumption
    // Try reading from /proc/cpuinfo or dmidecode
    FILE* f = std::fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            if (std::strncmp(line, "model name", 8) == 0) {
                // Parse frequency from model name (Intel/AMD)
                char* mhz = std::strstr(line, "MHz");
                if (mhz) {
                    mhz += 3;
                    while (*mhz == ' ') ++mhz;
                    cpu_freq_mhz = std::atof(mhz);
                }
            }
        }
        std::fclose(f);
    }

    // Prepare inputs
    constexpr uint32_t N_ITERS = 100'000;
    std::vector<uint64_t> latencies(N_ITERS);

    CfCState state{};
    net.reset(state, 0.0f);

    // Reset allocator counter
    g_alloc_count.store(0);

    // Warm up cache
    for (int i = 0; i < 1000; i++) {
        CfCInput input{};
        for (uint32_t d = 0; d < CfCConfig::D_INPUT; d++)
            input.features[d] = static_cast<float>(std::sin(d + i * 0.01));
        input.timestamp_ns = i * 100'000'000ULL;  // 100ms
        (void)net.infer(state, input, i * 100'000'000ULL);
    }

    // Benchmark
    const uint64_t start_cycles = rdtsc();

    for (uint32_t i = 0; i < N_ITERS; i++) {
        CfCInput input{};
        for (uint32_t d = 0; d < CfCConfig::D_INPUT; d++)
            input.features[d] = static_cast<float>(std::sin(d + i * 0.001));
        input.timestamp_ns = i * 100'000'000ULL;

        const uint64_t begin = rdtsc();
        CfCSignal result = net.infer(state, input, (i + 1) * 100'000'000ULL);
        const uint64_t end = rdtsc();
        latencies[i] = end - begin;

        // Prevent dead-code elimination
        if (std::isnan(result.probability_up)) {
            std::printf("NaN detected!\n");
            return 1;
        }
    }

    const uint64_t total_cycles = rdtsc() - start_cycles;

    // Sort for percentile computation
    std::sort(latencies.begin(), latencies.end());

    const LatencyStats stats{
        .min_cycles = latencies[0],
        .p50_cycles = latencies[N_ITERS / 2],
        .p99_cycles = latencies[N_ITERS * 99 / 100],
        .max_cycles = latencies[N_ITERS - 1],
    };

    // Compute stats
    uint64_t sum = 0;
    for (uint64_t l : latencies) sum += l;
    const double avg_us = stats.cycles_to_us(sum / N_ITERS, cpu_freq_mhz);

    // Throughput
    auto now = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto elapsed = std::chrono::high_resolution_clock::now() - now;
    const double wall_time = std::chrono::duration<double>(elapsed).count();

    const double total_time_us =
        static_cast<double>(total_cycles) / (cpu_freq_mhz * 1000.0);
    const double ips = N_ITERS / (total_time_us * 1e-6);

    std::printf("\n--- Results ---\n");
    std::printf("CPU freq:    %.1f MHz\n", cpu_freq_mhz);
    std::printf("Iterations:  %u\n", N_ITERS);
    std::printf("Heap allocs: %lu (must be 0)\n",
                g_alloc_count.load(std::memory_order_relaxed));
    std::printf("\nPer-inference latency:\n");
    std::printf("  p50:   %.3f μs  (%.0f cycles)\n",
                stats.cycles_to_us(stats.p50_cycles, cpu_freq_mhz),
                static_cast<double>(stats.p50_cycles));
    std::printf("  p99:   %.3f μs  (%.0f cycles)\n",
                stats.cycles_to_us(stats.p99_cycles, cpu_freq_mhz),
                static_cast<double>(stats.p99_cycles));
    std::printf("  max:   %.3f μs  (%.0f cycles)\n",
                stats.cycles_to_us(stats.max_cycles, cpu_freq_mhz),
                static_cast<double>(stats.max_cycles));
    std::printf("  avg:   %.3f μs\n", avg_us);
    std::printf("\nThroughput: %.0f inferences/sec\n", ips);

    // ── Accept/reject ──────────────────────────────────────────────────────────
    const bool p50_ok = stats.cycles_to_us(stats.p50_cycles, cpu_freq_mhz) < 3.0;
    const bool p99_ok = stats.cycles_to_us(stats.p99_cycles, cpu_freq_mhz) < 5.0;
    const bool alloc_ok = (g_alloc_count.load() == 0);

    std::printf("\n--- Budget Check ---\n");
    std::printf("  p50 < 3μs:    %s\n", p50_ok ? "PASS ✅" : "FAIL ❌");
    std::printf("  p99 < 5μs:    %s\n", p99_ok ? "PASS ✅" : "FAIL ❌");
    std::printf("  No allocs:    %s\n", alloc_ok ? "PASS ✅" : "FAIL ❌");

    if (!p50_ok || !p99_ok || !alloc_ok) {
        std::printf("\nBENCHMARK FAILED\n");
        return 1;
    }

    std::printf("\nBENCHMARK PASSED\n");
    return 0;
}
