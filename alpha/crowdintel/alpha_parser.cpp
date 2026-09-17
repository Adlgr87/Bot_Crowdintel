#include "alpha_receiver.hpp"
#include "spsc_ring_buffer.hpp"
#include <iostream>
#include <cstring>

/**
 * AlphaParser: Cold Path component that transforms JSON-like alerts 
 * into binary AlphaSignal PODs and pushes them to the Hot Path.
 */
class AlphaParser {
public:
    explicit AlphaParser(SPSC_RingBuffer<AlphaSignal>& queue) : queue_(queue) {}

    // Simulates receiving a webhook payload
    bool process_webhook_payload(const std::string& market, double conf, double ev, double q_val) {
        // 1. Statistical Filter (FDR q-value)
        // Reject signals with high false discovery rate
        if (q_val > 0.05) {
            return false; 
        }

        // 2. Confidence Filter
        if (conf < 0.85) {
            return false;
        }

        // 3. EV Filter
        if (ev < 0.02) {
            return false;
        }

        // 4. Construct POD signal
        AlphaSignal signal;
        signal.type = AlphaSignal::Type::WHALE_TRADE;
        
        // Safe copy of market slug to fixed-size buffer
        std::memset(signal.market_slug, 0, sizeof(signal.market_slug));
        std::strncpy(signal.market_slug, market.c_str(), sizeof(signal.market_slug) - 1);
        
        signal.confidence = conf;
        signal.ev_per_dollar = ev;
        signal.q_value = q_val;
        signal.timestamp_ns = 123456789; // Mock timestamp

        // 5. Push to Hot Path (Lock-Free)
        return queue_.try_push(signal);
    }

private:
    SPSC_RingBuffer<AlphaSignal>& queue_;
};
