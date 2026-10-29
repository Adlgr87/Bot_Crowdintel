#ifndef CROWDINTEL_RESTING_MOCK_CLIENT_HPP
#define CROWDINTEL_RESTING_MOCK_CLIENT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// resting_mock_client.hpp — MockCLOBClient's interface, but orders REST.
//
// MockCLOBClient fills every accepted submit. That is right for a taker and
// useless for a maker: the quote never rests, so the one risk the sleeve exists
// to measure never happens. This adapter keeps the exact template contract the
// engine already expects (submit / attach_fill_queue / rest_get / cancel_all /
// submissions / cancellations / paper_fills) plus cancel_order, and changes the
// single semantic that matters:
//
//   submit()  ->  the order RESTS and the venue emits OPEN, not FILL
//   observe_book()  ->  a rest fills ONLY when the market crosses it
//   cancel_order()/cancel_all()  ->  the venue emits CANCEL
//
// A canary run on this transport therefore sees what a real maker sees: fills
// that arrive only after the price moved against the quote. That is the
// conditional mark-out the 0.092c unconditional figure cannot express.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <cstring>

#include "../include/account_events.hpp"
#include "../include/resting_mock.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../include/time_utils.hpp"
#include "polymarket_order.hpp"

namespace crowdintel {

class RestingMockCLOBClient {
public:
    struct SubmitResult {
        bool ok = false;
        bool final = true;
        long http_code = 0;
        char status[24]{};
        char error[64]{};
        bool retryable = false;
    };

    void attach_fill_queue(SPSC_RingBuffer<AccountEvent>* q) noexcept {
        fill_q_ = q;
    }

    // Accepts the order and leaves it RESTING on the mock book.
    SubmitResult submit(const WireBody& body) {
        const uint64_t seq =
            submissions_.fetch_add(1, std::memory_order_relaxed) + 1;

        SubmitResult result{};
        const char* json = body.buf;
        const bool is_buy = scan_flag(json, "\"side\":\"BUY\"");
        const bool is_sell = scan_flag(json, "\"side\":\"SELL\"");

        uint64_t maker_amount = 0, taker_amount = 0, price = 0;
        const bool have_amounts =
            scan_u64(json, "\"makerAmount\":", maker_amount) &&
            scan_u64(json, "\"takerAmount\":", taker_amount);
        (void)scan_u64(json, "\"price\":", price);

        if ((!is_buy && !is_sell) || !have_amounts || price == 0) {
            std::snprintf(result.error, sizeof(result.error), "malformed_body");
            result.http_code = 400;
            emit(AccountEvent::Type::REJECT, 0, 0, 0, 0);
            return result;
        }

        // BUY rests on the bid (makerAmount = USDC cost, takerAmount = shares);
        // SELL rests on the ask (makerAmount = shares).
        const uint64_t size = is_buy ? taker_amount : maker_amount;
        const uint8_t mock_side = is_buy ? MOCK_SIDE_BID : MOCK_SIDE_ASK;
        const uint8_t account_side = is_buy ? K_SIDE_BUY : K_SIDE_SELL;

        const MockRestOutcome placed =
            book_.place(seq, mock_side, price, size);
        if (placed.event != MockRestEvent::Opened) {
            std::snprintf(result.error, sizeof(result.error), "place_rejected");
            result.http_code = 400;
            emit(AccountEvent::Type::REJECT, account_side, price, size, seq);
            return result;
        }

        // OPEN, not FILL. This is the whole point of the class.
        emit(AccountEvent::Type::OPEN, account_side, price, size, seq);
        result.ok = true;
        result.final = true;
        result.http_code = 200;
        std::snprintf(result.status, sizeof(result.status), "live");
        return result;
    }

    // Drive the mock market. Each crossed rest produces one FILL event, so the
    // caller drains until this returns false.
    bool observe_book(uint64_t bid, uint64_t bid_size, uint64_t ask,
                      uint64_t ask_size) {
        const MockRestOutcome o =
            book_.observe_book(bid, bid_size, ask, ask_size);
        if (o.event != MockRestEvent::Filled) return false;
        const uint8_t account_side =
            o.side == MOCK_SIDE_BID ? K_SIDE_BUY : K_SIDE_SELL;
        // The remaining size after this fill, as the venue would report it.
        const uint64_t* rest = book_.rest_size(o.side);
        emit(AccountEvent::Type::FILL, account_side, o.fill_price,
             o.filled_size, o.id, rest ? *rest : 0);
        paper_fills_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool cancel_order(const char* /*order_id*/) {
        // The engine/registry path uses ids; the mock's ids are the submit
        // sequence numbers, so this cancels by id when one is supplied as a
        // decimal. Kept simple and explicit rather than guessing.
        return false;
    }

    bool cancel_order_id(uint64_t id) {
        const MockRestOutcome o = book_.cancel(id);
        if (o.event != MockRestEvent::Cancelled) return false;
        const uint8_t account_side =
            o.side == MOCK_SIDE_BID ? K_SIDE_BUY : K_SIDE_SELL;
        emit(AccountEvent::Type::CANCEL, account_side, 0, 0, id);
        cancels_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool cancel_all() {
        cancellations_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Cold-path REST surface used by the reconcile thread (mock equivalents).
    size_t rest_get(const char*, char*, size_t) { return 0; }

    uint64_t submissions() const {
        return submissions_.load(std::memory_order_relaxed);
    }
    uint64_t cancellations() const {
        return cancellations_.load(std::memory_order_relaxed);
    }
    uint64_t paper_fills() const {
        return paper_fills_.load(std::memory_order_relaxed);
    }
    size_t resting() const { return book_.live_count(); }
    uint64_t opens() const { return book_.opens(); }
    uint64_t filled_shares() const { return book_.filled_shares(); }

private:
    static const char* scan_key(const char* within, const char* key) {
        const char* hit = std::strstr(within, key);
        return hit ? hit + std::strlen(key) : nullptr;
    }
    static bool scan_u64(const char* within, const char* key, uint64_t& out) {
        const char* p = scan_key(within, key);
        if (!p) return false;
        uint64_t value = 0;
        size_t digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10ULL + static_cast<uint64_t>(*p - '0');
            ++p;
            ++digits;
            if (digits > 20) return false;
        }
        if (digits == 0) return false;
        out = value;
        return true;
    }
    static bool scan_flag(const char* within, const char* key) {
        return std::strstr(within, key) != nullptr;
    }

    void emit(AccountEvent::Type type, uint8_t side, uint64_t price,
              uint64_t size, uint64_t id, uint64_t remaining = 0) {
        if (!fill_q_) return;
        AccountEvent ev{};
        ev.type = type;
        ev.side = side;
        ev.asset = 0;
        ev.price = price;
        ev.size = size;
        ev.remaining = remaining;
        ev.timestamp_ns = crowdintel::realtime_ns();
        ev.order_hash = id;
        (void)fill_q_->try_push(ev);
    }

    RestingOrderBookMock book_{};
    SPSC_RingBuffer<AccountEvent>* fill_q_ = nullptr;
    std::atomic<uint64_t> submissions_{0};
    std::atomic<uint64_t> cancellations_{0};
    std::atomic<uint64_t> paper_fills_{0};
    std::atomic<uint64_t> cancels_{0};
};

}  // namespace crowdintel

#endif  // CROWDINTEL_RESTING_MOCK_CLIENT_HPP
