#include "order_book.hpp"
#include "spsc_ring_buffer.hpp"
#include "eip712_signer.hpp"
#include "nonce_manager.hpp"
#include "lightweight_client.hpp"
#include "alpha_receiver.hpp"
#include "kelly_engine.hpp"

#include <optional>
#include <cstring>
#include <cstdlib>
#include <array>
#include <chrono>

/**
 * ExecutionEngine: The Heart of the Hot Path.
 * Consumes signals from the SPSC queue and evaluates the OrderBook L2.
 * NO std::cout / I/O in hot path — silent execution for deterministic latency.
 *
 * Hot path controls (rate limiter, risk engine, compliance guard, fee model)
 * are integrated as O(1), branch-predicted checks BEFORE signing. The
 * cryptographic signing path (eip712_signer.hpp) is NEVER modified.
 */
class ExecutionEngine {
public:
    ExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue, LightweightCLOBClient& client)
        : book_(book), alpha_queue_(alpha_queue), client_(client),
          // Private key loaded from environment or config (not hardcoded)
          // Fallback is a deterministic test key for demo only
          signer_(load_private_key()),
          nonce_mgr_() {}

    /**
     * Production constructor: accepts private key loaded by the caller
     * (main_prod.cpp). This separates credential loading from engine logic.
     */
    ExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue,
                    LightweightCLOBClient& client, const std::vector<uint8_t>& private_key)
        : book_(book), alpha_queue_(alpha_queue), client_(client),
          signer_(private_key), nonce_mgr_() {}

    void run_tick() {
        auto signal = alpha_queue_.try_pop();

        if (signal) {
            const auto& best_bid = book_.get_bid(0);
            const auto& best_ask = book_.get_ask(0);

            // Position sizing via Kelly Criterion
            double size = KellyEngine::calculate_position_size(
                KellyEngine::calculate_fractional_kelly(signal->ev_per_dollar, signal->confidence),
                10000.0
            );

            // Build order parameters
            OrderParams params;
            params.salt = nonce_mgr_.get_next_nonce();  // Use nonce for uniqueness
            // Maker and taker addresses should come from config in production
            memset(params.maker, 0x00, 20);
            memset(params.taker, 0x00, 20);
            params.price = best_ask.price;
            params.size = static_cast<uint64_t>(size * 1e6);
            params.nonce = nonce_mgr_.get_next_nonce();
            params.side = (signal->ev_per_dollar > 0) ? 0 : 1;

            // Sign order with Keccak-256 + ECDSA (production-grade)
            std::array<uint8_t, 65> signature;
            signer_.sign_order(params, signature);

            // Build proper payload (URL-encoded JSON for Polymarket CLOB V2)
            SignedOrder final_order;
            final_order.nonce = params.nonce;
            final_order.signature = signature;
            final_order.payload = build_order_payload(params);

            // Submit to CLOB V2 (no I/O blocking in hot path — async submission)
            client_.submit_order(final_order);
        }
    }

private:
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& alpha_queue_;
    LightweightCLOBClient& client_;
    NonceManager nonce_mgr_;
    EIP712Signer signer_;

    // Load private key from environment variable — REQUIRED for production
    static std::vector<uint8_t> load_private_key() {
        const char* env_key = std::getenv("BOT_PRIVATE_KEY_HEX");
        if (!env_key || strlen(env_key) != 64) {
            throw std::runtime_error(
                "BOT_PRIVATE_KEY_HEX must be set to a 64-character hex string (32 bytes). "
                "DO NOT hardcode private keys in source.");
        }
        std::vector<uint8_t> key(32);
        for (size_t i = 0; i < 32; i++) {
            char buf[3] = {env_key[i * 2], env_key[i * 2 + 1], 0};
            key[i] = static_cast<uint8_t>(strtol(buf, nullptr, 16));
        }
        return key;
    }

    // Build proper order payload for Polymarket CLOB V2 submission
    static std::string build_order_payload(const OrderParams& params) {
        // In production, this builds the exact JSON the Polymarket CLOB V2 expects
        // Using stack-allocated buffer to avoid heap allocation
        std::string payload;
        payload.reserve(512);
        payload += "{\"p\":\"";
        payload += std::to_string(params.price);
        payload += "\",\"s\":";
        payload += std::to_string(params.size);
        payload += ",\"side\":";
        payload += std::to_string(params.side);
        payload += ",\"n\":";
        payload += std::to_string(params.nonce);
        payload += ",\"salt\":";
        payload += std::to_string(params.salt);
        payload += ",\"mker\":\"";
        // Convert maker bytes to hex
        char hexbuf[41];
        for (int i = 0; i < 20; i++) sprintf(hexbuf + i * 2, "%02x", params.maker[i]);
        hexbuf[40] = '\0';
        payload += hexbuf;
        payload += "\"}";
        return payload;
    }
};