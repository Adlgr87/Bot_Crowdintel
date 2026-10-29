#ifndef CROWDINTEL_MAKER_ENGINE_HPP
#define CROWDINTEL_MAKER_ENGINE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// maker_engine.hpp — the maker tick path, wired end to end.
//
// This is the Motor C equivalent of run_engine(): one function that reads the
// book, decides, executes and reconciles the venue's answers. It exists because
// the maker sleeve is NOT the taker signal pipeline — it has no AlphaSignal, no
// Kelly sizing and no entry signal. Forcing it through ExecutionEngine would
// have meant adding a second personality to a 1000-line class.
//
// SAFETY IS NOT OPTIONAL HERE. A maker path that skipped the brakes would be a
// quiet way around everything Fase 0 built, so the loop enforces the same two
// things the taker path does before any signature exists:
//   * a latched kill stops quoting and withdraws both rests
//   * a BUY placement must pass RiskManager::authorize (exposure caps)
// A SELL is never authorized here: it reduces inventory, and blocking an exit
// is how a risk system turns a small loss into a large one.
//
// AUTHENTICITY: orders are signed inline (the pre-signed ladder assumes
// immediate consumption on a TTL; a resting order's life is cancel/replace).
// ─────────────────────────────────────────────────────────────────────────────

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../crypto/fast_random.hpp"
#include "../include/account_events.hpp"
#include "../include/order_book.hpp"
#include "../include/risk_manager.hpp"
#include "../include/time_utils.hpp"
#include "execution_engine.hpp"
#include "maker_loop.hpp"
#include "maker_runner.hpp"
#include "market_config.hpp"
#include "metrics.hpp"
#include "polymarket_order.hpp"
#include "presigned_pool.hpp"

namespace crowdintel {

inline QuoteConfig quote_config_from(const MarketConfig& cfg) {
    QuoteConfig q;
    q.adverse_sel_bps = cfg.adverse_sel_bps;
    q.min_edge_bps = cfg.min_edge_bps;
    q.rewards_max_spread_bps = cfg.rewards_max_spread_bps;
    q.rewards_min_size_shares = cfg.rewards_min_size_shares;
    q.order_shares = cfg.quote_size_shares;
    q.max_inventory_shares = cfg.max_quote_inventory_shares;
    return q;
}

inline ReplacePolicy replace_policy_from(const MarketConfig& cfg) {
    ReplacePolicy p;
    p.replace_drift_ticks = cfg.quote_replace_ticks;
    p.max_rest_ns = cfg.quote_max_age_ms * 1000000ULL;
    p.tick_size = cfg.tick_size;
    p.size_tolerance = cfg.quote_size_tolerance;
    return p;
}

struct MakerRunStats {
    long ticks = 0;
    long places = 0;
    long cancels = 0;
    long rejected = 0;   // placement refused by risk or the venue
    long skipped = 0;    // the book could not justify a quote
};

// Client must provide: submit(WireBody) -> SubmitResult{ok}, cancel_order(const
// char*), rest_get, cancel_all, submissions. Runs until g_running goes false or
// max_ticks productive ticks elapse.
template <typename Client>
MakerRunStats run_maker_engine(Client& client, const MarketConfig& cfg,
                               const EIP712Signer& signer, OrderBookL2& book,
                               SPSC_RingBuffer<AccountEvent>& account_q,
                               RiskManager& risk, std::atomic<bool>& running,
                               crowdintel::Metrics* metrics,
                               MakerLoop* loop_out = nullptr) {
    MakerLoop loop(quote_config_from(cfg), replace_policy_from(cfg));
    FastRandom rng;  // OS-seeded ChaCha20; salts must not be guessable
    MakerRunStats stats{};
    const uint64_t max_age_ns = cfg.max_book_age_ms * 1000000ULL;

    while (running.load(std::memory_order_acquire)) {
        // ── 1. Drain the venue's answers first, so the decision below sees the
        //       true state: a fill that arrived just now changes the inventory
        //       and the resting size.
        AccountEvent ev{};
        while (account_q.try_pop(ev)) {
            const uint8_t qside = ev.side == K_SIDE_BUY ? QUOTE_SIDE_BID
                                                        : QUOTE_SIDE_ASK;
            switch (ev.type) {
                case AccountEvent::Type::OPEN:
                    loop.on_open(ev.order_hash, qside, ev.price, ev.size,
                                 crowdintel::mono_ns());
                    break;
                case AccountEvent::Type::FILL:
                    loop.on_fill(ev.order_hash, ev.size);
                    break;
                case AccountEvent::Type::CANCEL:
                    loop.on_cancel(ev.order_hash);
                    break;
                default:
                    loop.on_reject(ev.order_hash);
                    break;
            }
        }

        // ── 2. A latched kill withdraws everything and stops quoting.
        if (risk.killed()) {
            MakerDecision kill{};
            kill.quotes.status = QuoteStatus::InvalidBook;  // forces withdraw
            const LiveQuote* b = loop.registry().find(QUOTE_SIDE_BID);
            const LiveQuote* a = loop.registry().find(QUOTE_SIDE_ASK);
            if (b) kill.cancel_bid_id = b->id;
            if (a) kill.cancel_ask_id = a->id;
            auto cancel_hex = [&](uint64_t id) {
                char hex[32];
                std::snprintf(hex, sizeof(hex), "0x%llx",
                              static_cast<unsigned long long>(id));
                return client.cancel_order(hex);
            };
            (void)run_maker_decision(
                kill, [](uint8_t, uint64_t, uint64_t) { return false; },
                cancel_hex);
            ++stats.ticks;
            break;
        }

        // ── 3. Read the book. A stale or unusable book is not a reason to keep
        //       resting a quote: the price reference is gone.
        OrderBookL2::Top top{};
        if (!book.read_top(top, max_age_ns) || top.bid.size == 0 ||
            top.ask.size == 0 || top.bid.price >= top.ask.price) {
            ++stats.skipped;
            if (metrics) metrics->on_decision(
                static_cast<uint32_t>(TickResult::NO_BOOK));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // ── 4. Decide. Fair value is the book mid: this sleeve has no forecast
        //       by design, its income is carry plus spread.
        MakerTickInput in{};
        in.best_bid = top.bid.price;
        in.bid_size = top.bid.size;
        in.best_ask = top.ask.price;
        in.ask_size = top.ask.size;
        in.fair_value = (top.bid.price + top.ask.price) / 2u;
        in.tick_size = cfg.tick_size;
        in.inventory_shares = 0;  // no tracker on this path yet
        in.now_ns = crowdintel::mono_ns();

        const MakerDecision d = loop.on_tick(in);
        if (d.quotes.status != QuoteStatus::Placed) ++stats.skipped;

        auto cancel_hex = [&](uint64_t id) {
            char hex[32];
            std::snprintf(hex, sizeof(hex), "0x%llx",
                          static_cast<unsigned long long>(id));
            return client.cancel_order(hex);
        };

        auto place = [&](uint8_t side, uint64_t price, uint64_t size) -> bool {
            uint64_t maker_amt = 0, taker_amt = 0, eff = 0;
            const uint8_t k_side = side == 0 ? K_SIDE_BUY : K_SIDE_SELL;
            if (!compute_order_amounts(k_side, price, size, cfg.tick_size,
                                       /*market_order=*/false, maker_amt,
                                       taker_amt, eff))
                return false;
            if (eff < cfg.min_size_shares) return false;

            // A BUY grows exposure: it must pass the same authorization the
            // taker path uses. A SELL reduces it and is never blocked here.
            if (k_side == K_SIDE_BUY) {
                const double notional =
                    static_cast<double>(eff) * 1e-6 * static_cast<double>(price) *
                    1e-6;
                const double exposure = notional;
                if (!risk.authorize(k_side, notional, exposure, exposure))
                    return false;
            }

            OrderV2 order{};
            order.salt = rng.next_salt();
            order.timestamp_ms = PresignedOrderPool::now_ms();
            std::memcpy(order.maker, cfg.maker, 20);
            std::memcpy(order.signer, cfg.signer, 20);
            std::memcpy(order.token_id, cfg.token_id_be, 32);
            order.maker_amount = maker_amt;
            order.taker_amount = taker_amt;
            order.side = k_side;
            order.signature_type = cfg.signature_type;

            uint8_t signature[65];
            if (!signer.sign_order(order, signature)) return false;
            const uint64_t expiration =
                cfg.wire_expiration(order.timestamp_ms / 1000ULL);
            WireBody body{};
            if (!build_wire_body(order, signature, cfg.token_id_dec,
                                 cfg.maker_hex, cfg.signer_hex,
                                 cfg.owner_api_key, cfg.entry_order_type, body,
                                 expiration))
                return false;
            const auto result = client.submit(body);
            return result.ok;
        };

        const MakerRunSummary s =
            run_maker_decision(d, place, cancel_hex);
        stats.places += (s.placed_bid ? 1 : 0) + (s.placed_ask ? 1 : 0);
        stats.cancels += (s.cancelled_bid ? 1 : 0) + (s.cancelled_ask ? 1 : 0);
        stats.rejected += s.errors;
        if (metrics)
            metrics->on_decision(static_cast<uint32_t>(
                s.placed_bid || s.placed_ask ? TickResult::SUBMITTED
                                             : TickResult::NO_SIGNAL));
        ++stats.ticks;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (loop_out) *loop_out = loop;
    return stats;
}

}  // namespace crowdintel

#endif  // CROWDINTEL_MAKER_ENGINE_HPP
