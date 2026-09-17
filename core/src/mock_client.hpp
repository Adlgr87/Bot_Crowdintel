#ifndef MOCK_CLIENT_HPP
#define MOCK_CLIENT_HPP

#include <array>
#include <iostream>
#include <string>

/**
 * MockCLOBClient: A non-blocking, zero-I/O client for benchmarking.
 * Used by tests/benchmarks to isolate Hot Path latency from network jitter.
 */
struct MockOrder {
    uint64_t nonce;
    std::array<uint8_t, 65> signature;
};

class MockCLOBClient {
public:
    MockCLOBClient() {}
    bool submit_order(const MockOrder& order) {
        // In Hot Path, we don't wait for network I/O.
        // This simulates the instant hand-off of a signed order to the NIC.
        (void)order; // Suppress unused parameter warning
        return true;
    }
};

#endif // MOCK_CLIENT_HPP
