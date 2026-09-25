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
#include <cstdlib>

/**
 * BenchExecutionEngine: A variant of ExecutionEngine that uses MockCLOBClient
 * to avoid network I/O during latency benchmarking.
 *
 * Security: Private key is loaded from BOT_PRIVATE_KEY_HEX env var
 * (never hardcoded in source). Falls back to a zero key for benchmarking only.
 */
class BenchExecutionEngine {
public:
    BenchExecutionEngine(OrderBookL2& book, SPSC_RingBuffer<AlphaSignal>& alpha_queue)
        : book_(book), alpha_queue_(alpha_queue),
          signer_(load_bench_key()), nonce_mgr_() {}

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
    static std::vector<uint8_t> load_bench_key() {
        // Load private key from env (same as production path).
        // For benchmarks only: if BOT_PRIVATE_KEY_HEX is not set, use a
        // deterministic zero key so the benchmark can run without setup.
        // This is NOT a production key — it is only used for performance measurement.
        const char* env_key = std::getenv("BOT_PRIVATE_KEY_HEX");
        if (env_key && strlen(env_key) == 64) {
            std::vector<uint8_t> key(32);
            for (size_t i = 0; i < 32; i++) {
                char buf[3] = {env_key[i * 2], env_key[i * 2 + 1], 0};
                key[i] = static_cast<uint8_t>(strtol(buf, nullptr, 16));
            }
            return key;
        }
        // Benchmark fallback: deterministic non-zero key (NOT for production use).
        // Must be in range [1, n-1] where n is the secp256k1 curve order.
        // 0x01 * 32 is valid (well within curve order).
        return std::vector<uint8_t>(32, 0x01);
    }

    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& alpha_queue_;
    MockCLOBClient client_;
    EIP712Signer signer_;
    NonceManager nonce_mgr_;
};

#endif // BENCH_ENGINE_HPP
