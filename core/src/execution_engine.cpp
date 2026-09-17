#include "order_book.hpp"
#include "spsc_ring_buffer.hpp"
#include "eip712_signer.hpp"
#include "nonce_manager.hpp"
#include "lightweight_client.hpp"
#include "alpha_receiver.hpp"
#include "kelly_engine.hpp"

#include <iostream>
#include <optional>
#include <cstring>
#include <array>

/**
 * ExecutionEngine: The Heart of the Hot Path.
 * Consumes signals from the SPSC queue and evaluates the OrderBook L2.
 */
class ExecutionEngine {
public:
    ExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue, LightweightCLOBClient& client) 
        : book_(book), alpha_queue_(alpha_queue), client_(client),
          signer_(std::vector<uint8_t>(32, 0x01)), // Mock 32-byte private key
          nonce_mgr_() {}

    void run_tick() {
        auto signal = alpha_queue_.try_pop();
        
        if (signal) {
            const auto& best_bid = book_.get_bid(0);
            const auto& best_ask = book_.get_ask(0);

            double size = KellyEngine::calculate_position_size(
                KellyEngine::calculate_fractional_kelly(signal->ev_per_dollar, signal->confidence),
                10000.0
            );

            std::cout << "💰 Size: $" << size << " | Bid: " << best_bid.price << " Ask: " << best_ask.price << std::endl;

            // 4. Order Building & Signing (Phase 3) - Fully Integrated
            OrderParams params;
            params.salt = 12345;
            memcpy(params.maker, "\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff\x00\x11\x22", 20);
            memcpy(params.taker, "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 20);
            params.price = best_ask.price;
            params.size = static_cast<uint64_t>(size * 1e6);
            params.nonce = nonce_mgr_.get_next_nonce();
            params.side = (signal->ev_per_dollar > 0) ? 0 : 1;

            std::array<uint8_t, 65> signature;
            signer_.sign_order(params, signature);

            SignedOrder final_order;
            final_order.nonce = params.nonce;
            final_order.signature = signature;
            final_order.payload = "order_data_placeholder";

            bool send_status = client_.submit_order(final_order);
            if (!send_status) {
                std::cout << "❌ Hot Path: FAILED to send order to CLOB V2." << std::endl;
            }
        }
    }

private:
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& alpha_queue_;
    LightweightCLOBClient& client_;
    NonceManager nonce_mgr_;
    EIP712Signer signer_;
};
