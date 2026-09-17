#include "order_book.hpp"
#include "spsc_ring_buffer.hpp"
#include "execution_engine.cpp"
#include "nonce_manager.hpp"
#include "lightweight_client.hpp"
#include "alpha_receiver.hpp"
#include <iostream>
#include <thread>

/**
 * MAIN HOT PATH LOOP
 * This loop must be pinned to an isolated CPU core.
 */
int main() {
    std::cout << "⚡ Starting Hot Path Execution Engine..." << std::endl;

    // 1. Initialize Core Components
    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal> alpha_queue;
    NonceManager nonce_mgr;
    LightweightCLOBClient client("API_KEY", "PASSPHRASE");
    ExecutionEngine engine(book, alpha_queue, client);

    // 2. Mock: Simulate a Cold Path signal being pushed
    // In production, this is done by the AlphaParser in a separate thread/core
    AlphaSignal mock_signal;
    mock_signal.type = AlphaSignal::Type::WHALE_TRADE;
    std::strncpy(mock_signal.market_slug, "BTC-USD-UP", 31);
    mock_signal.confidence = 0.95;
    mock_signal.ev_per_dollar = 0.05;
    mock_signal.q_value = 0.01;
    
    alpha_queue.try_push(mock_signal);

    // 3. The Tick Loop
    // Run for a few iterations to demonstrate the flow
    for (int i = 0; i < 5; ++i) {
        std::cout << "Tick " << i << "..." << std::endl;
        engine.run_tick();
    }

    std::cout << "✅ Hot Path Cycle Demonstrated." << std::endl;
    return 0;
}
