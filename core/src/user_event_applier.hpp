#ifndef USER_EVENT_APPLIER_HPP
#define USER_EVENT_APPLIER_HPP

// Phase 3 — turns private-channel events into ledger transitions.
//
// Attribution rules (no guessing):
//   * an order event is applied only when its venue order id is already in the
//     journal; anything else is counted as unattributed and blocks trading
//     until the REST reconciliation of Phase 5 attributes it;
//   * a trade event is applied to our taker order when `owner` is our API key,
//     and to each maker order whose `owner` is ours;
//   * trade increments are deduplicated by (order id, trade id): MATCHED,
//     MINED and CONFIRMED are three updates of the SAME trade, not three fills.
//     If the dedupe table overflows, increments stop and the divergence blocks
//     trading rather than risking a double count;
//   * OrderEvent.size_matched is cumulative, so it is only ever raised, never
//     lowered: a regression is a divergence, never a silent correction.
//
// The applier never invents fills: if an event does not match its journal, the
// outcome is "needs reconciliation", not an assumption.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "market_config.hpp"
#include "order_ledger.hpp"
#include "user_ws_client.hpp"

namespace user_apply {

enum class ApplyResult : uint8_t {
    kApplied = 0,
    kNoChange = 1,      // idempotent replay or informative-only event
    kUnattributed = 2,  // venue id unknown to the journal
    kDivergence = 3,    // ambiguous state: reconcile before trading
    kIllegal = 4,       // contradicts a terminal state: quarantine
};

inline const char* apply_result_name(ApplyResult result) noexcept {
    switch (result) {
        case ApplyResult::kApplied: return "applied";
        case ApplyResult::kNoChange: return "no_change";
        case ApplyResult::kUnattributed: return "unattributed";
        case ApplyResult::kDivergence: return "divergence";
        case ApplyResult::kIllegal: return "illegal";
    }
    return "invalid";
}

struct Stats {
    uint64_t applied = 0;
    uint64_t no_change = 0;
    uint64_t unattributed = 0;
    uint64_t divergences = 0;
    uint64_t illegal = 0;
    uint64_t maker_fills = 0;
    uint64_t trade_dedupe_hits = 0;
    uint64_t dedupe_overflow = 0;
    uint64_t retrying_seen = 0;
    uint64_t last_timestamp_ms = 0;
};

class UserEventApplier {
public:
    static constexpr size_t kDedupeSlots = 512;
    static constexpr size_t kDedupeMask = kDedupeSlots - 1;

    struct LedgerResultHolder {
        cledger::LedgerEvent event = cledger::LedgerEvent::kTradeUpdate;
        cledger::Transition transition{};
    };

    struct Entry {
        uint64_t h1 = 0;
        uint64_t h2 = 0;
    };

    explicit UserEventApplier(const MarketConfig& cfg) : cfg_(cfg) {}

    void attach_ledger(cledger::OrderLedger* ledger) noexcept { ledger_ = ledger; }
    bool configured() const noexcept {
        return ledger_ != nullptr && cfg_.owner_api_key[0] != '\0';
    }

    ApplyResult apply(const UserEvent& event, char* error,
                      size_t error_cap) noexcept {
        if (!ledger_) {
            set_error(error, error_cap, "no ledger attached");
            ++stats_.divergences;
            return ApplyResult::kDivergence;
        }
        stats_.last_timestamp_ms = event.timestamp_ms;
        return event.kind == UserEventKind::kOrder
                   ? apply_order(event, error, error_cap)
                   : apply_trade(event, error, error_cap);
    }

    const Stats& stats() const noexcept { return stats_; }

    // Trading gate contribution: any divergence keeps the bot out of the
    // market until a human (or the Phase 5 reconciliation) clears it.
    bool clean() const noexcept {
        return stats_.unattributed == 0 && stats_.divergences == 0 &&
               stats_.illegal == 0 && stats_.dedupe_overflow == 0;
    }

    // Only REST reconciliation may call this: it must have re-read the venue's
    // open orders and trades and proven the journal's totals.
    void reset_after_reconciliation() noexcept {
        stats_ = Stats{};
        for (Entry& entry : table_) entry = Entry{};
        used_ = 0;
    }

private:
    static void set_error(char* error, size_t cap, const char* text) noexcept {
        if (error && cap) std::snprintf(error, cap, "%s", text);
    }

    static void hash_pair(const char* a, const char* b, uint64_t& h1,
                          uint64_t& h2) noexcept {
        uint64_t first = 1469598103934665603ull;  // FNV-1a 64
        uint64_t second = 0x9E3779B97F4A7C15ull;  // splitmix-style accumulator
        auto mix = [&](const char* text) {
            for (const char* p = text; *p; ++p) {
                first ^= static_cast<uint8_t>(*p);
                first *= 1099511628211ull;
                second ^= static_cast<uint64_t>(static_cast<uint8_t>(*p)) + 0x9E37u;
                second *= 0xBF58476D1CE4E5B9ull;
                second ^= second >> 29;
            }
        };
        mix(a);
        mix("|");
        mix(b);
        h1 = first;
        h2 = second;
    }

    enum class Seen : uint8_t { kFirst = 0, kRepeat = 1, kOverflow = 2 };

    Seen remember_trade(const char* order_id, const char* trade_id) noexcept {
        uint64_t h1 = 0, h2 = 0;
        hash_pair(order_id, trade_id, h1, h2);
        size_t slot = static_cast<size_t>(h1) & kDedupeMask;
        for (size_t probe = 0; probe < kDedupeSlots; ++probe) {
            Entry& entry = table_[(slot + probe) & kDedupeMask];
            if (entry.h1 == h1 && entry.h2 == h2) return Seen::kRepeat;
            if (entry.h1 == 0 && entry.h2 == 0) {
                entry.h1 = h1 ? h1 : 1;
                entry.h2 = h2 ? h2 : 1;
                ++used_;
                return Seen::kFirst;
            }
        }
        ++stats_.dedupe_overflow;
        return Seen::kOverflow;
    }

    // Applies one cumulative fill value for an order, refusing to lower it.
    ApplyResult apply_fill(const char* client_order_id, uint64_t filled_fixed6,
                           LedgerResultHolder& holder, char* error,
                           size_t error_cap) noexcept {
        const cledger::OrderSummary* summary = ledger_->find(client_order_id);
        if (!summary) {
            set_error(error, error_cap, "client order id unknown");
            ++stats_.unattributed;
            return ApplyResult::kUnattributed;
        }
        if (filled_fixed6 < summary->filled_fixed6) {
            set_error(error, error_cap, "cumulative fill regressed");
            ++stats_.divergences;
            return ApplyResult::kDivergence;
        }
        holder.transition = ledger_->record_transition(
            client_order_id, holder.event, cledger::Evidence::kVenueChannel, "",
            filled_fixed6, error, error_cap);
        return classify(holder.transition);
    }

    ApplyResult classify(const cledger::Transition& step) noexcept {
        switch (step.result) {
            case cledger::TransitionResult::kApplied:
                ++stats_.applied;
                return ApplyResult::kApplied;
            case cledger::TransitionResult::kUnchanged:
                ++stats_.no_change;
                return ApplyResult::kNoChange;
            case cledger::TransitionResult::kNeedsReconcile:
                ++stats_.divergences;
                return ApplyResult::kDivergence;
            case cledger::TransitionResult::kIllegal:
                ++stats_.illegal;
                return ApplyResult::kIllegal;
        }
        ++stats_.divergences;
        return ApplyResult::kDivergence;
    }

    ApplyResult apply_order(const UserEvent& event, char* error,
                            size_t error_cap) noexcept {
        const cledger::OrderSummary* summary =
            ledger_->find_by_venue(event.venue_order_id);
        if (!summary) {
            set_error(error, error_cap, "order event for an unknown venue order");
            ++stats_.unattributed;
            return ApplyResult::kUnattributed;
        }
        char client_order_id[cledger::kClientOrderIdChars];
        std::snprintf(client_order_id, sizeof(client_order_id), "%s",
                      summary->client_order_id);

        if (event.order_status == UserOrderStatus::kUnknown) {
            set_error(error, error_cap, "unknown order status");
            ++stats_.divergences;
            return ApplyResult::kDivergence;
        }
        if (event.size_matched_fixed6 > event.original_fixed6) {
            set_error(error, error_cap, "matched size exceeds original size");
            ++stats_.divergences;
            return ApplyResult::kDivergence;
        }

        LedgerResultHolder holder;
        if (event.order_type == UserOrderType::kCancellation ||
            event.order_status == UserOrderStatus::kCanceled) {
            holder.event = cledger::LedgerEvent::kOrderCanceled;
        } else if (event.order_status == UserOrderStatus::kLive) {
            if (event.size_matched_fixed6 == 0) {
                holder.event = cledger::LedgerEvent::kOrderLive;
            } else {
                holder.event = cledger::LedgerEvent::kTradeUpdate;
            }
        } else {  // MATCHED
            holder.event = cledger::LedgerEvent::kTradeUpdate;
        }

        if (holder.event == cledger::LedgerEvent::kOrderLive)
            return classify(ledger_->record_transition(
                client_order_id, holder.event, cledger::Evidence::kVenueChannel,
                "", 0, error, error_cap));

        if (holder.event == cledger::LedgerEvent::kOrderCanceled)
            return classify(ledger_->record_transition(
                client_order_id, holder.event, cledger::Evidence::kVenueChannel,
                "", event.size_matched_fixed6, error, error_cap));

        return apply_fill(client_order_id, event.size_matched_fixed6, holder,
                          error, error_cap);
    }

    ApplyResult apply_trade(const UserEvent& event, char* error,
                            size_t error_cap) noexcept {
        // kApplied is the floor: any other outcome is "worse" and wins.
        ApplyResult worst = ApplyResult::kApplied;
        bool attributed = false;

        const bool taker_is_ours = cfg_.owner_api_key[0] &&
                                   std::strcmp(event.owner, cfg_.owner_api_key) == 0;
        if (taker_is_ours) {
            attributed = true;
            const ApplyResult result = apply_trade_increment(
                event.venue_order_id, event.trade_id, event.trade_size_fixed6,
                event.trade_status, error, error_cap);
            worst = worse_of(worst, result);
        }

        for (size_t i = 0; i < event.maker_count; ++i) {
            const UserEvent::MakerFill& fill = event.makers[i];
            if (!cfg_.owner_api_key[0] ||
                std::strcmp(fill.owner, cfg_.owner_api_key) != 0)
                continue;
            attributed = true;
            const ApplyResult result = apply_trade_increment(
                fill.order_id, event.trade_id, fill.matched_fixed6,
                event.trade_status, error, error_cap);
            if (result == ApplyResult::kApplied) ++stats_.maker_fills;
            worst = worse_of(worst, result);
        }

        if (!attributed) {
            // The subscription is per API key, so an event that names neither
            // our key as taker nor one of our maker orders is foreign data.
            set_error(error, error_cap, "trade not attributable to this account");
            ++stats_.unattributed;
            return worse_of(worst, ApplyResult::kUnattributed);
        }
        return worst;
    }

    ApplyResult apply_trade_increment(const char* order_id, const char* trade_id,
                                      uint64_t increment_fixed6,
                                      UserTradeStatus status, char* error,
                                      size_t error_cap) noexcept {
        const cledger::OrderSummary* summary = ledger_->find_by_venue(order_id);
        if (!summary) {
            set_error(error, error_cap, "trade for an unknown venue order");
            ++stats_.unattributed;
            return ApplyResult::kUnattributed;
        }
        char client_order_id[cledger::kClientOrderIdChars];
        std::snprintf(client_order_id, sizeof(client_order_id), "%s",
                      summary->client_order_id);
        LedgerResultHolder holder;

        switch (status) {
            case UserTradeStatus::kMined:
            case UserTradeStatus::kConfirmed: {
                const Seen seen = remember_trade(order_id, trade_id);
                if (seen == Seen::kRepeat) {
                    ++stats_.trade_dedupe_hits;
                    return ApplyResult::kNoChange;  // same trade, later status
                }
                if (seen == Seen::kOverflow) {
                    set_error(error, error_cap,
                              "trade dedupe table full: reconcile before trading");
                    ++stats_.divergences;
                    return ApplyResult::kDivergence;
                }
                // First sighting of a settled trade: we never saw MATCHED, so
                // the journal cannot prove the fill total.
                set_error(error, error_cap,
                          "settled trade without a matched increment");
                ++stats_.divergences;
                return ApplyResult::kDivergence;
            }
            case UserTradeStatus::kRetrying:
                if (remember_trade(order_id, trade_id) == Seen::kOverflow) {
                    set_error(error, error_cap, "trade dedupe table full");
                    ++stats_.divergences;
                    return ApplyResult::kDivergence;
                }
                ++stats_.retrying_seen;
                return ApplyResult::kNoChange;
            case UserTradeStatus::kFailed: {
                // A failed trade invalidates the increments already applied.
                holder.event = cledger::LedgerEvent::kTradeFailed;
                const ApplyResult result = classify(ledger_->record_transition(
                    client_order_id, holder.event, cledger::Evidence::kVenueChannel,
                    "", 0, error, error_cap));
                if (result != ApplyResult::kApplied)
                    return worse_of(result, ApplyResult::kDivergence);
                ++stats_.divergences;
                return ApplyResult::kDivergence;
            }
            case UserTradeStatus::kMatched:
                break;
            case UserTradeStatus::kUnknown:
                set_error(error, error_cap, "unknown trade status");
                ++stats_.divergences;
                return ApplyResult::kDivergence;
        }

        const Seen seen = remember_trade(order_id, trade_id);
        if (seen == Seen::kRepeat) {
            ++stats_.trade_dedupe_hits;
            return ApplyResult::kNoChange;
        }
        if (seen == Seen::kOverflow) {
            set_error(error, error_cap,
                      "trade dedupe table full: reconcile before trading");
            ++stats_.divergences;
            return ApplyResult::kDivergence;
        }
        holder.event = cledger::LedgerEvent::kTradeUpdate;
        return apply_fill(client_order_id,
                          summary->filled_fixed6 + increment_fixed6, holder,
                          error, error_cap);
    }

    static ApplyResult worse_of(ApplyResult a, ApplyResult b) noexcept {
        return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
    }

    const MarketConfig& cfg_;
    cledger::OrderLedger* ledger_ = nullptr;
    Stats stats_{};
    size_t used_ = 0;
    Entry table_[kDedupeSlots]{};
};

}  // namespace user_apply

#endif  // USER_EVENT_APPLIER_HPP
