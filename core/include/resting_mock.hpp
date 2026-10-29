#ifndef CROWDINTEL_RESTING_MOCK_HPP
#define CROWDINTEL_RESTING_MOCK_HPP

// ─────────────────────────────────────────────────────────────────────────────
// resting_mock.hpp — a transport that models RESTING orders.
//
// WHY THIS EXISTS
// MockCLOBClient synthesizes a fill on every accepted submit. That is correct
// for a taker and fatal for a maker: a resting quote never rests, so it is never
// exposed to the one risk the maker sleeve is trying to measure. A canary run on
// that transport would report a beautiful fill rate and zero adverse selection,
// and mean nothing at all.
//
// This models the behaviour that matters: an order rests, and it fills ONLY when
// the market price crosses it. That single rule is what makes adverse selection
// observable — a maker is filled precisely when the price moves against them,
// which is exactly what the measured 0.092c UNCONDITIONAL figure in
// docs/EXPERIMENT_MOTOR_C.md does NOT capture.
//
// Deliberately not a simulator: no queue position, no latency, no partial-fill
// priority beyond available size. It reproduces the one mechanism without which
// the measurement is meaningless, and no more. Anything richer would be a
// backtester, which the repo already has (tests/replay/l2_backtester).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>

namespace crowdintel {

inline constexpr size_t MOCK_MAX_RESTS = 8;
inline constexpr uint8_t MOCK_SIDE_BID = 0;
inline constexpr uint8_t MOCK_SIDE_ASK = 1;

enum class MockRestEvent : uint8_t {
    None = 0,   // nothing happened
    Opened,     // the order is now resting on the book
    Filled,     // the market crossed the resting price
    Cancelled,  // the rest was withdrawn
    Rejected    // invalid id, side, price or size
};

inline const char* mock_rest_event_name(MockRestEvent e) noexcept {
    switch (e) {
        case MockRestEvent::None:      return "none";
        case MockRestEvent::Opened:    return "opened";
        case MockRestEvent::Filled:    return "filled";
        case MockRestEvent::Cancelled: return "cancelled";
        case MockRestEvent::Rejected:  return "rejected";
    }
    return "unknown";
}

struct MockRestOutcome {
    MockRestEvent event = MockRestEvent::None;
    uint64_t id = 0;
    uint64_t filled_size = 0;   // 1e6 scale, for Filled
    uint64_t fill_price = 0;    // the RESTING price: a maker is paid its quote
    uint8_t side = 0;
};

class RestingOrderBookMock {
public:
    // A resting order is accepted, never filled. This is the whole difference
    // from MockCLOBClient.
    MockRestOutcome place(uint64_t id, uint8_t side, uint64_t price,
                          uint64_t size) noexcept {
        MockRestOutcome out;
        out.id = id;
        out.side = side;
        if (id == 0 || size == 0 || price == 0 || price >= 1000000u ||
            (side != MOCK_SIDE_BID && side != MOCK_SIDE_ASK)) {
            out.event = MockRestEvent::Rejected;
            return out;
        }
        // One live rest per side: a replace supersedes, it never stacks.
        for (size_t i = 0; i < MOCK_MAX_RESTS; ++i) {
            if (rests_[i].live && rests_[i].side == side) rests_[i].live = false;
        }
        for (size_t i = 0; i < MOCK_MAX_RESTS; ++i) {
            if (!rests_[i].live) {
                rests_[i].live = true;
                rests_[i].id = id;
                rests_[i].side = side;
                rests_[i].price = price;
                rests_[i].size = size;
                ++opens_;
                out.event = MockRestEvent::Opened;
                return out;
            }
        }
        out.event = MockRestEvent::Rejected;  // no capacity
        return out;
    }

    // Advance the book. Fills the first rest the market has crossed and returns
    // one outcome per call, so the caller drains in a loop.
    //
    //   a resting BID fills when the market's ask reaches down to it
    //   a resting ASK fills when the market's bid reaches up to it
    //
    // The fill price is the RESTING price: a maker is paid what it quoted. The
    // size is capped by the liquidity that crossed.
    MockRestOutcome observe_book(uint64_t bid, uint64_t bid_size, uint64_t ask,
                                 uint64_t ask_size) noexcept {
        MockRestOutcome out;
        if (bid == 0 || ask == 0 || bid >= ask) return out;  // invalid book: no fill

        for (size_t i = 0; i < MOCK_MAX_RESTS; ++i) {
            if (!rests_[i].live) continue;

            const bool is_bid = rests_[i].side == MOCK_SIDE_BID;
            const bool crossed = is_bid ? (rests_[i].price >= ask)
                                        : (rests_[i].price <= bid);
            if (!crossed) continue;

            const uint64_t available = is_bid ? ask_size : bid_size;
            if (available == 0) continue;

            const uint64_t take =
                available < rests_[i].size ? available : rests_[i].size;
            out.event = MockRestEvent::Filled;
            out.id = rests_[i].id;
            out.side = rests_[i].side;
            out.filled_size = take;
            out.fill_price = rests_[i].price;
            filled_shares_ += take;
            ++fills_;

            if (take >= rests_[i].size) {
                rests_[i].live = false;
            } else {
                rests_[i].size -= take;
            }
            return out;
        }
        return out;  // event stays None
    }

    MockRestOutcome cancel(uint64_t id) noexcept {
        MockRestOutcome out;
        out.id = id;
        for (size_t i = 0; i < MOCK_MAX_RESTS; ++i) {
            if (rests_[i].live && rests_[i].id == id) {
                rests_[i].live = false;
                ++cancels_;
                out.event = MockRestEvent::Cancelled;
                out.side = rests_[i].side;
                return out;
            }
        }
        return out;  // cancelling an absent id is a no-op, not an error
    }

    size_t live_count() const noexcept {
        size_t n = 0;
        for (size_t i = 0; i < MOCK_MAX_RESTS; ++i)
            if (rests_[i].live) ++n;
        return n;
    }

    const uint64_t* rest_size(uint8_t side) const noexcept {
        for (size_t i = 0; i < MOCK_MAX_RESTS; ++i)
            if (rests_[i].live && rests_[i].side == side) return &rests_[i].size;
        return nullptr;
    }

    uint64_t opens() const noexcept { return opens_; }
    uint64_t fills() const noexcept { return fills_; }
    uint64_t cancels() const noexcept { return cancels_; }
    uint64_t filled_shares() const noexcept { return filled_shares_; }

private:
    struct Rest {
        uint64_t id = 0;
        uint64_t price = 0;
        uint64_t size = 0;
        uint8_t side = 0;
        bool live = false;
    };
    Rest rests_[MOCK_MAX_RESTS]{};
    uint64_t opens_ = 0;
    uint64_t fills_ = 0;
    uint64_t cancels_ = 0;
    uint64_t filled_shares_ = 0;
};

}  // namespace crowdintel

#endif  // CROWDINTEL_RESTING_MOCK_HPP
