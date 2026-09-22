#include "alpha_receiver.hpp"
#include "spsc_ring_buffer.hpp"

#include <chrono>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// AlphaParser (cold path): CrowdIntel webhook payload → binary AlphaSignal.
//
// Applies the statistical filters that do NOT depend on live market data:
//   1. FDR q-value  (reject if q > max_q)
//   2. Confidence   (reject if conf < min_conf)
//   3. p_win sanity (reject outside (0,1))
// Economic filters (edge vs live price) are applied by the engine, which sees
// the order book — the parser deliberately does not.
// ─────────────────────────────────────────────────────────────────────────────

class AlphaParser {
public:
    AlphaParser(SPSC_RingBuffer<AlphaSignal>& queue,
                double max_q = 0.05, double min_conf = 0.85)
        : queue_(queue), max_q_(max_q), min_conf_(min_conf) {}

    bool process_webhook_payload(const char* market, double p_win,
                                 double conf, double q_val,
                                 uint8_t direction_hint = 2) {
        if (q_val > max_q_ || conf < min_conf_) return false;
        if (!(p_win > 0.0 && p_win < 1.0))     return false;

        AlphaSignal s{};
        s.type = AlphaSignal::Type::WHALE_TRADE;
        s.direction_hint = direction_hint;
        s.p_win = p_win;
        s.confidence = conf;
        s.q_value = q_val;
        s.timestamp_ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (market) {
            std::strncpy(s.market_slug, market, sizeof(s.market_slug) - 1);
            s.market_slug[sizeof(s.market_slug) - 1] = '\0';
        }
        return queue_.try_push(s);
    }

private:
    SPSC_RingBuffer<AlphaSignal>& queue_;
    double max_q_;
    double min_conf_;
};
