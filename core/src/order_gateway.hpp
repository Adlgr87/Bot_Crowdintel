#ifndef ORDER_GATEWAY_HPP
#define ORDER_GATEWAY_HPP

// Asynchronous order egress.  The decision thread only copies len bytes into a
// bounded SPSC queue; HMAC, curl allocation and TCP/TLS waits live here.  The
// gateway retries only responses explicitly classified as safe/temporary by
// the client and exposes venue outcomes separately from enqueue outcomes.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include "../include/spsc_ring_buffer.hpp"
#include "polymarket_order.hpp"

// Motor C: a per-order withdrawal queued to the same worker that submits, so a
// cancel can never overtake the placement it targets.
struct CancelRequest {
    char order_id[72]{};  // "0x" + 64 hex + NUL
};

template <typename Client, size_t Capacity = 128>
class OrderGateway {
public:
    explicit OrderGateway(Client& client,
                          const std::atomic<bool>* trading_enabled = nullptr)
        : client_(client), trading_enabled_(trading_enabled) {}
    ~OrderGateway() { stop(false); }

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;

    // Enqueue a per-order withdrawal. Ordering with respect to submissions is
    // preserved because both are drained by the same worker: a cancel cannot
    // race ahead of the placement it refers to.
    //
    // Returns false when the id is not a bounded hex string or the queue is
    // full. A silently dropped cancel is a quote left exposed, so the caller
    // must be able to see it.
    bool cancel(const char* order_id) {
        if (!valid_cancel_id(order_id)) return false;
        CancelRequest req{};
        const size_t len = std::strlen(order_id);
        std::memcpy(req.order_id, order_id, len);
        req.order_id[len] = '\0';
        return cancel_q_.try_push(req);
    }

    uint64_t cancelled_orders() const {
        return cancelled_orders_.load(std::memory_order_relaxed);
    }

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        drain_.store(false, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
    }

    // Emergency/default stop discards unsent work. A controlled test or
    // supervised shutdown may explicitly drain already authorized orders.
    void stop(bool drain = false) {
        drain_.store(drain, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        if (!drain) {
            QueuedBody discarded{};
            while (queue_.try_pop(discarded))
                cancelled_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    SubmitResult submit(const WireBody& body) {
        if (trading_enabled_ &&
            !trading_enabled_->load(std::memory_order_acquire)) {
            cancelled_.fetch_add(1, std::memory_order_relaxed);
            SubmitResult result{};
            std::snprintf(result.error, sizeof(result.error), "trading disabled");
            return result;
        }
        QueuedBody queued{};
        queued.len = body.len;
        if (queued.len >= sizeof(queued.buf)) {
            SubmitResult result{};
            std::snprintf(result.error, sizeof(result.error), "wire body too large");
            return result;
        }
        std::memcpy(queued.buf, body.buf, body.len);
        if (!queue_.try_push(queued)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            SubmitResult result{};
            std::snprintf(result.error, sizeof(result.error), "order gateway queue full");
            return result;
        }
        enqueued_.fetch_add(1, std::memory_order_relaxed);
        SubmitResult result{};
        result.ok = true;
        result.final = false;
        std::snprintf(result.status, sizeof(result.status), "queued");
        return result;
    }

    uint64_t enqueued() const { return enqueued_.load(std::memory_order_relaxed); }
    uint64_t accepted() const { return accepted_.load(std::memory_order_relaxed); }
    uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t retried() const { return retried_.load(std::memory_order_relaxed); }
    uint64_t cancelled() const { return cancelled_.load(std::memory_order_relaxed); }

private:
    struct QueuedBody {
        char buf[1536];
        size_t len = 0;
    };

    void run() {
        if constexpr (requires(Client& c) { c.warmup(); }) client_.warmup();

        QueuedBody queued{};
        CancelRequest cancel_req{};
        while (running_.load(std::memory_order_acquire) ||
               (drain_.load(std::memory_order_acquire) &&
                (!queue_.empty() || !cancel_q_.empty()))) {
            // Cancels are drained first and are NOT gated on trading_enabled:
            // withdrawing a quote is risk-reducing, and refusing to withdraw
            // while the kill switch is latched is exactly the wrong direction.
            if (cancel_q_.try_pop(cancel_req)) {
                if constexpr (requires(Client& c, const char* id) {
                                  c.cancel_order(id);
                              }) {
                    if (client_.cancel_order(cancel_req.order_id))
                        cancelled_orders_.fetch_add(1,
                                                    std::memory_order_relaxed);
                }
                continue;  // one item per iteration: keeps submit/cancel fair
            }
            if (!queue_.try_pop(queued)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                continue;
            }
            if (trading_enabled_ &&
                !trading_enabled_->load(std::memory_order_acquire)) {
                cancelled_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            WireBody body{};
            body.len = queued.len;
            std::memcpy(body.buf, queued.buf, queued.len);
            body.buf[body.len] = '\0';

            SubmitResult result{};
            bool was_cancelled = false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (trading_enabled_ &&
                    !trading_enabled_->load(std::memory_order_acquire)) {
                    cancelled_.fetch_add(1, std::memory_order_relaxed);
                    was_cancelled = true;
                    std::snprintf(result.error, sizeof(result.error),
                                  "trading disabled before egress");
                    break;
                }
                result = client_.submit(body);
                if (result.ok || !result.retryable) break;
                retried_.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50 * (1 << attempt)));
            }
            if (was_cancelled) continue;
            if (result.ok) accepted_.fetch_add(1, std::memory_order_relaxed);
            else rejected_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    Client& client_;
    const std::atomic<bool>* trading_enabled_;
    static bool valid_cancel_id(const char* id) {
        if (!id) return false;
        const size_t len = std::strlen(id);
        if (len < 4 || len >= sizeof(CancelRequest::order_id)) return false;
        if (id[0] != '0' || (id[1] != 'x' && id[1] != 'X')) return false;
        for (size_t i = 2; i < len; ++i) {
            const char c = id[i];
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                             (c >= 'A' && c <= 'F');
            if (!hex) return false;
        }
        return true;
    }

    SPSC_RingBuffer<QueuedBody, Capacity> queue_;
    SPSC_RingBuffer<CancelRequest, Capacity> cancel_q_;
    std::atomic<bool> running_{false};
    std::atomic<bool> drain_{false};
    std::thread thread_;
    std::atomic<uint64_t> enqueued_{0};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> retried_{0};
    std::atomic<uint64_t> cancelled_{0};
    std::atomic<uint64_t> cancelled_orders_{0};
};

#endif  // ORDER_GATEWAY_HPP
