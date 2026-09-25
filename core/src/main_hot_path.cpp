#include "order_book.hpp"
#include "spsc_ring_buffer.hpp"
#include "execution_engine.hpp"
#include "nonce_manager.hpp"
#include "lightweight_client.hpp"
#include "alpha_receiver.hpp"
#include "ws_market_listener.hpp"
#include <iostream>
#include <cstdlib>
#include <thread>
#include <chrono>

/**
 * MAIN HOT PATH LOOP
 * This loop must be pinned to an isolated CPU core.
 */
int main() {
    std::cout << "⚡ Starting Hot Path Execution Engine (CROWDINTEL v1.0)..." << std::endl;

    // 1. Initialize Core Components
    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal> alpha_queue;
    NonceManager nonce_mgr;
    
    // Load credentials from environment — NEVER hardcode in production entrypoint
    const char* env_api_key = std::getenv("CLOB_API_KEY");
    const char* env_secret = std::getenv("CLOB_SECRET");
    const char* env_passphrase = std::getenv("CLOB_PASSPHRASE");
    if (!env_api_key || !env_secret || !env_passphrase) {
        std::cerr << "❌ FATAL: CLOB_API_KEY, CLOB_SECRET, and CLOB_PASSPHRASE must be set in the environment."
                  << std::endl;
        return 1;
    }
    LightweightCLOBClient client(env_api_key, env_secret, env_passphrase, "https://clob.polymarket.com");
    ExecutionEngine engine(book, alpha_queue, client);

    // 2. Start the persistent WebSocket listener in the background
    WsMarketListener listener(alpha_queue);
    listener.start();

    // Give the listener a moment to start
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 3. Tick Loop - Processes signals from the queue
    // In a real deployment, this runs continuously on a pinned core.
    std::cout << "--- Running 10 ticks to demonstrate the flow ---" << std::endl;
    for (int i = 0; i < 10; ++i) {
        engine.run_tick();
        // Yield to allow the listener thread to push new data
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 4. Clean shutdown
    listener.stop();
    std::cout << "✅ Hot Path Cycle Demonstrated. Listener Stopped." << std::endl;
    return 0;
}
