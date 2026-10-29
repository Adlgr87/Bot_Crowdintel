#ifndef CROWDINTEL_MAKER_LOOP_HPP
#define CROWDINTEL_MAKER_LOOP_HPP

// ─────────────────────────────────────────────────────────────────────────────
// maker_loop.hpp — the Motor C quote loop: the piece that makes the sleeve run.
//
// Composes the three decision cores that already exist and were tested
// separately, without adding a fourth opinion about how to trade:
//
//   maker_quote.hpp      -> WHERE to quote (cushion, reward band, skew)
//   quote_registry.hpp   -> WHEN to pull (drift, age, size)
//   this file            -> ties them together once per tick
//
// It deliberately does NOT sign, send, or touch the network. It emits a
// MakerDecision naming what to cancel and what to place; the engine executes it
// through its existing egress, because signing belongs in one place.
//
// ORDER OF OPERATIONS PER TICK: cancel first, then place. A replace that placed
// before cancelling could momentarily double the exposure on that side, which is
// exactly what the inventory caps exist to prevent.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>

#include "../include/maker_quote.hpp"
#include "../include/quote_registry.hpp"

namespace crowdintel {

struct MakerTickInput {
    uint64_t best_bid = 0;
    uint64_t bid_size = 0;
    uint64_t best_ask = 0;
    uint64_t ask_size = 0;
    uint64_t fair_value = 0;      // model probability, 1e6
    uint64_t tick_size = 10000;   // market property, from the venue metadata
    int64_t inventory_shares = 0;  // signed, 1e6
    uint64_t now_ns = 0;
};

struct MakerDecision {
    QuotePlan quotes{};            // status != Placed means "stop quoting"
    QuoteAction bid_action = QuoteAction::Keep;
    QuoteAction ask_action = QuoteAction::Keep;
    bool place_bid = false;
    bool place_ask = false;
    uint64_t bid_price = 0;
    uint64_t ask_price = 0;
    uint64_t bid_size = 0;
    uint64_t ask_size = 0;
    // Resting ids to cancel first; 0 means nothing to cancel on that side.
    uint64_t cancel_bid_id = 0;
    uint64_t cancel_ask_id = 0;
};

class MakerLoop {
public:
    MakerLoop(const QuoteConfig& quote_cfg, const ReplacePolicy& replace_cfg)
        : quote_cfg_(quote_cfg), replace_cfg_(replace_cfg) {}

    MakerDecision on_tick(const MakerTickInput& in) {
        MakerDecision d;
        QuoteInput qi;
        qi.best_bid = in.best_bid;
        qi.best_ask = in.best_ask;
        qi.fair_value = in.fair_value;
        qi.tick_size = in.tick_size;
        qi.inventory_shares = in.inventory_shares;
        d.quotes = plan_quotes(qi, quote_cfg_);

        const LiveQuote* live_bid = registry_.find(QUOTE_SIDE_BID);
        const LiveQuote* live_ask = registry_.find(QUOTE_SIDE_ASK);

        // The placement engine refused (tight book, band too narrow, no size,
        // inventory full): withdraw whatever rests. Resting a quote the model
        // no longer wants is how a maker becomes exit liquidity.
        if (d.quotes.status != QuoteStatus::Placed) {
            if (live_bid) {
                d.cancel_bid_id = live_bid->id;
                d.bid_action = QuoteAction::Cancel;
            }
            if (live_ask) {
                d.cancel_ask_id = live_ask->id;
                d.ask_action = QuoteAction::Cancel;
            }
            ++skips_;
            return d;
        }

        d.bid_price = d.quotes.bid_price;
        d.ask_price = d.quotes.ask_price;
        d.bid_size = d.quotes.bid_size;
        d.ask_size = d.quotes.ask_size;

        d.bid_action = registry_.action_for(QUOTE_SIDE_BID, d.bid_price,
                                            d.bid_size, in.now_ns,
                                            replace_cfg_);
        d.ask_action = registry_.action_for(QUOTE_SIDE_ASK, d.ask_price,
                                            d.ask_size, in.now_ns,
                                            replace_cfg_);

        if (d.bid_action == QuoteAction::Replace) {
            d.place_bid = d.bid_size != 0;
            d.cancel_bid_id = live_bid ? live_bid->id : 0;
            ++places_;
        } else if (d.bid_action == QuoteAction::Cancel) {
            d.cancel_bid_id = live_bid ? live_bid->id : 0;
            ++cancels_;
        } else {
            ++keeps_;
        }

        if (d.ask_action == QuoteAction::Replace) {
            d.place_ask = d.ask_size != 0;
            d.cancel_ask_id = live_ask ? live_ask->id : 0;
            ++places_;
        } else if (d.ask_action == QuoteAction::Cancel) {
            d.cancel_ask_id = live_ask ? live_ask->id : 0;
            ++cancels_;
        } else {
            ++keeps_;
        }
        return d;
    }

    // Venue confirmations.
    void on_open(uint64_t id, uint8_t side, uint64_t price, uint64_t size,
                 uint64_t now_ns) {
        (void)registry_.on_open(id, side, price, size, now_ns);
    }
    void on_fill(uint64_t id, uint64_t size) { registry_.on_fill(id, size); }
    void on_cancel(uint64_t id) { registry_.on_cancel(id); }
    void on_reject(uint64_t id) { registry_.on_reject(id); }

    size_t resting() const { return registry_.live_count(); }
    const QuoteRegistry& registry() const { return registry_; }

    uint64_t places() const { return places_; }
    uint64_t cancels() const { return cancels_; }
    uint64_t keeps() const { return keeps_; }
    uint64_t skips() const { return skips_; }

private:
    QuoteConfig quote_cfg_{};
    ReplacePolicy replace_cfg_{};
    QuoteRegistry registry_{};
    uint64_t places_ = 0;
    uint64_t cancels_ = 0;
    uint64_t keeps_ = 0;
    uint64_t skips_ = 0;
};

}  // namespace crowdintel

#endif  // CROWDINTEL_MAKER_LOOP_HPP
