#ifndef METADATA_PIPELINE_HPP
#define METADATA_PIPELINE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Market metadata pipeline (Phase 1): identity → metadata → status validation →
// book snapshot → runtime publication → trading may be enabled.
//
// Order of operations is deliberate and fail-closed at every step:
//   1. Resolve the condition id (explicit, or Gamma by slug, or CLOB by token).
//   2. Read the market from Gamma (status, tokens, min size, tick, fee schedule)
//      and from the CLOB (/clob-markets: nr, mts, tokens, fd).
//   3. Read the per-token CLOB endpoints (/tick-size, /neg-risk) and the book
//      (/book) — the book is also the source of min_order_size and the depth
//      snapshot used to seed the local L2 book.
//   4. Cross-check every overlapping value.  Any disagreement, gap or
//      unsupported value fails the pipeline; nothing is defaulted.
//   5. Validate the market state (active, not closed, not archived, accepting
//      orders, order book enabled, not restricted).
//   6. Publish to MarketRuntime atomically; only then may the hot path trade.
//
// No hardcoded tick size, minimum order size, fee rate, negative-risk flag,
// token id or market status survives this pipeline: the environment values are
// used only as *expectations* to cross-check the venue (and as fixtures in
// replay/paper mode, where there is no venue).
// ─────────────────────────────────────────────────────────────────────────────

#include <strings.h>
#include <time.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/json_scan.hpp"
#include "../include/venue_metadata.hpp"
#include "clob_rest_client.hpp"

namespace venue {

struct PipelineOptions {
    bool use_gamma = true;
    bool use_clob_market = true;
    bool use_token_endpoints = true;
    bool require_book = true;
    bool require_status = true;
};

struct PipelineResult {
    MarketMetadata metadata{};
    BookSnapshot book{};
    MetadataReport report{};
    bool book_valid = false;
    uint64_t server_time_s = 0;
    char resolved_token_id[80]{};
    char detail[192]{};
};

class MetadataPipeline {
public:
    MetadataPipeline(clob::ClobApiClient& api, const PipelineOptions& options)
        : api_(api), options_(options) {}

    // Runs the whole pipeline.  Returns true only when every requested source
    // answered, all values agree and the market validates as tradable.
    bool run(const char* slug, const char* condition_id, const char* token_id,
             const MetadataPolicy& policy, PipelineResult& result) {
        result = PipelineResult{};
        clob::CallResult call{};

        // 1. Condition id.
        char resolved_condition[70]{};
        if (condition_id && condition_id[0]) {
            if (!json_scan::is_hex_bytes(condition_id, std::strlen(condition_id), 32)) {
                fail(result, "condition_id_malformed");
                return false;
            }
            std::snprintf(resolved_condition, sizeof(resolved_condition), "%s",
                          condition_id);
        } else if (token_id && token_id[0]) {
            if (!api_.market_by_token(token_id, resolved_condition,
                                      sizeof(resolved_condition), call)) {
                fail(result, call.ambiguous ? "condition_lookup_ambiguous"
                                            : "condition_lookup_failed");
                return false;
            }
        } else if (slug && slug[0] && options_.use_gamma) {
            MarketMetadata gamma_metadata{};
            gamma_metadata.reset();
            if (!api_.gamma_market_by_slug(slug, gamma_metadata, call)) {
                fail(result, "gamma_slug_lookup_failed");
                return false;
            }
            std::snprintf(resolved_condition, sizeof(resolved_condition), "%s",
                          gamma_metadata.condition_id);
            merge(result.metadata, gamma_metadata, MetadataSource::GAMMA);
        } else {
            fail(result, "no_market_identity");
            return false;
        }
        std::snprintf(result.metadata.condition_id,
                      sizeof(result.metadata.condition_id), "%s", resolved_condition);

        // 2. Gamma market (status is authoritative for tradability).
        if (options_.use_gamma && !result.metadata.saw(MetadataSource::GAMMA)) {
            MarketMetadata gamma_metadata{};
            gamma_metadata.reset();
            std::snprintf(gamma_metadata.condition_id,
                          sizeof(gamma_metadata.condition_id), "%s",
                          resolved_condition);
            if (!api_.gamma_market_by_condition(resolved_condition, gamma_metadata,
                                                call)) {
                fail(result, "gamma_market_failed");
                return false;
            }
            if (!merge(result.metadata, gamma_metadata, MetadataSource::GAMMA)) {
                fail(result, "gamma_metadata_disagrees");
                return false;
            }
        }

        // 3. CLOB market (compact trading parameters).
        if (options_.use_clob_market) {
            MarketMetadata clob_metadata{};
            clob_metadata.reset();
            std::snprintf(clob_metadata.condition_id,
                          sizeof(clob_metadata.condition_id), "%s",
                          resolved_condition);
            if (!api_.clob_market(resolved_condition, clob_metadata, call)) {
                fail(result, "clob_market_failed");
                return false;
            }
            if (!merge(result.metadata, clob_metadata, MetadataSource::CLOB_MARKET)) {
                fail(result, "clob_metadata_disagrees");
                return false;
            }
        }

        // 4. Token selection: explicit, otherwise the market's first token.
        const char* selected = token_id && token_id[0] ? token_id : nullptr;
        if (!selected) {
            if (result.metadata.token_count == 0) {
                fail(result, "token_unresolved");
                return false;
            }
            selected = result.metadata.tokens[0];
        }
        if (!json_scan::is_decimal_integer(selected, std::strlen(selected))) {
            fail(result, "token_id_malformed");
            return false;
        }
        std::snprintf(result.resolved_token_id, sizeof(result.resolved_token_id),
                      "%s", selected);
        std::snprintf(result.metadata.token_id, sizeof(result.metadata.token_id),
                      "%s", selected);

        // 5. Per-token endpoints.
        if (options_.use_token_endpoints) {
            uint64_t tick = 0;
            if (!api_.tick_size(selected, tick, call)) {
                fail(result, "tick_size_query_failed");
                return false;
            }
            if (result.metadata.tick_raw && result.metadata.tick_raw != tick) {
                fail(result, "tick_size_disagreement");
                return false;
            }
            result.metadata.tick_raw = tick;
            result.metadata.mark_source(MetadataSource::TICK_SIZE);

            bool neg_risk = false;
            if (!api_.neg_risk(selected, neg_risk, call)) {
                fail(result, "neg_risk_query_failed");
                return false;
            }
            if (result.metadata.saw(MetadataSource::CLOB_MARKET) ||
                result.metadata.saw(MetadataSource::GAMMA)) {
                if (result.metadata.neg_risk != neg_risk) {
                    fail(result, "neg_risk_disagreement");
                    return false;
                }
            }
            result.metadata.neg_risk = neg_risk;
            result.metadata.mark_source(MetadataSource::NEG_RISK);

            uint64_t bps = 0;
            if (api_.fee_rate_bps(selected, bps, call))
                result.metadata.fee_rate_bps = bps;  // audit value only
        }

        // 6. Book snapshot (also seeds depth and re-checks the parameters).
        if (options_.require_book) {
            if (!api_.book(selected, result.book, call)) {
                fail(result, "book_query_failed");
                return false;
            }
            result.book_valid = true;
            if (result.book.asset_id[0] &&
                std::strcmp(result.book.asset_id, selected) != 0) {
                fail(result, "book_token_mismatch");
                return false;
            }
            if (result.book.market[0] &&
                strcasecmp(result.book.market, resolved_condition) != 0) {
                fail(result, "book_market_mismatch");
                return false;
            }
            if (result.book.tick_valid) {
                if (result.metadata.tick_raw &&
                    result.metadata.tick_raw != result.book.tick_raw) {
                    fail(result, "book_tick_disagreement");
                    return false;
                }
                result.metadata.tick_raw = result.book.tick_raw;
            }
            if (result.book.min_order_size_valid) {
                if (result.metadata.min_size_raw &&
                    result.metadata.min_size_raw != result.book.min_order_size_raw) {
                    fail(result, "min_order_size_disagreement");
                    return false;
                }
                result.metadata.min_size_raw = result.book.min_order_size_raw;
            }
            if (result.book.neg_risk_valid &&
                result.metadata.neg_risk != result.book.neg_risk) {
                fail(result, "book_neg_risk_disagreement");
                return false;
            }
            result.metadata.mark_source(MetadataSource::BOOK);
            std::snprintf(result.metadata.book_hash,
                          sizeof(result.metadata.book_hash), "%s", result.book.hash);
            result.metadata.book_timestamp_ms = result.book.timestamp_ms;
            result.metadata.last_trade_price_raw = result.book.last_trade_price_raw;
        }

        if (result.metadata.token_count == 0 && !result.metadata.add_token(selected)) {
            fail(result, "token_set_unavailable");
            return false;
        }

        // 7. Server time for the freshness budget.
        (void)api_.server_time(result.server_time_s, call);

        // 8. Validation.  Without a book or without status the market is not
        //    tradable, so the policy defaults are the strict ones.
        MetadataPolicy effective = policy;
        effective.require_gamma_status = options_.require_status;
        validate_metadata(result.metadata, effective, result.report);
        if (!result.report.ok) {
            result.report.format(result.detail, sizeof(result.detail));
            return false;
        }
        result.metadata.observed_wall_ns = ledger_now_wall_ns();
        std::snprintf(result.detail, sizeof(result.detail), "metadata_validated");
        return true;
    }

private:
    static uint64_t ledger_now_wall_ns() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    static void fail(PipelineResult& result, const char* reason) noexcept {
        result.report.reset();
        result.report.fail(reason);
        std::snprintf(result.detail, sizeof(result.detail), "%s", reason);
    }

    // Merges a source snapshot into the aggregate.  Returns false when two
    // sources disagree about a value they both report.
    static bool merge(MarketMetadata& target, const MarketMetadata& source,
                      MetadataSource origin) noexcept {
        if (source.condition_id[0] && target.condition_id[0] &&
            strcasecmp(source.condition_id, target.condition_id) != 0)
            return false;
        if (source.condition_id[0])
            std::snprintf(target.condition_id, sizeof(target.condition_id), "%s",
                          source.condition_id);
        if (source.tick_raw) {
            if (target.tick_raw && target.tick_raw != source.tick_raw) return false;
            target.tick_raw = source.tick_raw;
        }
        if (source.min_size_raw) {
            if (target.min_size_raw && target.min_size_raw != source.min_size_raw)
                return false;
            target.min_size_raw = source.min_size_raw;
        }
        const bool source_has_neg_risk =
            source.saw(MetadataSource::CLOB_MARKET) ||
            source.saw(MetadataSource::GAMMA) || source.saw(MetadataSource::BOOK) ||
            source.saw(MetadataSource::NEG_RISK);
        if (source_has_neg_risk) {
            const bool target_has_neg_risk =
                target.saw(MetadataSource::CLOB_MARKET) ||
                target.saw(MetadataSource::GAMMA) ||
                target.saw(MetadataSource::BOOK) ||
                target.saw(MetadataSource::NEG_RISK);
            if (target_has_neg_risk && target.neg_risk != source.neg_risk) return false;
            target.neg_risk = source.neg_risk;
        }
        if (source.fee_rate_micro) {
            if (target.fee_rate_micro && target.fee_rate_micro != source.fee_rate_micro)
                return false;
            target.fee_rate_micro = source.fee_rate_micro;
        }
        if (source.fee_exponent_micro) {
            if (target.fee_exponent_micro &&
                target.fee_exponent_micro != source.fee_exponent_micro)
                return false;
            target.fee_exponent_micro = source.fee_exponent_micro;
        }
        if (source.maker_rebate_micro) target.maker_rebate_micro = source.maker_rebate_micro;
        if (source.status_known) {
            target.active = source.active;
            target.closed = source.closed;
            target.archived = source.archived;
            target.accepting_orders = source.accepting_orders;
            target.enable_order_book = source.enable_order_book;
            target.restricted = source.restricted;
            target.status_known = true;
            target.seconds_delay = source.seconds_delay;
        }
        if (source.fees_enabled) target.fees_enabled = true;
        for (size_t i = 0; i < source.token_count; ++i)
            if (!target.add_token(source.tokens[i])) return false;
        target.mark_source(origin);
        return true;
    }

    clob::ClobApiClient& api_;
    PipelineOptions options_{};
};

}  // namespace venue

#endif  // METADATA_PIPELINE_HPP
