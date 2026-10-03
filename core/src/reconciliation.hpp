#ifndef RECONCILIATION_HPP
#define RECONCILIATION_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Startup / post-disconnect reconciliation and the READINESS verdict.
//
// The venue's user stream does not replay what was missed while we were
// disconnected ("Recover After Reconnecting",
// https://docs.polymarket.com/trading/realtime-order-updates), and the market
// stream is not an account-state source.  Authoritative state comes from REST:
// GET /data/orders, GET /data/trades, GET /data/order/{id},
// GET /balance-allowance.
//
// READINESS is exactly READY or BLOCKED with explicit reasons.  Any of these
// blocks live trading:
//   * a local order that is not on the venue and cannot be resolved
//   * a venue order with no local record (adopted + blocked)
//   * a fill reported by the venue that the ledger never recorded
//   * a matched-size disagreement between the local and venue views
//   * collateral balance or allowance below the configured requirement
//   * inventory that disagrees with the venue's outcome-token balance
//   * any order left in UNKNOWN or SUBMITTING
//   * a heartbeat verdict implying the venue cancelled our orders
//   * a ledger that cannot be opened or durably written
//
// Concurrency: the ledger mutex is taken only for local reads/writes.  Network
// calls always happen *outside* the lock, in three phases:
//   A. read the venue (orders, trades, balances)      — no lock
//   B. snapshot local state and compute divergences   — short lock
//   C. record the resulting events                    — short lock
// Reconnecting a socket never launches a second listener: the clients own one
// thread each and reconnect internally; reconciliation runs on the supervisor
// (cold) thread before trading is re-enabled.
// ─────────────────────────────────────────────────────────────────────────────

#include <strings.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/event_ledger.hpp"
#include "../include/venue_metadata.hpp"
#include "clob_rest_client.hpp"
#include "order_heartbeat.hpp"

namespace recon {

enum class Readiness : uint8_t { BLOCKED = 0, READY = 1 };

inline const char* readiness_name(Readiness readiness) noexcept {
    return readiness == Readiness::READY ? "READY" : "BLOCKED";
}

struct Context {
    char condition_id[70]{};
    char token_id[80]{};
    char collateral_spender[43]{};  // exchange that must be approved
    bool neg_risk = false;
    uint64_t min_collateral = 0;    // base units required to trade
    uint64_t target_allowance = 0;  // exposure limit + operating margin
    uint64_t max_pages = 4;
    uint32_t signature_type = 0;
    bool allow_initial_position_adoption = false;  // true only at cold startup
};

struct Report {
    static constexpr size_t K_MAX_REASONS = 12;
    Readiness readiness = Readiness::BLOCKED;
    size_t reason_count = 0;
    char reasons[K_MAX_REASONS][48]{};

    size_t venue_open_orders = 0;
    size_t local_open_orders = 0;
    size_t adopted_orders = 0;
    size_t orders_missing_on_venue = 0;
    size_t matched_size_divergences = 0;
    size_t fills_unregistered = 0;
    size_t trades_examined = 0;
    size_t unknown_orders = 0;
    size_t tickets_linked = 0;
    uint64_t collateral_balance = 0;
    uint64_t collateral_allowance = 0;
    uint64_t outcome_balance = 0;
    uint64_t ledger_inventory = 0;
    bool balances_valid = false;
    uint64_t run_id = 0;
    uint64_t started_wall_ns = 0;
    uint64_t finished_wall_ns = 0;
    char heartbeat_health[16]{};

    void block(const char* reason) noexcept {
        readiness = Readiness::BLOCKED;
        if (reason_count < K_MAX_REASONS) {
            std::snprintf(reasons[reason_count], sizeof(reasons[reason_count]), "%s",
                          reason);
            ++reason_count;
        }
    }
    void reset() noexcept {
        readiness = Readiness::BLOCKED;
        reason_count = 0;
        for (auto& reason : reasons) reason[0] = '\0';
    }
    void format_reasons(char* out, size_t cap) const noexcept {
        if (!out || cap == 0) return;
        out[0] = '\0';
        size_t written = 0;
        for (size_t i = 0; i < reason_count; ++i) {
            const size_t length = std::strlen(reasons[i]);
            if (written + length + 2 >= cap) break;
            if (written) { out[written++] = ';'; out[written] = '\0'; }
            std::memcpy(out + written, reasons[i], length);
            written += length;
            out[written] = '\0';
        }
    }
};

class Reconciler {
public:
    static constexpr size_t K_MAX_VENUE_ORDERS = 256;
    static constexpr size_t K_MAX_LOCAL_ORDERS = 256;
    static constexpr size_t K_MAX_TRADE_PAGES = 4;

    Reconciler(clob::ClobApiClient& api, ledger::EventLedger& ledger,
               const Context& context)
        : api_(api), ledger_(ledger), context_(context) {}

    bool run(Report& report, const heartbeat::OrderHeartbeat* heartbeat_monitor,
             bool startup) {
        report = Report{};
        report.started_wall_ns = ledger::now_wall_ns();
        report.run_id = report.started_wall_ns;
        context_.allow_initial_position_adoption = startup;

        if (ledger_.corrupt() || !ledger_.is_open()) {
            report.block("ledger_unavailable");
            return finish(report);
        }
        if (!api_.credentials_ready()) {
            report.block("credentials_unavailable");
            return finish(report);
        }

        // ── Phase A: authoritative venue reads (no ledger lock held) ─────────
        if (!fetch_open_orders(report)) return finish(report, heartbeat_monitor);

        // ── Phase B: local snapshot + divergence plan ────────────────────────
        LocalOrder local[K_MAX_LOCAL_ORDERS];
        size_t local_count = 0;
        {
            auto lock = ledger_.guard();
            local_count = snapshot_local_orders(local);
        }
        report.local_open_orders = local_count;
        for (size_t i = 0; i < local_count; ++i)
            resolve_local_order(local[i], report);

        // Adopt venue orders with no local record (after ticket matching).
        for (size_t i = 0; i < report.venue_open_orders; ++i) {
            if (venue_orders_[i].consumed) continue;
            bool known = false;
            {
                auto lock = ledger_.guard();
                known = ledger_.orders().find(venue_orders_[i].id) != nullptr;
            }
            if (known) continue;
            ++report.adopted_orders;
            adopt_order(venue_orders_[i]);
            report.block("venue_order_missing_locally");
        }

        // ── Phase C: trades the ledger never recorded ────────────────────────
        if (!reconcile_trades(report)) return finish(report, heartbeat_monitor);

        // ── Phase D: balances, allowances, inventory ─────────────────────────
        if (!reconcile_balances(report)) return finish(report, heartbeat_monitor);

        // ── Phase E: heartbeat verdict and residual unknown state ────────────
        if (heartbeat_monitor && heartbeat_monitor->enabled()) {
            const heartbeat::Health health = heartbeat_monitor->health();
            std::snprintf(report.heartbeat_health, sizeof(report.heartbeat_health),
                          "%s", heartbeat::health_name(health));
            if (heartbeat::health_blocks_new_orders(health))
                report.block("heartbeat_not_healthy");
            if (heartbeat::health_implies_cancelled_orders(health))
                report.block("heartbeat_orders_may_be_cancelled");
        } else {
            std::snprintf(report.heartbeat_health, sizeof(report.heartbeat_health),
                          "%s", heartbeat::health_name(heartbeat::Health::DISABLED));
        }
        const size_t blocking = ledger_.blocking_orders();
        report.unknown_orders = blocking;
        if (blocking != 0) report.block("order_state_unknown");
        if (report.reason_count == 0) report.readiness = Readiness::READY;
        return finish(report, heartbeat_monitor);
    }

    bool run_after_disconnect(Report& report,
                              const heartbeat::OrderHeartbeat* heartbeat_monitor) {
        return run(report, heartbeat_monitor, false);
    }

    bool run_startup(Report& report,
                     const heartbeat::OrderHeartbeat* heartbeat_monitor) {
        return run(report, heartbeat_monitor, true);
    }

private:
    struct VenueOrder {
        char id[80]{};
        char status[24]{};
        char asset_id[80]{};
        bool status_known = false;
        bool consumed = false;
        uint8_t side = 0;
        uint64_t matched = 0;
        uint64_t original = 0;
        uint64_t price_raw = 0;
    };

    struct LocalOrder {
        char id[80]{};
        char token_id[80]{};
        OrderState state = OrderState::NONE;
        uint8_t side = 0;
        uint64_t matched = 0;
        uint64_t original = 0;
        uint64_t price_raw = 0;
        uint64_t salt = 0;
        bool is_ticket = false;
    };

    // ── Phase A ─────────────────────────────────────────────────────────────
    bool fetch_open_orders(Report& report) {
        char cursor[32]{};
        size_t venue_orders = 0;
        for (uint64_t page_index = 0; page_index < context_.max_pages; ++page_index) {
            clob::OrderPage page{};
            clob::CallResult call{};
            if (!api_.open_orders(context_.condition_id, nullptr,
                                  cursor[0] ? cursor : nullptr, page, call)) {
                report.block(call.ambiguous ? "orders_query_ambiguous"
                                            : "orders_query_failed");
                return false;
            }
            if (!page.complete) {
                report.block("orders_page_incomplete");
                return false;
            }
            for (size_t i = 0; i < page.count; ++i) {
                if (venue_orders >= K_MAX_VENUE_ORDERS) {
                    report.block("venue_order_set_truncated");
                    return false;
                }
                VenueOrder& slot = venue_orders_[venue_orders++];
                slot = VenueOrder{};
                std::snprintf(slot.id, sizeof(slot.id), "%s", page.items[i].id);
                std::snprintf(slot.status, sizeof(slot.status), "%s",
                              page.items[i].status);
                std::snprintf(slot.asset_id, sizeof(slot.asset_id), "%s",
                              page.items[i].asset_id);
                slot.status_known = slot.status[0] != '\0';
                slot.side = page.items[i].side;
                slot.matched = page.items[i].size_matched;
                slot.original = page.items[i].original_size;
                slot.price_raw = page.items[i].price_raw;
            }
            if (!page.has_more || !page.next_cursor[0]) break;
            std::snprintf(cursor, sizeof(cursor), "%s", page.next_cursor);
            if (std::strcmp(cursor, clob::K_CURSOR_END_VALUE) == 0) break;
        }
        report.venue_open_orders = venue_orders;
        return true;
    }

    // ── Phase B ─────────────────────────────────────────────────────────────
    size_t snapshot_local_orders(LocalOrder* out) const {
        size_t count = 0;
        for (size_t slot = 0; slot < ledger::EventLedger::K_ORDER_SLOTS; ++slot) {
            const ledger::OrderRecord* record = ledger_.orders().at(slot);
            if (!record || !record->order_id[0]) continue;
            if (!order_state_is_open(record->state)) continue;
            if (context_.condition_id[0] && record->condition_id[0] &&
                strcasecmp(record->condition_id, context_.condition_id) != 0)
                continue;
            if (count >= K_MAX_LOCAL_ORDERS) break;
            LocalOrder& local = out[count++];
            local = LocalOrder{};
            std::snprintf(local.id, sizeof(local.id), "%s", record->order_id);
            std::snprintf(local.token_id, sizeof(local.token_id), "%s",
                          record->token_id);
            local.state = record->state;
            local.side = record->side;
            local.matched = record->matched_size;
            local.original = record->original_size;
            local.price_raw = record->price_raw;
            local.salt = record->salt;
            local.is_ticket = record->order_id[0] == 'L' && record->order_id[1] == ':';
        }
        return count;
    }

    // Resolves one local order against the venue set (network calls allowed:
    // the ledger lock is NOT held here).
    void resolve_local_order(const LocalOrder& local, Report& report) {
        VenueOrder* match = find_venue_order(local.id);
        if (!match && local.is_ticket) {
            // A submission ticket has no venue id yet: look for the venue order
            // that matches its fingerprint (asset, side, price, size).  A unique
            // match links the ticket; anything else stays ambiguous.
            match = match_ticket(local, report);
        }
        const bool unresolved = order_state_blocks_new_orders(local.state);
        if (match) {
            match->consumed = true;
            bool recognized = false;
            const bool fully = match->original != 0 && match->matched >= match->original;
            const OrderState venue_state = venue_status::from_order_status(
                match->status_known ? match->status : nullptr, fully, recognized);
            if (local.is_ticket) {
                ++report.tickets_linked;
                link_ticket(local, *match, venue_state, recognized);
                return;
            }
            if (unresolved && recognized) {
                apply_order_state(local.id, venue_state, match->matched, match->status,
                                  ledger::Source::REST, "resolved_by_reconciliation");
                return;
            }
            if (match->matched > local.matched) {
                ++report.matched_size_divergences;
                record_state_event(local.id, "matched_size_divergence");
                report.block("fill_not_registered");
                return;
            }
            if (unresolved) report.block("order_state_unknown");
            return;
        }

        // Not on the book: resolve with an authoritative single read.
        ++report.orders_missing_on_venue;
        if (local.is_ticket) {
            // No venue id to query.  The order either never reached the venue or
            // is resting under an id we do not know: both are UNKNOWN.
            mark_unknown(local.id, "unlinked_submission_ticket");
            report.block("order_state_unknown");
            return;
        }
        clob::OrderWire wire{};
        clob::CallResult single{};
        if (api_.order(local.id, wire, single)) {
            bool recognized = false;
            const bool fully = wire.original_size != 0 &&
                               wire.size_matched >= wire.original_size;
            const OrderState resolved =
                venue_status::from_order_status(wire.status, fully, recognized);
            if (!recognized) {
                mark_unknown(local.id, "unresolved_venue_status");
                report.block("order_state_unknown");
                return;
            }
            if (resolved == OrderState::LIVE ||
                resolved == OrderState::PARTIALLY_FILLED) {
                // Resting somewhere the open-order list did not show: keep it
                // open but refuse to trade until the views agree.
                record_state_event(local.id, "order_open_but_unlisted");
                report.block("order_visibility_divergence");
                return;
            }
            apply_order_state(local.id, resolved, wire.size_matched, wire.status,
                              ledger::Source::REST, "resolved_by_reconciliation");
            if (wire.size_matched > local.matched) {
                ++report.matched_size_divergences;
                report.block("fill_not_registered");
            }
            return;
        }
        if (single.ambiguous) {
            mark_unknown(local.id, "order_query_ambiguous");
            report.block("order_state_unknown");
            return;
        }
        if (single.response.code == 404) {
            // The venue has no record: the order never became an order, so there
            // is no exposure.  Readiness still blocks until an operator (or the
            // next clean run) confirms the account view.
            apply_order_state(local.id, OrderState::FAILED, 0, "absent",
                              ledger::Source::REST, "order_absent_on_venue");
            report.block("local_order_absent_on_venue");
            return;
        }
        mark_unknown(local.id, "order_query_failed");
        report.block("order_state_unknown");
    }

    VenueOrder* find_venue_order(const char* order_id) noexcept {
        for (size_t i = 0; i < K_MAX_VENUE_ORDERS; ++i) {
            if (venue_orders_[i].id[0] && !venue_orders_[i].consumed &&
                std::strcmp(venue_orders_[i].id, order_id) == 0)
                return &venue_orders_[i];
        }
        return nullptr;
    }

    // Matches an unlinked submission ticket against the venue's open orders by
    // (asset, side, price, size).  Returns a match only when it is unique:
    // ambiguity must stay ambiguity.
    VenueOrder* match_ticket(const LocalOrder& ticket, Report& report) noexcept {
        VenueOrder* unique = nullptr;
        size_t matches = 0;
        for (size_t i = 0; i < K_MAX_VENUE_ORDERS; ++i) {
            VenueOrder& candidate = venue_orders_[i];
            if (!candidate.id[0] || candidate.consumed) continue;
            if (candidate.side != ticket.side) continue;
            if (candidate.original != ticket.original) continue;
            if (candidate.price_raw && ticket.price_raw &&
                candidate.price_raw != ticket.price_raw)
                continue;
            if (ticket.token_id[0] && candidate.asset_id[0] &&
                std::strcmp(candidate.asset_id, ticket.token_id) != 0)
                continue;
            ++matches;
            unique = &candidate;
        }
        if (matches == 1) return unique;
        if (matches > 1) report.block("submission_ticket_ambiguous");
        return nullptr;
    }

    void link_ticket(const LocalOrder& ticket, const VenueOrder& venue_order,
                     OrderState venue_state, bool recognized) {
        auto lock = ledger_.guard();
        char error[128]{};
        ledger::Event link{};
        link.type = ledger::EventType::ORDER_STATE;
        link.source = ledger::Source::REST;
        link.wall_ns = ledger::now_wall_ns();
        link.add_str(ledger::F_ORDER_ID, venue_order.id);
        if (ticket.token_id[0]) link.add_str(ledger::F_TOKEN_ID, ticket.token_id);
        if (context_.condition_id[0])
            link.add_str(ledger::F_CONDITION_ID, context_.condition_id);
        link.add_u8(ledger::F_SIDE, ticket.side);
        link.add_u64(ledger::F_SALT, ticket.salt);
        link.add_u64(ledger::F_SIZE_RAW, venue_order.original);
        link.add_u64(ledger::F_MATCHED_RAW, venue_order.matched);
        link.add_u64(ledger::F_PRICE_RAW, venue_order.price_raw);
        link.add_u8(ledger::F_STATE,
                    static_cast<uint8_t>(recognized ? venue_state
                                                    : OrderState::UNKNOWN));
        if (venue_order.status_known)
            link.add_str(ledger::F_STATUS_TEXT, venue_order.status);
        link.add_str(ledger::F_REASON, "ticket_linked_to_venue_order");
        ledger::compute_event_key(link.type, link.source, venue_order.id, "", 0,
                                  ticket.salt, link.key);
        (void)ledger_.commit_locked(link, error, sizeof(error));

        ledger::Event retire{};
        retire.type = ledger::EventType::ORDER_STATE;
        retire.source = ledger::Source::REST;
        retire.wall_ns = ledger::now_wall_ns();
        retire.add_str(ledger::F_ORDER_ID, ticket.id);
        retire.add_u8(ledger::F_STATE,
                      static_cast<uint8_t>(recognized ? OrderState::SUPERSEDED
                                                      : OrderState::UNKNOWN));
        retire.add_str(ledger::F_REASON, "linked_to_venue_order");
        ledger::compute_event_key(retire.type, retire.source, ticket.id, "", 0,
                                  ticket.salt + 11, retire.key);
        (void)ledger_.commit_locked(retire, error, sizeof(error));

        // Transfer the worst-case reservation to the venue-keyed record before
        // releasing the ticket's, so an open order is never unreserved.
        if (recognized && order_state_is_open(venue_state)) {
            ledger::Event reserve{};
            reserve.type = ledger::EventType::RISK_RESERVE;
            reserve.source = ledger::Source::REST;
            reserve.wall_ns = ledger::now_wall_ns();
            reserve.add_str(ledger::F_ORDER_ID, venue_order.id);
            reserve.add_u64(ledger::F_RESERVE_AMOUNT,
                            ticket.side == 0 ? venue_order.original *
                                                   venue_order.price_raw / 1000000ULL
                                             : 0);
            ledger::compute_event_key(reserve.type, reserve.source, venue_order.id, "",
                                      0, ticket.salt + 13, reserve.key);
            (void)ledger_.commit_locked(reserve, error, sizeof(error));
        }
        ledger::Event release{};
        release.type = ledger::EventType::RISK_RELEASE;
        release.source = ledger::Source::REST;
        release.wall_ns = ledger::now_wall_ns();
        release.add_str(ledger::F_ORDER_ID, ticket.id);
        ledger::compute_event_key(release.type, release.source, ticket.id, "", 0,
                                  ticket.salt + 12, release.key);
        (void)ledger_.commit_locked(release, error, sizeof(error));
    }

    // ── Phase C ─────────────────────────────────────────────────────────────
    bool reconcile_trades(Report& report) {
        char cursor[32]{};
        for (uint64_t page_index = 0; page_index < K_MAX_TRADE_PAGES; ++page_index) {
            clob::TradePage page{};
            clob::CallResult call{};
            if (!api_.trades(context_.condition_id, nullptr,
                             cursor[0] ? cursor : nullptr, page, call)) {
                report.block(call.ambiguous ? "trades_query_ambiguous"
                                            : "trades_query_failed");
                return false;
            }
            if (!page.complete) {
                report.block("trades_page_incomplete");
                return false;
            }
            for (size_t i = 0; i < page.count; ++i) {
                ++report.trades_examined;
                const clob::TradeWire& wire = page.items[i];
                bool known = false;
                {
                    auto lock = ledger_.guard();
                    known = ledger_.fills().find(wire.id) != nullptr;
                }
                if (known) continue;
                ++report.fills_unregistered;
                record_unregistered_fill(wire);
                report.block("fill_not_registered");
            }
            if (!page.has_more || !page.next_cursor[0]) return true;
            std::snprintf(cursor, sizeof(cursor), "%s", page.next_cursor);
            if (std::strcmp(cursor, clob::K_CURSOR_END_VALUE) == 0) return true;
        }
        report.block("trade_history_truncated");
        return false;
    }

    void record_unregistered_fill(const clob::TradeWire& wire) {
        const auto kind = venue_status::trade_kind(wire.status);
        auto lock = ledger_.guard();
        ledger::Event event{};
        event.type = ledger::EventType::FILL;
        event.source = ledger::Source::REST;
        event.wall_ns = ledger::now_wall_ns();
        event.venue_ts_ms = wire.match_time_s * 1000ULL;
        event.add_str(ledger::F_TRADE_ID, wire.id);
        if (wire.taker_order_id[0])
            event.add_str(ledger::F_ORDER_ID, wire.taker_order_id);
        event.add_str(ledger::F_TOKEN_ID, wire.asset_id);
        if (wire.market[0]) event.add_str(ledger::F_CONDITION_ID, wire.market);
        event.add_u8(ledger::F_SIDE, wire.side);
        event.add_u8(ledger::F_RESULT, static_cast<uint8_t>(kind));
        event.add_u64(ledger::F_PRICE_RAW, wire.price_raw);
        event.add_u64(ledger::F_SIZE_RAW, wire.size_raw);
        event.add_u64(ledger::F_FEE_BPS, wire.fee_rate_bps);
        event.add_str(ledger::F_STATUS_TEXT, wire.status);
        event.add_str(ledger::F_REASON, "discovered_by_reconciliation");
        ledger::compute_event_key(event.type, event.source, wire.taker_order_id,
                                  wire.id, event.venue_ts_ms, 0, event.key);
        char error[128]{};
        (void)ledger_.commit_locked(event, error, sizeof(error));
    }

    // ── Phase D ─────────────────────────────────────────────────────────────
    bool reconcile_balances(Report& report) {
        clob::CallResult call{};
        venue::BalanceAllowance collateral{};
        if (!api_.balance_allowance(venue::AssetType::COLLATERAL, nullptr,
                                    context_.collateral_spender, collateral, call)) {
            report.block(call.ambiguous ? "collateral_query_ambiguous"
                                        : "collateral_query_failed");
            return false;
        }
        report.collateral_balance = collateral.balance;
        report.collateral_allowance =
            collateral.allowance_valid ? collateral.allowance : 0;
        report.balances_valid = collateral.balance_valid;
        record_balance(venue::AssetType::COLLATERAL, "", context_.collateral_spender,
                       collateral);
        if (collateral.balance < context_.min_collateral)
            report.block("collateral_balance_insufficient");
        if (!collateral.allowance_valid)
            report.block("collateral_allowance_unknown");
        else if (collateral.allowance < context_.target_allowance)
            report.block("collateral_allowance_insufficient");

        const venue::AssetType outcome_type =
            venue::is_protocol_v2_position_id(context_.token_id)
                ? venue::AssetType::CONDITIONAL_V2
                : venue::AssetType::CONDITIONAL;
        venue::BalanceAllowance outcome{};
        clob::CallResult outcome_call{};
        char exchange[43]{};
        uint8_t exchange_bytes[20]{};
        api_.exchange_for_token(context_.token_id, context_.neg_risk, exchange_bytes,
                                exchange);
        if (!api_.balance_allowance(outcome_type, context_.token_id, exchange, outcome,
                                    outcome_call)) {
            report.block(outcome_call.ambiguous ? "inventory_query_ambiguous"
                                                : "inventory_query_failed");
            return false;
        }
        report.outcome_balance = outcome.balance;
        record_balance(outcome_type, context_.token_id, exchange, outcome);

        bool never_observed = false;
        {
            auto lock = ledger_.guard();
            report.ledger_inventory = ledger_.available_inventory_locked(context_.token_id);
            const ledger::PositionRecord* position =
                ledger_.positions().find(context_.token_id);
            never_observed = !position || position->observed_wall_ns == 0;
        }
        if (never_observed && context_.allow_initial_position_adoption) {
            // Cold start: the venue is the only source of truth for inventory.
            record_position(context_.token_id, outcome.balance);
        } else if (report.ledger_inventory > outcome.balance) {
            // Believing we can sell more than we hold must never reach egress.
            report.block("inventory_exceeds_venue_balance");
        }

        // An ERC-1155 approval is only required to sell.
        const bool has_inventory =
            outcome.balance != 0 || report.ledger_inventory != 0;
        if (has_inventory && outcome.allowance_valid && outcome.allowance == 0)
            report.block("outcome_token_allowance_missing");
        if (has_inventory && !outcome.allowance_valid)
            report.block("outcome_token_allowance_unknown");
        return true;
    }

    void record_balance(venue::AssetType type, const char* token_id,
                        const char* spender, const venue::BalanceAllowance& value) {
        auto lock = ledger_.guard();
        ledger::Event event{};
        event.type = ledger::EventType::BALANCE_SNAPSHOT;
        event.source = ledger::Source::REST;
        event.wall_ns = ledger::now_wall_ns();
        if (token_id && token_id[0]) event.add_str(ledger::F_TOKEN_ID, token_id);
        if (spender && spender[0]) event.add_str(ledger::F_SPENDER, spender);
        event.add_u8(ledger::F_ASSET_TYPE, static_cast<uint8_t>(type));
        if (value.balance_valid) event.add_u64(ledger::F_BALANCE, value.balance);
        if (value.allowance_valid)
            event.add_u64(ledger::F_ALLOWANCE, value.allowance);
        ledger::compute_event_key(
            event.type, event.source, "", "", 0,
            ledger::fnv1a(token_id ? token_id : "",
                          std::strlen(token_id ? token_id : "")) ^
                ledger::fnv1a(spender ? spender : "",
                              std::strlen(spender ? spender : "")),
            event.key);
        char error[128]{};
        (void)ledger_.commit_locked(event, error, sizeof(error));
    }

    void record_position(const char* token_id, uint64_t shares) {
        auto lock = ledger_.guard();
        ledger::Event event{};
        event.type = ledger::EventType::POSITION_SNAPSHOT;
        event.source = ledger::Source::REST;
        event.wall_ns = ledger::now_wall_ns();
        event.add_str(ledger::F_TOKEN_ID, token_id);
        event.add_u64(ledger::F_SIZE_RAW, shares);
        ledger::compute_event_key(event.type, event.source, "", "", 0,
                                  ledger::fnv1a(token_id, std::strlen(token_id)),
                                  event.key);
        char error[128]{};
        (void)ledger_.commit_locked(event, error, sizeof(error));
    }

    void apply_order_state(const char* order_id, OrderState state, uint64_t matched,
                           const char* status, ledger::Source source,
                           const char* reason) {
        auto lock = ledger_.guard();
        ledger::Event event{};
        event.type = ledger::EventType::ORDER_STATE;
        event.source = source;
        event.wall_ns = ledger::now_wall_ns();
        event.add_str(ledger::F_ORDER_ID, order_id);
        event.add_u8(ledger::F_STATE, static_cast<uint8_t>(state));
        if (matched) event.add_u64(ledger::F_MATCHED_RAW, matched);
        if (status && status[0]) event.add_str(ledger::F_STATUS_TEXT, status);
        if (reason && reason[0]) event.add_str(ledger::F_REASON, reason);
        ledger::compute_event_key(event.type, event.source, order_id, "",
                                  event.wall_ns / 1000000ULL,
                                  static_cast<uint64_t>(state), event.key);
        char error[128]{};
        (void)ledger_.commit_locked(event, error, sizeof(error));
    }

    void mark_unknown(const char* order_id, const char* reason) {
        auto lock = ledger_.guard();
        ledger::Event event{};
        event.type = ledger::EventType::ORDER_UNKNOWN;
        event.source = ledger::Source::REST;
        event.wall_ns = ledger::now_wall_ns();
        event.add_str(ledger::F_ORDER_ID, order_id);
        event.add_u8(ledger::F_STATE, static_cast<uint8_t>(OrderState::UNKNOWN));
        event.add_str(ledger::F_REASON, reason);
        ledger::compute_event_key(event.type, event.source, order_id, "",
                                  event.wall_ns / 1000000ULL,
                                  ledger::fnv1a(reason, std::strlen(reason)),
                                  event.key);
        char error[128]{};
        (void)ledger_.commit_locked(event, error, sizeof(error));
    }

    void record_state_event(const char* order_id, const char* reason) {
        auto lock = ledger_.guard();
        ledger::Event event{};
        event.type = ledger::EventType::STATE_EVENT;
        event.source = ledger::Source::REST;
        event.wall_ns = ledger::now_wall_ns();
        event.add_str(ledger::F_ORDER_ID, order_id);
        event.add_str(ledger::F_REASON, reason);
        ledger::compute_event_key(event.type, event.source, order_id, "", 0,
                                  ledger::fnv1a(reason, std::strlen(reason)),
                                  event.key);
        char error[128]{};
        (void)ledger_.commit_locked(event, error, sizeof(error));
    }

    void adopt_order(const VenueOrder& venue_order) {
        bool recognized = false;
        const OrderState state = venue_status::from_order_status(
            venue_order.status_known ? venue_order.status : nullptr,
            venue_order.original != 0 && venue_order.matched >= venue_order.original,
            recognized);
        auto lock = ledger_.guard();
        ledger::Event event{};
        event.type = ledger::EventType::VENUE_ORDER_ADOPTED;
        event.source = ledger::Source::REST;
        event.wall_ns = ledger::now_wall_ns();
        event.add_str(ledger::F_ORDER_ID, venue_order.id);
        if (venue_order.asset_id[0])
            event.add_str(ledger::F_TOKEN_ID, venue_order.asset_id);
        else if (context_.token_id[0])
            event.add_str(ledger::F_TOKEN_ID, context_.token_id);
        if (context_.condition_id[0])
            event.add_str(ledger::F_CONDITION_ID, context_.condition_id);
        event.add_u8(ledger::F_SIDE, venue_order.side);
        event.add_u64(ledger::F_SIZE_RAW, venue_order.original);
        event.add_u64(ledger::F_MATCHED_RAW, venue_order.matched);
        event.add_u64(ledger::F_PRICE_RAW, venue_order.price_raw);
        event.add_u8(ledger::F_STATE,
                     static_cast<uint8_t>(recognized ? state : OrderState::UNKNOWN));
        if (venue_order.status_known)
            event.add_str(ledger::F_STATUS_TEXT, venue_order.status);
        event.add_str(ledger::F_REASON, "adopted_from_venue");
        ledger::compute_event_key(event.type, event.source, venue_order.id, "", 0,
                                  venue_order.matched, event.key);
        char error[128]{};
        (void)ledger_.commit_locked(event, error, sizeof(error));
    }

    bool finish(Report& report,
                const heartbeat::OrderHeartbeat* heartbeat_monitor = nullptr) {
        (void)heartbeat_monitor;
        report.finished_wall_ns = ledger::now_wall_ns();
        char reasons[256]{};
        report.format_reasons(reasons, sizeof(reasons));
        {
            auto lock = ledger_.guard();
            ledger::Event event{};
            event.type = ledger::EventType::RECONCILIATION_RUN;
            event.source = ledger::Source::REST;
            event.wall_ns = report.finished_wall_ns;
            event.venue_ts_ms = report.started_wall_ns / 1000000ULL;
            event.add_u64(ledger::F_RUN_ID, report.run_id);
            event.add_u8(ledger::F_RESULT,
                         report.readiness == Readiness::READY ? 1 : 0);
            if (reasons[0]) event.add_str(ledger::F_REASON, reasons);
            ledger::compute_event_key(event.type, event.source, "", "", 0,
                                      report.run_id, event.key);
            char error[128]{};
            if (!ledger_.commit_locked(event, error, sizeof(error))) {
                report.block("ledger_write_failed");
                report.readiness = Readiness::BLOCKED;
                return false;
            }
        }
        return report.readiness == Readiness::READY;
    }

    clob::ClobApiClient& api_;
    ledger::EventLedger& ledger_;
    Context context_;
    VenueOrder venue_orders_[K_MAX_VENUE_ORDERS]{};
};

}  // namespace recon

#endif  // RECONCILIATION_HPP
