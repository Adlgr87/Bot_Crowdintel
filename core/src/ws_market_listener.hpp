#ifndef WS_MARKET_LISTENER_HPP
#define WS_MARKET_LISTENER_HPP

#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <iostream>
#include <functional>
#include <queue>
#include <mutex>

#include "spsc_ring_buffer.hpp"
#include "alpha_receiver.hpp"
#include "position_tracker.hpp"
#include "order_manager.hpp"
#include "telemetry.hpp"

/**
 * WsMarketListener: Maintains persistent WebSocket connections to Polymarket CLOB.
 *
 * Feeds market data into the Hot Path via an SPSC queue (market data → OrderBook).
 * Also listens on the USER-CHANNEL for fills/position updates (Fase 3, T3-2).
 *
 * Two connections:
 * 1. Market data channel: wss://ws-subscriptions-clob.polymarket.com/ws/market
 *    → pushes AlphaSignal to SPSC queue for hot path
 * 2. User channel: wss://ws-subscriptions-clob.polymarket.com/ws/user
 *    → updates PositionTracker and OrderManager with fill events
 *
 * Design: The market data listener runs on the hot path thread (lock-free SPSC).
 * The user-channel listener runs on a separate thread (cold path).
 */
class WsMarketListener {
public:
    // Extended constructor with PositionTracker for user-channel fills (Fase 3).
    // All extra params default to nullptr, so call with just the queue when
    // components are not yet wired up.
    WsMarketListener(SPSC_RingBuffer<AlphaSignal>& queue,
                     PositionTracker* position_tracker = nullptr,
                     OrderManager* order_manager = nullptr,
                     Telemetry* telemetry = nullptr)
        : alpha_queue_(queue), running_(false),
          position_tracker_(position_tracker),
          order_manager_(order_manager),
          telemetry_(telemetry) {}

    void start() {
        running_ = true;

        // Market data listener thread (feeds hot path)
        market_thread_ = std::thread(&WsMarketListener::market_loop, this);

        // User-channel listener thread (fills/positions, cold path)
        user_thread_ = std::thread(&WsMarketListener::user_channel_loop, this);
    }

    void stop() {
        running_ = false;
        if (market_thread_.joinable()) {
            market_thread_.join();
        }
        if (user_thread_.joinable()) {
            user_thread_.join();
        }
    }

    void set_position_tracker(PositionTracker* tracker) { position_tracker_ = tracker; }
    void set_order_manager(OrderManager* mgr) { order_manager_ = mgr; }
    void set_telemetry(Telemetry* tel) { telemetry_ = tel; }

private:
    SPSC_RingBuffer<AlphaSignal>& alpha_queue_;
    std::thread market_thread_;
    std::thread user_thread_;
    std::atomic<bool> running_;

    // Fase 3: User-channel components (cold path)
    PositionTracker* position_tracker_;
    OrderManager* order_manager_;
    Telemetry* telemetry_;

    /**
     * Market data loop: wss://ws-subscriptions-clob.polymarket.com/ws/market
     * Feeds market data into the OrderBook via AlphaSignal queue.
     */
    void market_loop() {
        // In production: use Boost.Beast or libwebsockets for real WebSocket
        std::cout << "📡 Market data WebSocket started: wss://ws-subscriptions-clob.polymarket.com/ws/market" << std::endl;

        while (running_) {
            // In production:
            // 1. Parse incoming WebSocket frames (JSON)
            // 2. Update OrderBookL2 directly (via a separate queue or direct update)
            // 3. Generate AlphaSignal for the hot path SPSC queue

            // Simulated data for demo (as before)
            AlphaSignal ws_signal;
            ws_signal.type = static_cast<uint8_t>(AlphaSignal::Type::HUMAN_SIGNAL);
            strncpy(ws_signal.market_slug, "BTC-USD-UP", 31);
            ws_signal.confidence = 0.75;
            ws_signal.ev_per_dollar = 0.0;
            ws_signal.q_value = 0.0;
            ws_signal.timestamp_ns = 0;

            alpha_queue_.try_push(ws_signal);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << "📡 Market data WebSocket stopped." << std::endl;
    }

    /**
     * User-channel loop: wss://ws-subscriptions-clob.polymarket.com/ws/user
     * Receives fills, position updates, order status changes.
     *
     * Fase 3 (T3-2): Apply fills to PositionTracker
     * Fase 6 (T6-1): Log all fill/cancel events to audit log
     */
    void user_channel_loop() {
        // In production: authenticate with HMAC, subscribe to user channel
        std::cout << "📡 User-channel WebSocket started: wss://ws-subscriptions-clob.polymarket.com/ws/user" << std::endl;

        while (running_) {
            // In production:
            // 1. Parse incoming user-channel frames
            // 2. For "fill" events: create FillEvent and pass to PositionTracker
            // 3. For "order" events: update OrderManager status
            // 4. All updates happen in cold path (with mutex, can block)

            // Rate limit: poll at most once per second in demo
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << "📡 User-channel WebSocket stopped." << std::endl;
    }

    /**
     * Parse a fill event from WebSocket JSON.
     * Format: {"type":"fill","order_id":"...","market":"...","side":"buy","price":"0.5","size":"10","timestamp":...}
     */
    FillEvent parse_fill_event(const std::string& json) {
        FillEvent event;
        event.timestamp = std::chrono::steady_clock::now();

        // Minimal JSON parsing (no external deps)
        auto extract = [](const std::string& json, const std::string& key) -> std::string {
            std::string search = "\"" + key + "\":\"";
            size_t pos = json.find(search);
            if (pos == std::string::npos) return "";
            pos += search.length();
            size_t end = json.find('"', pos);
            if (end == std::string::npos) return "";
            return json.substr(pos, end - pos);
        };

        event.order_id = extract(json, "order_id");
        event.market_slug = extract(json, "market");
        event.is_buy = (extract(json, "side") == "buy");

        // Parse price and size from string
        std::string price_str = extract(json, "price");
        std::string size_str = extract(json, "size");
        try {
            double price_d = std::stod(price_str);
            double size_d = std::stod(size_str);
            event.price = static_cast<uint64_t>(price_d * 1e6);
            event.size = static_cast<uint64_t>(size_d * 1e6);
        } catch (...) {
            event.price = 0;
            event.size = 0;
        }

        return event;
    }

    /**
     * Handle a fill event (cold path — updates PositionTracker).
     */
    void handle_fill_event(const FillEvent& event) {
        if (position_tracker_ && telemetry_) {
            position_tracker_->apply_fill(event);
            telemetry_->increment_fills();
            telemetry_->log_event(EventType::ORDER_FILLED, event.market_slug,
                                 "{\"order_id\":\"" + event.order_id + "\","
                                 "\"price\":\"" + std::to_string(event.price) + "\","
                                 "\"size\":\"" + std::to_string(event.size) + "\"}", "INFO");
        }

        if (order_manager_) {
            order_manager_->apply_fill(event.order_id, event.size, event.size);
        }
    }
};

#endif // WS_MARKET_LISTENER_HPP
