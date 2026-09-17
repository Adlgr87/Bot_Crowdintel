#ifndef WS_MARKET_LISTENER_HPP
#define WS_MARKET_LISTENER_HPP

#include <string>
#include <thread>
#include <atomic>
#include <iostream>

#include "spsc_ring_buffer.hpp"
#include "alpha_receiver.hpp"

/**
 * WsMarketListener: Maintains a persistent WebSocket connection to Polymarket CLOB.
 * Feeds market data into the Hot Path via an SPSC queue.
 */
class WsMarketListener {
public:
    WsMarketListener(SPSC_RingBuffer<AlphaSignal>& queue) 
        : alpha_queue_(queue), running_(false) {}

    void start() {
        running_ = true;
        listener_thread_ = std::thread(&WsMarketListener::run_loop, this);
    }

    void stop() {
        running_ = false;
        if (listener_thread_.joinable()) {
            listener_thread_.join();
        }
    }

private:
    SPSC_RingBuffer<AlphaSignal>& alpha_queue_;
    std::thread listener_thread_;
    std::atomic<bool> running_;

    void run_loop() {
        // A production-ready listener would use a proper WebSocket library like:
        // - Boost.Beast (C++)
        // - websocketpp (C++)
        // - or a bare libwebsockets C wrapper.
        
        // For demonstration, we use a simplified loop.
        // The connection is persistent; data arrives as JSON text frames.
        
        std::cout << "📡 WebSocket Listener started. Connecting to wss://ws-subscriptions-clob.polymarket.com/ws/market..." << std::endl;
        
        while(running_) {
            // In production:
            // 1. Curl performs a WebSocket upgrade handshake (using CURLINFO_ACTIVESOCKET or similar).
            // 2. Then enters a loop to recv() frames.
            // 3. Parse JSON frame (e.g., {"type": "trade", "price": "0.5", ...}).
            // 4. Push parsed event into the SPSC queue to update OrderBookL2.

            // Simulated data arrival for demo purposes.
            // In the real bot, this would be triggered by an actual incoming frame.
            AlphaSignal ws_signal;
            ws_signal.type = AlphaSignal::Type::HUMAN_SIGNAL; // A generic market event
            strncpy(ws_signal.market_slug, "BTC-USD-UP", 31);
            ws_signal.confidence = 0.75; // Low confidence for raw market data
            ws_signal.ev_per_dollar = 0.0; // Raw data has no inherent EV
            ws_signal.q_value = 0.0;

            alpha_queue_.try_push(ws_signal);
            
            // A real loop would sleep/poll based on incoming data, not a fixed interval.
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); 
        }
        std::cout << "📡 WebSocket Listener stopped." << std::endl;
    }
};

#endif // WS_MARKET_LISTENER_HPP
