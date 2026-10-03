#ifndef MOCK_CLIENT_HPP
#define MOCK_CLIENT_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "polymarket_order.hpp"

class MockCLOBClient {
public:
    // `id_prefix` keeps the two simulation modes distinguishable in logs:
    // replay is fully offline, paper is driven by live public market data.
    explicit MockCLOBClient(const struct MarketConfig&,
                            const char* id_prefix = "mock")
        : id_prefix_(id_prefix ? id_prefix : "mock") {}

    SubmitResult submit(const WireBody&) {
        const uint64_t submission = submissions_.fetch_add(
            1, std::memory_order_relaxed) + 1;
        SubmitResult result{};
        result.ok = true;
        result.final = true;
        result.http_code = 200;
        std::snprintf(result.status, sizeof(result.status), "matched");
        // A simulated venue must acknowledge with an order id: a ledgered mock
        // order without one would stay PendingSubmit and block the next run.
        std::snprintf(result.order_id, sizeof(result.order_id), "%s-%llu",
                      id_prefix_, static_cast<unsigned long long>(submission));
        return result;
    }

    uint64_t submissions() const {
        return submissions_.load(std::memory_order_relaxed);
    }

private:
    const char* id_prefix_;
    std::atomic<uint64_t> submissions_{0};
};

#endif  // MOCK_CLIENT_HPP
