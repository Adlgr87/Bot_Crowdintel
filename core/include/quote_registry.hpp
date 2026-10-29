#ifndef CROWDINTEL_QUOTE_REGISTRY_HPP
#define CROWDINTEL_QUOTE_REGISTRY_HPP

// ─────────────────────────────────────────────────────────────────────────────
// quote_registry.hpp — resting-quote bookkeeping for the maker sleeve.
//
// WHY THIS EXISTS
// A GTC order does not end when it is sent: it lives on the venue until it is
// filled or cancelled. Without cancel/replace a maker keeps advertising a stale
// price — and stale prices are exactly what informed flow takes. The measured
// cost of that mistake is the gap between the unconditional adverse move
// (0.092c median, docs/EXPERIMENT_MOTOR_C.md) and the conditional mark-out on a
// fill, which can be an order of magnitude larger.
//
// This is the decision layer only: given the quotes the venue has confirmed and
// the quotes we now want, it answers Keep / Cancel / Replace per side. It owns
// no sockets and no clock, so every rule below is deterministic and testable.
//
// EVENT SOURCES (already wired in the repo)
//   AccountEvent::Type::OPEN   -> on_open      (the venue accepted a rest)
//   AccountEvent::Type::FILL   -> on_fill      (partial fills arrive repeatedly)
//   AccountEvent::Type::CANCEL -> on_cancel
//   AccountEvent::Type::REJECT -> on_reject
//
// INVARIANTS
//   * a quote is `live` only after the venue confirms OPEN; we never assume a
//     send became a rest (the same fail-closed rule the rest of the engine uses)
//   * size is reduced by fills, never increased: a partial fill leaves a smaller
//     rest, and pretending otherwise would double-count inventory
//   * at most one live quote per side; the newest OPEN supersedes the older one
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>

namespace crowdintel {

inline constexpr size_t MAX_LIVE_QUOTES = 8;
inline constexpr uint8_t QUOTE_SIDE_BID = 0;
inline constexpr uint8_t QUOTE_SIDE_ASK = 1;

struct LiveQuote {
    uint64_t id = 0;        // venue order id (hashed)
    uint8_t side = 0;       // QUOTE_SIDE_BID / QUOTE_SIDE_ASK
    bool live = false;
    uint64_t price = 0;     // 1e6 units
    uint64_t size = 0;      // 1e6 units, remaining
    uint64_t placed_ns = 0;
};

enum class QuoteAction : uint8_t {
    Keep = 0,     // the resting quote still matches the target
    Cancel,       // the side is no longer wanted
    Replace       // cancel and re-place at the new target
};

inline const char* quote_action_name(QuoteAction a) noexcept {
    switch (a) {
        case QuoteAction::Keep:    return "keep";
        case QuoteAction::Cancel:  return "cancel";
        case QuoteAction::Replace: return "replace";
    }
    return "unknown";
}

struct ReplacePolicy {
    // Reprice once the target drifts this many ticks from the resting price.
    // 0 means "reprice on any change", which on a fast book means churn.
    uint64_t replace_drift_ticks = 1;
    // Maximum age before a resting quote is refreshed. A quote that outlives the
    // information behind it is the adverse-selection trap this file prevents.
    uint64_t max_rest_ns = 30'000'000'000ULL;  // 30 s
    uint64_t tick_size = 10000;                // 1e6 units
    // A size change smaller than this is ignored, to avoid churn on inventory
    // rounding.
    uint64_t size_tolerance = 1000000ULL;  // 1 share
};

class QuoteRegistry {
public:
    // Venue confirmed the order is resting. Returns false if the registry is
    // full, in which case the caller must cancel before adding.
    bool on_open(uint64_t id, uint8_t side, uint64_t price, uint64_t size,
                 uint64_t now_ns) noexcept {
        if (id == 0 || size == 0) return false;
        if (side != QUOTE_SIDE_BID && side != QUOTE_SIDE_ASK) return false;

        // A new OPEN on the same side supersedes the previous rest there; the
        // venue will have cancelled the old one or it was already gone.
        for (size_t i = 0; i < MAX_LIVE_QUOTES; ++i) {
            if (quotes_[i].live && quotes_[i].side == side) {
                quotes_[i].live = false;
                if (count_) --count_;  // superseding frees the slot
            }
        }

        for (size_t i = 0; i < MAX_LIVE_QUOTES; ++i) {
            if (!quotes_[i].live) {
                quotes_[i] = LiveQuote{};
                quotes_[i].id = id;
                quotes_[i].side = side;
                quotes_[i].price = price;
                quotes_[i].size = size;
                quotes_[i].placed_ns = now_ns;
                quotes_[i].live = true;
                ++count_;
                return true;
            }
        }
        return false;
    }

    // Partial fills arrive repeatedly; the rest shrinks and disappears at zero.
    void on_fill(uint64_t id, uint64_t fill_size) noexcept {
        for (size_t i = 0; i < MAX_LIVE_QUOTES; ++i) {
            if (!quotes_[i].live || quotes_[i].id != id) continue;
            if (fill_size >= quotes_[i].size) {
                quotes_[i].live = false;
                if (count_) --count_;
            } else {
                quotes_[i].size -= fill_size;
            }
            return;
        }
    }

    void on_cancel(uint64_t id) noexcept {
        for (size_t i = 0; i < MAX_LIVE_QUOTES; ++i) {
            if (quotes_[i].live && quotes_[i].id == id) {
                quotes_[i].live = false;
                if (count_) --count_;
                return;
            }
        }
    }

    // A rejected order never rested: nothing to remove, nothing to assume.
    void on_reject(uint64_t id) noexcept {
        for (size_t i = 0; i < MAX_LIVE_QUOTES; ++i) {
            if (quotes_[i].live && quotes_[i].id == id) {
                quotes_[i].live = false;
                if (count_) --count_;
                return;
            }
        }
    }

    // Keep / Cancel / Replace for one side.
    //   desired_price == 0  -> the side is not wanted (Cancel, or Keep if absent)
    QuoteAction action_for(uint8_t side, uint64_t desired_price,
                           uint64_t desired_size, uint64_t now_ns,
                           const ReplacePolicy& policy) const noexcept {
        const LiveQuote* live = find(side);

        if (desired_price == 0 || desired_size == 0) {
            return live ? QuoteAction::Cancel : QuoteAction::Keep;
        }
        if (!live) return QuoteAction::Replace;  // nothing resting: place

        // Age: a quote older than the policy window carries stale information.
        if (policy.max_rest_ns != 0 && now_ns > live->placed_ns &&
            (now_ns - live->placed_ns) >= policy.max_rest_ns)
            return QuoteAction::Replace;

        // Price drift, in ticks.
        const uint64_t drift = live->price > desired_price
                                   ? live->price - desired_price
                                   : desired_price - live->price;
        const uint64_t threshold =
            policy.replace_drift_ticks * (policy.tick_size ? policy.tick_size : 1);
        if (drift >= threshold) return QuoteAction::Replace;

        // Size drift: inventory skew changed how much we want to show.
        const uint64_t size_drift = live->size > desired_size
                                        ? live->size - desired_size
                                        : desired_size - live->size;
        if (size_drift > policy.size_tolerance) return QuoteAction::Replace;

        return QuoteAction::Keep;
    }

    const LiveQuote* find(uint8_t side) const noexcept {
        for (size_t i = 0; i < MAX_LIVE_QUOTES; ++i) {
            if (quotes_[i].live && quotes_[i].side == side) return &quotes_[i];
        }
        return nullptr;
    }

    size_t live_count() const noexcept { return count_; }

    void clear() noexcept {
        for (size_t i = 0; i < MAX_LIVE_QUOTES; ++i) quotes_[i] = LiveQuote{};
        count_ = 0;
    }

private:
    LiveQuote quotes_[MAX_LIVE_QUOTES]{};
    size_t count_ = 0;
};

}  // namespace crowdintel

#endif  // CROWDINTEL_QUOTE_REGISTRY_HPP
