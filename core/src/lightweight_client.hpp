#ifndef LIGHTWEIGHT_CLIENT_HPP
#define LIGHTWEIGHT_CLIENT_HPP

#include <string>
#include <vector>
#include <array>
#include <iostream>

/**
 * LightweightCLOBClient: Custom HTTP/WS client for Polymarket.
 * Bypasses heavy SDKs and JSON libraries in the Hot Path.
 * Uses pre-allocated buffers for request construction.
 */
struct SignedOrder {
    uint64_t nonce;
    std::array<uint8_t, 65> signature;
    std::string payload;
};

class LightweightCLOBClient {
public:
    LightweightCLOBClient(const std::string& api_key, const std::string& passphrase)
        : api_key_(api_key), passphrase_(passphrase) {}

    // Submit order with zero-allocation in the request body
    bool submit_order(const SignedOrder& order) {
        // In production:
        // 1. Build raw HTTP request string in a pre-allocated buffer.
        // 2. Send via TCP_NODELAY socket.
        // 3. Parse response headers only.
        
        std::cout << "🌐 Submitting Order | Nonce: " << order.nonce << " | Sig: " << (int)order.signature[0] << "..." << std::endl;
        return true;
    }

private:
    std::string api_key_;
    std::string passphrase_;
};

#endif // LIGHTWEIGHT_CLIENT_HPP
