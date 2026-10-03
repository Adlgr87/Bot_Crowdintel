#ifndef ORDER_RECORDER_HPP
#define ORDER_RECORDER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Order recorder: the bridge between order egress and the persistent ledger.
//
// Contract (fail-closed):
//   * BEFORE the transport is touched, a submission ticket is durably recorded
//     (LOCAL_CREATED → SIGNED → SUBMITTING) keyed by the order's EIP-712 digest,
//     so a crash or a lost response still leaves proof of what was in flight;
//   * AFTER the transport answers, the outcome is recorded:
//       - unambiguous success  → venue-keyed order record (LIVE/PARTIALLY_FILLED/
//                                MATCHED) + ticket SUPERSEDED, plus any trade ids
//                                the response carries (a FAK partial fill is a
//                                real fill and must not wait for the stream);
//       - unambiguous failure  → ticket REJECTED/FAILED (no exposure);
//       - ambiguous outcome    → ticket UNKNOWN, never re-sent, and
//                                `trading_enabled` must go false until an
//                                authoritative read resolves it.
//   * a ledger write failure is reported to the caller, which must stop trading:
//     continuing without a durable record is how duplicate exposure happens.
//
// This class contains no sockets and no venue vocabulary beyond the wire body we
// ourselves serialized, so it is unit-testable with fixtures.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../crypto/order_digest.hpp"
#include "../include/event_ledger.hpp"
#include "../include/json_scan.hpp"
#include "../include/order_state.hpp"
#include "../include/venue_metadata.hpp"
#include "clob_rest_client.hpp"
#include "market_config.hpp"
#include "polymarket_order.hpp"

namespace recorder {

// Parsed view of the wire body we are about to send.
struct WireOrder {
    uint64_t salt = 0;
    uint64_t maker_amount = 0;
    uint64_t taker_amount = 0;
    uint64_t timestamp_ms = 0;
    uint64_t expiration_s = 0;
    uint8_t side = 0;
    uint8_t signature_type = 0;
    char maker[43]{};
    char signer[43]{};
    char token_id[80]{};
    char metadata[67]{};
    char builder[67]{};
    char order_type[8]{};
    char owner[96]{};
    OrderV2 order{};
    uint8_t digest[32]{};
    char digest_hex[67]{};
    char ticket_id[80]{};  // "L:" + digest hex
    bool valid = false;
};

inline bool hex_to_bytes(const char* text, size_t expected_bytes, uint8_t* out) noexcept {
    const size_t len = std::strlen(text);
    if (len != expected_bytes * 2 + 2 || text[0] != '0' ||
        (text[1] != 'x' && text[1] != 'X'))
        return false;
    auto nibble = [](char c, int& value) {
        if (c >= '0' && c <= '9') { value = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { value = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { value = c - 'A' + 10; return true; }
        return false;
    };
    for (size_t i = 0; i < expected_bytes; ++i) {
        int high = 0;
        int low = 0;
        if (!nibble(text[2 + i * 2], high) || !nibble(text[3 + i * 2], low))
            return false;
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

// Parses the exact body build_wire_body() produces.  Anything unexpected is
// rejected: we would otherwise record an order we cannot describe.
inline bool parse_wire_body(const char* body, size_t len, bool neg_risk,
                            WireOrder& out) noexcept {
    out = WireOrder{};
    if (!body || len == 0) return false;
    size_t start = 0;
    size_t end = 0;
    if (!json_scan::find_object(body, len, "order", start, end)) return false;
    const char* order = body + start;
    const size_t order_len = end - start;
    if (!json_scan::get_u64(order, order_len, "salt", out.salt)) {
        // build_wire_body emits salt as a JSON number; accept a quoted form too
        // so a hand-built body is parsed rather than silently mis-described.
        char text[32]{};
        if (!json_scan::get_string(order, order_len, "salt", text, sizeof(text)))
            return false;
        if (!json_scan::parse_fixed(text, std::strlen(text), 1, out.salt)) return false;
    }
    if (!json_scan::get_string(order, order_len, "maker", out.maker, sizeof(out.maker)))
        return false;
    if (!json_scan::get_string(order, order_len, "signer", out.signer,
                               sizeof(out.signer)))
        return false;
    if (!json_scan::get_string(order, order_len, "tokenId", out.token_id,
                               sizeof(out.token_id)))
        return false;
    // Amounts and timestamps are decimal *strings* on the wire (never binary
    // floating point), per build_wire_body and the official SDK payload.
    if (!json_scan::get_fixed(order, order_len, "makerAmount", 1, out.maker_amount))
        return false;
    if (!json_scan::get_fixed(order, order_len, "takerAmount", 1, out.taker_amount))
        return false;
    {
        uint64_t side = 0;
        if (json_scan::get_u64(order, order_len, "side", side)) {
            if (side > 1) return false;
            out.side = static_cast<uint8_t>(side);
        } else {
            char text[8]{};
            if (!json_scan::get_string(order, order_len, "side", text, sizeof(text)))
                return false;
            if (std::strcmp(text, "BUY") == 0) out.side = 0;
            else if (std::strcmp(text, "SELL") == 0) out.side = 1;
            else return false;
        }
    }
    {
        uint64_t signature_type = 0;
        if (!json_scan::get_u64(order, order_len, "signatureType", signature_type)) {
            char text[8]{};
            if (!json_scan::get_string(order, order_len, "signatureType", text,
                                       sizeof(text)))
                return false;
            if (!json_scan::parse_fixed(text, std::strlen(text), 1, signature_type))
                return false;
        }
        out.signature_type = static_cast<uint8_t>(signature_type);
    }
    if (!json_scan::get_fixed(order, order_len, "timestamp", 1, out.timestamp_ms))
        return false;
    if (!json_scan::get_fixed(order, order_len, "expiration", 1, out.expiration_s))
        out.expiration_s = 0;
    if (!json_scan::get_string(order, order_len, "metadata", out.metadata,
                               sizeof(out.metadata)))
        return false;
    if (!json_scan::get_string(order, order_len, "builder", out.builder,
                               sizeof(out.builder)))
        return false;
    if (!json_scan::get_string(body, len, "orderType", out.order_type,
                               sizeof(out.order_type)))
        return false;
    (void)json_scan::get_string(body, len, "owner", out.owner, sizeof(out.owner));

    if (out.side > 1 || out.signature_type > 2) return false;
    if (!json_scan::is_decimal_integer(out.token_id, std::strlen(out.token_id)))
        return false;
    if (!hex_to_bytes(out.maker, 20, out.order.maker)) return false;
    if (!hex_to_bytes(out.signer, 20, out.order.signer)) return false;
    if (!hex_to_bytes(out.metadata, 32, out.order.metadata)) return false;
    if (!hex_to_bytes(out.builder, 32, out.order.builder)) return false;
    uint64_t limbs[4] = {0, 0, 0, 0};
    if (!venue::parse_uint256_limbs(out.token_id, std::strlen(out.token_id), limbs))
        return false;
    for (int word = 0; word < 4; ++word) {
        for (int byte = 0; byte < 8; ++byte)
            out.order.token_id[word * 8 + byte] =
                static_cast<uint8_t>((limbs[3 - word] >> (8 * (7 - byte))) & 0xFFULL);
    }
    out.order.salt = out.salt;
    out.order.maker_amount = out.maker_amount;
    out.order.taker_amount = out.taker_amount;
    out.order.side = out.side;
    out.order.signature_type = out.signature_type;
    out.order.timestamp_ms = out.timestamp_ms;
    if (!crowdintel::compute_order_digest(out.order, neg_risk, out.digest)) return false;
    crowdintel::digest_to_hex(out.digest, out.digest_hex);
    std::snprintf(out.ticket_id, sizeof(out.ticket_id), "L:%s", out.digest_hex + 2);
    out.valid = true;
    return true;
}

class OrderRecorder {
public:
    OrderRecorder(ledger::EventLedger& ledger, const MarketConfig& config)
        : ledger_(ledger), config_(config) {}

    // Records the ticket.  Must be called before the transport is touched.
    // Returns false when the durable write failed → the caller must not send.
    bool record_submission(const WireOrder& wire, char* error, size_t cap) {
        if (!wire.valid) {
            std::snprintf(error, cap, "wire body could not be parsed");
            return false;
        }
        auto lock = ledger_.guard();
        ledger::Event event{};
        event.type = ledger::EventType::ORDER_SUBMITTING;
        event.source = ledger::Source::LOCAL;
        event.wall_ns = ledger::now_wall_ns();
        event.venue_ts_ms = wire.timestamp_ms;
        event.add_str(ledger::F_ORDER_ID, wire.ticket_id);
        event.add_str(ledger::F_TOKEN_ID, wire.token_id);
        if (config_.condition_id[0])
            event.add_str(ledger::F_CONDITION_ID, config_.condition_id);
        event.add_u8(ledger::F_SIDE, wire.side);
        event.add_u64(ledger::F_SALT, wire.salt);
        event.add_u64(ledger::F_MAKER_AMOUNT, wire.maker_amount);
        event.add_u64(ledger::F_TAKER_AMOUNT, wire.taker_amount);
        // BUY commits collateral (makerAmount), SELL commits inventory
        // (makerAmount is the share amount); both are the worst case.
        event.add_u64(ledger::F_PRICE_RAW, price_raw(wire));
        event.add_u64(ledger::F_SIZE_RAW, size_raw(wire));
        event.add_u8(ledger::F_STATE, static_cast<uint8_t>(OrderState::SUBMITTING));
        event.add_str(ledger::F_ORDER_TYPE, wire.order_type);
        event.add_str(ledger::F_STATUS_TEXT, "submitting");
        ledger::compute_event_key(event.type, event.source, wire.ticket_id, "",
                                  wire.timestamp_ms, wire.salt, event.key);
        if (!ledger_.commit_locked(event, error, cap)) {
            write_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        // Reserve the worst-case exposure so the risk gate cannot double-spend
        // while the submission is in flight.
        ledger::Event reserve{};
        reserve.type = ledger::EventType::RISK_RESERVE;
        reserve.source = ledger::Source::LOCAL;
        reserve.wall_ns = ledger::now_wall_ns();
        reserve.add_str(ledger::F_ORDER_ID, wire.ticket_id);
        reserve.add_u64(ledger::F_RESERVE_AMOUNT, wire.maker_amount);
        ledger::compute_event_key(reserve.type, reserve.source, wire.ticket_id, "", 0,
                                  wire.salt, reserve.key);
        if (!ledger_.commit_locked(reserve, error, cap)) {
            write_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        tickets_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Records the venue outcome.  `ambiguous` means the venue may already have
    // acted; the ticket then goes to UNKNOWN and trading must stop until a
    // reconciliation resolves it.
    bool record_result(const WireOrder& wire, const SubmitResult& result,
                       bool ambiguous, const clob::OrderPostResult* detail,
                       char* error, size_t cap) {
        if (!wire.valid) return false;
        auto lock = ledger_.guard();
        OrderState state = OrderState::UNKNOWN;
        const char* reason = "ambiguous_transport";
        if (ambiguous) {
            state = OrderState::UNKNOWN;
            // `reason` is already "ambiguous_transport": the ledger vocabulary is
            // fixed on purpose (it is what reconciliation and the post-mortem
            // read), and the transport's own text is recorded separately below.
            ambiguous_results_.fetch_add(1, std::memory_order_relaxed);
        } else if (result.ok && result.order_id[0]) {
            bool recognized = false;
            const uint64_t filled = detail ? detail->taking_amount : 0;
            const uint64_t requested = wire.side == 0 ? wire.taker_amount
                                                      : wire.maker_amount;
            state = venue_status::from_post_response(result.status, filled, requested,
                                                     recognized);
            if (!recognized) {
                state = OrderState::UNKNOWN;
                reason = "unrecognized_post_status";
            } else {
                reason = "venue_accepted";
            }
        } else if (result.ok) {
            state = OrderState::UNKNOWN;
            reason = "accepted_without_order_id";
        } else if (result.error[0] && !result.retryable) {
            state = OrderState::REJECTED;
            reason = "venue_rejected";
        } else {
            state = OrderState::FAILED;
            reason = "transport_failed_pre_send";
        }

        if (result.order_id[0]) {
            // The venue id is now known: open the venue-keyed record and retire
            // the ticket.  Both events are durable before we return.
            if (std::strcmp(result.order_id, wire.digest_hex) == 0 &&
                !order_id_matches_digest_.exchange(true))
                std::fprintf(stderr,
                             "NOTE: venue orderID equals the local EIP-712 digest "
                             "(hypothesis confirmed for this order)\n");
            ledger::Event venue_event{};
            venue_event.type = ledger::EventType::ORDER_STATE;
            venue_event.source = ledger::Source::REST;
            venue_event.wall_ns = ledger::now_wall_ns();
            venue_event.venue_ts_ms = wire.timestamp_ms;
            venue_event.add_str(ledger::F_ORDER_ID, result.order_id);
            venue_event.add_str(ledger::F_TOKEN_ID, wire.token_id);
            if (config_.condition_id[0])
                venue_event.add_str(ledger::F_CONDITION_ID, config_.condition_id);
            venue_event.add_u8(ledger::F_SIDE, wire.side);
            venue_event.add_u64(ledger::F_SALT, wire.salt);
            venue_event.add_u64(ledger::F_MAKER_AMOUNT, wire.maker_amount);
            venue_event.add_u64(ledger::F_TAKER_AMOUNT, wire.taker_amount);
            venue_event.add_u64(ledger::F_PRICE_RAW, price_raw(wire));
            venue_event.add_u64(ledger::F_SIZE_RAW, size_raw(wire));
            const uint64_t filled_shares = detail ? detail->taking_amount : 0;
            if (wire.side == 0)
                venue_event.add_u64(ledger::F_MATCHED_RAW, filled_shares);
            venue_event.add_u8(ledger::F_STATE, static_cast<uint8_t>(state));
            venue_event.add_str(ledger::F_STATUS_TEXT,
                                result.status[0] ? result.status : "unknown");
            venue_event.add_str(ledger::F_ORDER_TYPE, wire.order_type);
            venue_event.add_str(ledger::F_REASON, reason);
            ledger::compute_event_key(venue_event.type, venue_event.source,
                                      result.order_id, "", wire.timestamp_ms,
                                      wire.salt, venue_event.key);
            if (!ledger_.commit_locked(venue_event, error, cap)) {
                write_failures_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // Move the worst-case reservation from the ticket to the venue-keyed
            // record BEFORE retiring the ticket, so there is no instant in which
            // an open order is unreserved (that window is exactly where a double
            // sell or a double spend would be authorised).
            if (order_state_is_open(state))
                reserve_for(result.order_id, wire.maker_amount, error, cap);
            retire_ticket(wire, state, error, cap);
            if (detail) record_response_trades(wire, *detail, error, cap);
            return true;
        }

        // No venue id: update the ticket itself.
        ledger::Event event{};
        event.type = state == OrderState::UNKNOWN ? ledger::EventType::ORDER_UNKNOWN
                                                  : ledger::EventType::ORDER_STATE;
        event.source = ledger::Source::REST;
        event.wall_ns = ledger::now_wall_ns();
        event.venue_ts_ms = wire.timestamp_ms;
        event.add_str(ledger::F_ORDER_ID, wire.ticket_id);
        event.add_u8(ledger::F_STATE, static_cast<uint8_t>(state));
        event.add_str(ledger::F_REASON, reason);
        // One value per field: Event::find() returns the FIRST field with a given
        // id, so writing both the venue status and the transport error into
        // F_STATUS_TEXT made the second one durably unreachable - exactly the text
        // an operator needs for an UNKNOWN order.  The venue status keeps its
        // precedence (it is the semantic value already mapped into `state`) and
        // the transport error is recorded when there is no status.
        if (result.status[0]) {
            event.add_str(ledger::F_STATUS_TEXT, result.status);
        } else if (result.error[0]) {
            char trimmed[96]{};
            std::snprintf(trimmed, sizeof(trimmed), "%.95s", result.error);
            event.add_str(ledger::F_STATUS_TEXT, trimmed);
        }
        ledger::compute_event_key(event.type, event.source, wire.ticket_id, "",
                                  wire.timestamp_ms, wire.salt + 1, event.key);
        if (!ledger_.commit_locked(event, error, cap)) {
            write_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (state != OrderState::UNKNOWN) {
            // Retiring the ticket releases the reservation.
            ledger::Event release{};
            release.type = ledger::EventType::RISK_RELEASE;
            release.source = ledger::Source::LOCAL;
            release.wall_ns = ledger::now_wall_ns();
            release.add_str(ledger::F_ORDER_ID, wire.ticket_id);
            ledger::compute_event_key(release.type, release.source, wire.ticket_id, "",
                                      0, wire.salt + 2, release.key);
            if (!ledger_.commit_locked(release, error, cap)) {
                write_failures_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        return true;
    }

    uint64_t tickets() const noexcept { return tickets_.load(std::memory_order_relaxed); }
    uint64_t ambiguous_results() const noexcept {
        return ambiguous_results_.load(std::memory_order_relaxed);
    }
    uint64_t write_failures() const noexcept {
        return write_failures_.load(std::memory_order_relaxed);
    }

private:
    static uint64_t price_raw(const WireOrder& wire) noexcept {
        const uint64_t shares = wire.side == 0 ? wire.taker_amount : wire.maker_amount;
        const uint64_t cost = wire.side == 0 ? wire.maker_amount : wire.taker_amount;
        if (!shares) return 0;
        return cost * 1000000ULL / shares;
    }

    static uint64_t size_raw(const WireOrder& wire) noexcept {
        return wire.side == 0 ? wire.taker_amount : wire.maker_amount;
    }

    // Records the worst-case reservation against a venue-keyed order.
    bool reserve_for(const char* order_id, uint64_t amount, char* error, size_t cap) {
        ledger::Event event{};
        event.type = ledger::EventType::RISK_RESERVE;
        event.source = ledger::Source::LOCAL;
        event.wall_ns = ledger::now_wall_ns();
        event.add_str(ledger::F_ORDER_ID, order_id);
        event.add_u64(ledger::F_RESERVE_AMOUNT, amount);
        ledger::compute_event_key(event.type, event.source, order_id, "", 0, amount,
                                  event.key);
        if (!ledger_.commit_locked(event, error, cap)) {
            write_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // Caller holds ledger_.guard().
    bool retire_ticket(const WireOrder& wire, OrderState venue_state, char* error,
                       size_t cap) {
        ledger::Event event{};
        event.type = ledger::EventType::ORDER_STATE;
        event.source = ledger::Source::LOCAL;
        event.wall_ns = ledger::now_wall_ns();
        event.venue_ts_ms = wire.timestamp_ms;
        event.add_str(ledger::F_ORDER_ID, wire.ticket_id);
        // The venue-keyed record now owns the lifecycle; the ticket is retired so
        // exposure is never counted twice.
        event.add_u8(ledger::F_STATE,
                     static_cast<uint8_t>(venue_state == OrderState::UNKNOWN
                                              ? OrderState::UNKNOWN
                                              : OrderState::SUPERSEDED));
        event.add_str(ledger::F_REASON, "linked_to_venue_order");
        ledger::compute_event_key(event.type, event.source, wire.ticket_id, "",
                                  wire.timestamp_ms, wire.salt + 3, event.key);
        if (!ledger_.commit_locked(event, error, cap)) {
            write_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ledger::Event release{};
        release.type = ledger::EventType::RISK_RELEASE;
        release.source = ledger::Source::LOCAL;
        release.wall_ns = ledger::now_wall_ns();
        release.add_str(ledger::F_ORDER_ID, wire.ticket_id);
        ledger::compute_event_key(release.type, release.source, wire.ticket_id, "", 0,
                                  wire.salt + 4, release.key);
        if (!ledger_.commit_locked(release, error, cap)) {
            write_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // POST /order returns the trade ids created at placement.  A FAK that
    // partially fills produces real exposure immediately, so it is recorded
    // here rather than waiting for the user stream.
    // Caller holds ledger_.guard().
    void record_response_trades(const WireOrder& wire,
                                const clob::OrderPostResult& detail, char* error,
                                size_t cap) {
        for (size_t i = 0; i < detail.trade_id_count; ++i) {
            ledger::Event event{};
            event.type = ledger::EventType::FILL;
            event.source = ledger::Source::REST;
            event.wall_ns = ledger::now_wall_ns();
            event.venue_ts_ms = wire.timestamp_ms;
            event.add_str(ledger::F_TRADE_ID, detail.trade_ids[i]);
            event.add_str(ledger::F_TOKEN_ID, wire.token_id);
            if (config_.condition_id[0])
                event.add_str(ledger::F_CONDITION_ID, config_.condition_id);
            event.add_u8(ledger::F_SIDE, wire.side);
            // The response does not split the fill per trade id; record it as
            // matched-not-broadcast and let reconciliation confirm the size.
            event.add_u8(
                ledger::F_RESULT,
                static_cast<uint8_t>(venue_status::TradeKind::MATCHED_NOT_BROADCASTED));
            event.add_u64(ledger::F_PRICE_RAW, price_raw(wire));
            event.add_u64(ledger::F_SIZE_RAW,
                          detail.trade_id_count ? detail.taking_amount /
                                                      detail.trade_id_count
                                                : 0);
            event.add_str(ledger::F_REASON, "from_post_order_response");
            ledger::compute_event_key(event.type, event.source, "", detail.trade_ids[i],
                                      wire.timestamp_ms, i, event.key);
            (void)ledger_.commit_locked(event, error, cap);
        }
    }

    ledger::EventLedger& ledger_;
    const MarketConfig& config_;
    std::atomic<uint64_t> tickets_{0};
    std::atomic<uint64_t> ambiguous_results_{0};
    std::atomic<uint64_t> write_failures_{0};
    std::atomic<bool> order_id_matches_digest_{false};
};

}  // namespace recorder

#endif  // ORDER_RECORDER_HPP
