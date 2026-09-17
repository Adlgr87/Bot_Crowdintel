#ifndef ALPHA_RECEIVER_HPP
#define ALPHA_RECEIVER_HPP

#include <string>
#include <cstdint>
#include <optional>

/**
 * AlphaSignal: POD structure for insider signals from CrowdIntel.
 * Designed for zero-allocation passage through the SPSC Ring Buffer.
 */
struct AlphaSignal {
    enum class Type : uint8_t {
        WHALE_TRADE = 0,
        FUNDING_CLUSTER = 1,
        MACHINE_DETECTION = 2,
        HUMAN_SIGNAL = 3
    };

    Type type;
    char market_slug[32];   // Fixed size to avoid std::string allocation
    double confidence;      // 0.0 to 1.0
    double ev_per_dollar;   // Expected Value
    double q_value;         // False Discovery Rate
    uint64_t timestamp_ns;  // RDTSC or system clock
};

#endif // ALPHA_RECEIVER_HPP
