#ifndef MOCK_CLIENT_HPP
#define MOCK_CLIENT_HPP

// MockCLOBClient: zero-I/O client for benchmarking and offline runs. Same
// submit() interface as LightweightCLOBClient, so engines template over
// either. Standalone on purpose (no curl/OpenSSL includes) so the offline
// build (CROWDINTEL_NETWORK=OFF) needs no network libraries at all.

#include <atomic>
#include <cstdint>

#include "polymarket_order.hpp"

class MockCLOBClient {
public:
    explicit MockCLOBClient(const struct MarketConfig&) {}
    SubmitResult submit(const WireBody& body) {
        (void)body;
        submissions_.fetch_add(1, std::memory_order_relaxed);
        return SubmitResult{true, 200, {0}};
    }
    uint64_t submissions() const { return submissions_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> submissions_{0};
};

#endif // MOCK_CLIENT_HPP
