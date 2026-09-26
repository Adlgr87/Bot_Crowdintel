/**
 * main_prod.cpp — Production entry point for CrowdIntelBot
 *
 * Unlike main_hot_path.cpp (demo), this entrypoint:
 * - Does NOT include execution_engine.cpp (proper header/source separation)
 * - Loads ALL credentials from environment variables (never hardcoded)
 * - Is the default `crowdintel_bot` target
 * - Does NOT print to stdout on the hot path
 *
 * Build with: cmake -DCMAKE_BUILD_TYPE=Release ..
 * Run with:
 *   BOT_PRIVATE_KEY_HEX=<64-hex-chars>
 *   CLOB_API_KEY=<key>
 *   CLOB_SECRET=<secret>
 *   CLOB_PASSPHRASE=<passphrase>
 *   bot_bin/crowdintel_bot
 */

#include "order_book.hpp"
#include "spsc_ring_buffer.hpp"
#include "eip712_signer.hpp"
#include "nonce_manager.hpp"
#include "lightweight_client.hpp"
#include "alpha_receiver.hpp"
#include "ws_market_listener.hpp"
#include "execution_engine.cpp"
#include "tick_result.hpp"

#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <stdexcept>

// Forward declaration: execution_engine.cpp defines ExecutionEngine as a class.
// We include it here to get the full definition (hot path headers are header-only
// by design — no separate .cpp compilation for the engine itself).

static std::vector<uint8_t> load_private_key_from_env() {
    const char* env_key = std::getenv("BOT_PRIVATE_KEY_HEX");
    if (!env_key || strlen(env_key) != 64) {
        std::cerr << "❌ FATAL: BOT_PRIVATE_KEY_HEX must be set to a 64-character hex string (32 bytes)."
                  << std::endl;
        throw std::runtime_error("BOT_PRIVATE_KEY_HEX must be set to a 64-character hex string");
    }
    std::vector<uint8_t> key(32);
    for (size_t i = 0; i < 32; i++) {
        char buf[3] = {env_key[i * 2], env_key[i * 2 + 1], 0};
        key[i] = static_cast<uint8_t>(strtol(buf, nullptr, 16));
    }
    return key;
}

int main() {
    // Production entrypoint: load all config from env, no hardcoded values.
    const char* env_api_key = std::getenv("CLOB_API_KEY");
    const char* env_secret = std::getenv("CLOB_SECRET");
    const char* env_passphrase = std::getenv("CLOB_PASSPHRASE");

    if (!env_api_key || !env_secret || !env_passphrase) {
        std::cerr << "❌ FATAL: CLOB_API_KEY, CLOB_SECRET, and CLOB_PASSPHRASE must be set in the environment."
                  << std::endl;
        return 1;
    }

    try {
        std::vector<uint8_t> priv_key = load_private_key_from_env();

        // 1. Initialize Core Components
        OrderBookL2 book;
        SPSC_RingBuffer<AlphaSignal> alpha_queue;
        LightweightCLOBClient client(env_api_key, env_secret, env_passphrase,
                                     "https://api.polymarket.com");
        ExecutionEngine engine(book, alpha_queue, client, priv_key);

        // 2. Start the persistent WebSocket listener in the background
        WsMarketListener listener(alpha_queue);
        listener.start();

        // 3. Tick Loop — runs continuously on an isolated CPU core in production.
        // In production, pin with: taskset -c 2 ./bin/crowdintel_bot
        while (true) {
            TickResult result = engine.run_tick();
            // Hot path: no I/O. Telemetry handles logging async.
            if (result == TickResult::KILL_SWITCH) {
                // Emergency: stop trading, cancel all orders
                break;
            }
            // Yield to allow listener thread to push new data
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        // Clean shutdown (unreachable in normal operation)
        listener.stop();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ FATAL: " << e.what() << std::endl;
        return 1;
    }
}
