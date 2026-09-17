#ifndef BENCH_ENGINE_HPP
#define BENCH_ENGINE_HPP

#include "order_book.hpp"
#include "spsc_ring_buffer.hpp"
#include "mock_client.hpp"
#include "kelly_engine.hpp"
#include "alpha_receiver.hpp"
#include "eip712_signer.hpp"
#include "nonce_manager.hpp"
#include <cstring>
#include <array>

/**
 * BenchExecutionEngine: A variant of ExecutionEngine that uses MockCLOBClient
 * to avoid network I/O during latency benchmarking.
 */
class BenchExecutionEngine {
public:
    BenchExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue)
        : book_(book), alpha_queue_(alpha_queue), signer_(std::vector<uint8_t>(32, 0xAA)), nonce_mgr_() {}

    void run_tick() {
        auto signal = alpha_queue_.try_pop();
        if (signal) {
            const auto& best_ask = book_.get_ask(0);
            double size = KellyEngine::calculate_position_size(
                KellyEngine::calculate_fractional_kelly(signal->ev_per_dollar, signal->confidence), 10000.0);

            // Sign the order (real crypto op)
            OrderParams params;
            params.salt = 0xCAFEBABE;
            memset(params.maker, 0x11, 20);
            memset(params.taker, 0x00, 20);
            params.price = best_ask.price;
            params.size = static_cast<uint64_t>(size * 1e6);
            params.nonce = nonce_mgr_.get_next_nonce();
            params.side = 0;
            std::array<uint8_t, 65> signature;
            signer_.sign_order(params, signature);

            // Submit (mock, no I/O)
            MockOrder order;
            order.nonce = params.nonce;
            order.signature = signature;
            client_.submit_order(order);
        }
    }

private:
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& alpha_queue_;
    MockCLOBClient client_;
    EIP712Signer signer_;
    NonceManager nonce_mgr_;
};

#endif // BENCH_ENGINE_HPP
