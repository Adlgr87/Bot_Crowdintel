#ifndef MOCK_CLIENT_HPP
#define MOCK_CLIENT_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "polymarket_order.hpp"

class MockCLOBClient {
public:
    explicit MockCLOBClient(const struct MarketConfig&) {}

    SubmitResult submit(const WireBody&) {
        submissions_.fetch_add(1, std::memory_order_relaxed);
        SubmitResult result{};
        result.ok = true;
        result.final = true;
        result.http_code = 200;
        std::snprintf(result.status, sizeof(result.status), "matched");
        return result;
    }

    uint64_t submissions() const {
        return submissions_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> submissions_{0};
};

#endif  // MOCK_CLIENT_HPP
