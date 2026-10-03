#ifndef MARKET_METADATA_HPP
#define MARKET_METADATA_HPP

// Phase 1 — dynamic market metadata.
//
// Venue parameters that the venue can change (tick size, minimum order size,
// fee rate/exponent, negative-risk flag, outcome token, market status) are never
// taken from ENV/config/hardcoded defaults in a live path. They are resolved
// from the documented public REST surface at startup and validated against each
// other, cross-source, before any order can be constructed.
//
// Verified sources (docs.polymarket.com):
//   * GET https://gamma-api.polymarket.com/markets/slug/{slug}      (status, tokens, negRisk)
//   * GET https://gamma-api.polymarket.com/markets?condition_ids=…  (same, by condition)
//   * GET https://clob.polymarket.com/clob-markets/{condition_id}   (mts, mos, tbf, fd{r,e,to}, t[])
//   * GET https://clob.polymarket.com/tick-size?token_id=…          (minimum_tick_size)
//   * GET https://clob.polymarket.com/fee-rate?token_id=…           (base_fee, bps)
//   * GET https://clob.polymarket.com/book?token_id=…               (tick_size, min_order_size, neg_risk)
//   * GET https://clob.polymarket.com/time                          (server clock)
//   * GET https://clob.polymarket.com/ok                            (health)
//
// Every parser below is a pure function over a bounded byte range so it can be
// exercised with recorded fixtures and rejects duplicate/absent/ambiguous fields.
//
// Acronyms: CLOB = Central Limit Order Book; bps = basis points (1/10000);
// EIP-712 = Ethereum typed structured data signing; TLS = Transport Layer
// Security; WSS = WebSocket Secure; RPC = Remote Procedure Call; ENV = process
// environment variables; TOML = Tom's Obvious Minimal Language.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "json_field.hpp"
#include "polymarket_order.hpp"

namespace crowdintel {

// ── Documented contract addresses (docs.polymarket.com/resources/contracts) ──
// Kept identical to the signer's constants; a mismatch would produce a valid
// signature for the wrong verifying contract.
inline constexpr uint8_t K_STANDARD_EXCHANGE_ADDRESS[20] = {
    0xE1, 0x11, 0x18, 0x00, 0x00, 0xd2, 0x66, 0x3C, 0x00, 0x91,
    0xe4, 0xf4, 0x00, 0x23, 0x75, 0x45, 0xB8, 0x7B, 0x99, 0x6B};
inline constexpr uint8_t K_NEG_RISK_EXCHANGE_ADDRESS[20] = {
    0xe2, 0x22, 0x2d, 0x27, 0x9d, 0x74, 0x40, 0x50, 0xd2, 0x8e,
    0x00, 0x52, 0x00, 0x10, 0x52, 0x00, 0x00, 0x31, 0x0F, 0x59};

inline constexpr size_t K_CONDITION_ID_CHARS = 67;  // 0x + 64 hex + NUL
inline constexpr size_t K_TOKEN_ID_CHARS = 79;      // up to 78 digits + NUL

struct MarketRuntime {
    bool resolved = false;

    char condition_id[K_CONDITION_ID_CHARS]{};  // 0x + 64 lowercase hex
    char token_id_dec[K_TOKEN_ID_CHARS]{};      // decimal uint256 (asset id)
    uint8_t token_id_be[32]{};
    char outcome_label[32]{};                   // venue label, e.g. "Yes"
    char market_slug[96]{};

    uint64_t tick_size = 0;        // price increment, fixed point 1e-6
    uint64_t min_order_size = 0;   // minimum order size, fixed point 1e-6 shares
    double fee_rate = 0.0;         // taker fee coefficient (fraction, not bps)
    double fee_exponent = 1.0;     // fee curve exponent applied to p*(1-p)
    bool fee_taker_only = true;
    bool fee_from_fee_details = false;  // true: fd{r,e}; false: tbf bps, e = 1

    bool neg_risk = false;
    bool accepting_orders = false;
    bool active = false;
    bool closed = true;
    bool enable_order_book = false;
    bool archived = false;
    bool restricted = false;

    uint8_t exchange[20]{};        // selected by neg_risk
    int64_t clock_offset_ms = 0;   // venue wall clock minus local wall clock
    uint64_t resolved_unix_ms = 0;

    // Provenance: which source supplied each venue-mutable value.
    char tick_source[24]{};
    char min_size_source[24]{};
    char fee_source[24]{};
};

enum class ResolutionIssue : uint8_t {
    NONE = 0,
    MALFORMED_JSON,
    MISSING_FIELD,
    DUPLICATE_FIELD,
    IDENTITY_MISMATCH,
    TOKEN_NOT_FOUND,
    OUTCOME_NOT_FOUND,
    MARKET_NOT_TRADING,
    TICK_MISMATCH,
    TICK_UNSUPPORTED,
    MIN_SIZE_MISMATCH,
    NEG_RISK_MISMATCH,
    FEE_UNAVAILABLE,
    FEE_OUT_OF_RANGE,
    CLOCK_SKEW,
    EXCHANGE_MISMATCH,
};

inline const char* resolution_issue_name(ResolutionIssue issue) noexcept {
    switch (issue) {
        case ResolutionIssue::NONE: return "none";
        case ResolutionIssue::MALFORMED_JSON: return "malformed_json";
        case ResolutionIssue::MISSING_FIELD: return "missing_field";
        case ResolutionIssue::DUPLICATE_FIELD: return "duplicate_field";
        case ResolutionIssue::IDENTITY_MISMATCH: return "identity_mismatch";
        case ResolutionIssue::TOKEN_NOT_FOUND: return "token_not_found";
        case ResolutionIssue::OUTCOME_NOT_FOUND: return "outcome_not_found";
        case ResolutionIssue::MARKET_NOT_TRADING: return "market_not_trading";
        case ResolutionIssue::TICK_MISMATCH: return "tick_mismatch";
        case ResolutionIssue::TICK_UNSUPPORTED: return "tick_unsupported";
        case ResolutionIssue::MIN_SIZE_MISMATCH: return "min_size_mismatch";
        case ResolutionIssue::NEG_RISK_MISMATCH: return "neg_risk_mismatch";
        case ResolutionIssue::FEE_UNAVAILABLE: return "fee_unavailable";
        case ResolutionIssue::FEE_OUT_OF_RANGE: return "fee_out_of_range";
        case ResolutionIssue::CLOCK_SKEW: return "clock_skew";
        case ResolutionIssue::EXCHANGE_MISMATCH: return "exchange_mismatch";
    }
    return "unknown";
}

struct MarketResolution {
    ResolutionIssue issue = ResolutionIssue::MALFORMED_JSON;
    char detail[160]{};
    MarketRuntime runtime{};
    bool ok() const noexcept { return issue == ResolutionIssue::NONE; }
};

// ── Pure parsers ─────────────────────────────────────────────────────────────

inline bool is_lower_hex_id(const char* text, size_t hex_chars) noexcept {
    if (!text) return false;
    if (std::strlen(text) != hex_chars) return false;
    for (size_t i = 0; i < hex_chars; ++i) {
        const char c = text[i];
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

inline bool valid_condition_id(const char* text) noexcept {
    return text && std::strncmp(text, "0x", 2) == 0 &&
           is_lower_hex_id(text + 2, 64);
}

// Decimal uint256 as produced by the venue (no sign, no leading zeros).
inline bool valid_token_id(const char* text) noexcept {
    if (!text || !*text) return false;
    const size_t length = std::strlen(text);
    if (length == 0 || length > 78) return false;
    if (length > 1 && text[0] == '0') return false;
    for (size_t i = 0; i < length; ++i)
        if (text[i] < '0' || text[i] > '9') return false;
    return true;
}

inline bool equals_ascii_ci(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 32);
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

struct GammaMarketView {
    bool parsed = false;
    char condition_id[K_CONDITION_ID_CHARS]{};
    char slug[96]{};
    char token_ids[2][K_TOKEN_ID_CHARS]{};
    char outcome_labels[2][32]{};
    size_t token_count = 0;
    bool active = false;
    bool closed = true;
    bool accepting_orders = false;
    bool enable_order_book = false;
    bool archived = false;
    bool restricted = false;
    bool has_neg_risk = false;
    bool neg_risk = false;
    bool has_tick = false;
    uint64_t tick_size = 0;
    bool has_min_size = false;
    uint64_t min_order_size = 0;
};

// Gamma returns outcome labels/token ids as JSON-encoded arrays inside strings:
//   "outcomes": "[\"Yes\", \"No\"]", "clobTokenIds": "[\"7132…\", \"5211…\"]"
inline bool parse_gamma_market(const char* json, size_t length,
                               GammaMarketView& out) noexcept {
    out = GammaMarketView{};
    if (!json || length == 0 || length > 262144) return false;
    if (!bounded_json::valid_document(json, length)) return false;
    const char* ignored = nullptr;
    if (json_field::locate(json, length, "conditionId", &ignored) != 1 ||
        json_field::locate(json, length, "outcomes", &ignored) != 1 ||
        json_field::locate(json, length, "clobTokenIds", &ignored) != 1)
        return false;
    if (!json_field::string(json, length, "conditionId", out.condition_id,
                            sizeof(out.condition_id)))
        return false;
    if (!valid_condition_id(out.condition_id)) return false;

    char labels[2][32]{};
    char tokens[2][K_TOKEN_ID_CHARS]{};
    const size_t label_count = json_field::embedded_string_array<32>(
        json, length, "outcomes", &labels[0][0], 2);
    const size_t token_count = json_field::embedded_string_array<K_TOKEN_ID_CHARS>(
        json, length, "clobTokenIds", &tokens[0][0], 2);
    if (label_count == 0 || label_count != token_count || token_count > 2)
        return false;
    for (size_t i = 0; i < token_count; ++i) {
        if (!valid_token_id(tokens[i]) || labels[i][0] == '\0') return false;
        std::memcpy(out.token_ids[i], tokens[i], std::strlen(tokens[i]) + 1);
        std::memcpy(out.outcome_labels[i], labels[i], std::strlen(labels[i]) + 1);
    }
    out.token_count = token_count;

    // Status flags are required: an absent flag is not "false by default".
    bool flag = false;
    if (!json_field::boolean(json, length, "active", flag)) return false;
    out.active = flag;
    if (!json_field::boolean(json, length, "closed", flag)) return false;
    out.closed = flag;
    if (!json_field::boolean(json, length, "acceptingOrders", flag)) return false;
    out.accepting_orders = flag;
    if (!json_field::boolean(json, length, "enableOrderBook", flag)) return false;
    out.enable_order_book = flag;
    if (json_field::boolean(json, length, "archived", flag)) out.archived = flag;
    if (json_field::boolean(json, length, "restricted", flag)) out.restricted = flag;
    if (json_field::boolean(json, length, "negRisk", flag)) {
        out.has_neg_risk = true;
        out.neg_risk = flag;
    }
    if (json_field::string(json, length, "slug", out.slug, sizeof(out.slug)) &&
        out.slug[0] == '\0')
        return false;

    uint64_t fixed = 0;
    if (json_field::fixed6(json, length, "orderPriceMinTickSize", fixed) && fixed)
        { out.has_tick = true; out.tick_size = fixed; }
    if (json_field::fixed6(json, length, "orderMinSize", fixed) && fixed)
        { out.has_min_size = true; out.min_order_size = fixed; }
    out.parsed = true;
    return true;
}

// Gamma returns a JSON array for /markets?condition_ids=...; the first element
// is parsed with the same object parser. An empty array is a hard failure.
inline bool parse_gamma_market_list(const char* json, size_t length,
                                    GammaMarketView& out) noexcept {
    out = GammaMarketView{};
    if (!json || length == 0 || length > 1048576) return false;
    if (!bounded_json::valid_document(json, length)) return false;
    const char* p = json;
    const char* end = json + length;
    while (p < end && json_field::is_space(*p)) ++p;
    if (p == end || *p != '[') return false;
    ++p;
    while (p < end && json_field::is_space(*p)) ++p;
    if (p == end || *p != '{') return false;
    const char* element_begin = p;
    if (!json_field::skip_value(p, end)) return false;
    const json_field::Span element{element_begin, p};
    return parse_gamma_market(element.begin, element.size(), out);
}

struct ClobMarketDetailsView {
    struct Token {
        char token_id[K_TOKEN_ID_CHARS]{};
        char outcome[32]{};
    };
    bool parsed = false;
    Token tokens[4]{};
    size_t token_count = 0;
    bool has_tick = false;
    uint64_t tick_size = 0;
    bool has_min_size = false;
    uint64_t min_order_size = 0;
    bool has_maker_fee = false;
    bool has_taker_fee = false;
    double maker_fee_bps = 0.0;
    double taker_fee_bps = 0.0;
    bool has_fee_details = false;
    double fee_rate = 0.0;
    double fee_exponent = 1.0;
    bool fee_taker_only = true;
    bool rfq_enabled = false;
};

inline bool parse_clob_market_details(const char* json, size_t length,
                                      ClobMarketDetailsView& out) noexcept {
    out = ClobMarketDetailsView{};
    if (!json || length == 0 || length > 262144) return false;
    if (!bounded_json::valid_document(json, length)) return false;

    uint64_t fixed = 0;
    if (json_field::fixed6(json, length, "mts", fixed)) {
        if (fixed == 0) return false;
        out.has_tick = true;
        out.tick_size = fixed;
    }
    if (json_field::fixed6(json, length, "mos", fixed)) {
        if (fixed == 0) return false;
        out.has_min_size = true;
        out.min_order_size = fixed;
    }
    double number = 0.0;
    if (json_field::number(json, length, "mbf", number)) {
        if (!(number >= 0.0 && number <= 10000.0)) return false;
        out.has_maker_fee = true;
        out.maker_fee_bps = number;
    }
    if (json_field::number(json, length, "tbf", number)) {
        if (!(number >= 0.0 && number <= 10000.0)) return false;
        out.has_taker_fee = true;
        out.taker_fee_bps = number;
    }
    if (json_field::boolean(json, length, "rfqe", out.rfq_enabled) == false)
        out.rfq_enabled = false;

    // Token list: [{"t":"<token id>","o":"<outcome label>"}, ...]
    json_field::Span array;
    if (json_field::unique(json, length, "t", array)) {
        if (array.size() < 2 || *array.begin != '[' || array.end[-1] != ']')
            return false;
        const char* p = array.begin + 1;
        const char* end = array.end - 1;
        while (p < end) {
            while (p < end && json_field::is_space(*p)) ++p;
            if (p == end) break;
            if (out.token_count >= 4 || *p != '{') return false;
            const char* element_begin = p;
            if (!json_field::skip_value(p, end)) return false;
            const json_field::Span element{element_begin, p};
            char token[K_TOKEN_ID_CHARS]{};
            char outcome[32]{};
            // Nested fields are unique within the element document.
            if (!json_field::string(element.begin, element.size(), "t", token,
                                    sizeof(token)) ||
                !json_field::string(element.begin, element.size(), "o", outcome,
                                    sizeof(outcome)))
                return false;
            if (!valid_token_id(token) || outcome[0] == '\0') return false;
            ClobMarketDetailsView::Token& slot = out.tokens[out.token_count];
            std::memcpy(slot.token_id, token, std::strlen(token) + 1);
            std::memcpy(slot.outcome, outcome, std::strlen(outcome) + 1);
            ++out.token_count;
            while (p < end && json_field::is_space(*p)) ++p;
            if (p < end) {
                if (*p != ',') return false;
                ++p;
            }
        }
    }

    // Fee details: fd = {r: rate, e: exponent, to: takerOnly}.
    json_field::Span fd;
    if (json_field::unique(json, length, "fd", fd)) {
        if (fd.size() < 2 || *fd.begin != '{' || fd.end[-1] != '}')
            return false;
        const char* begin = fd.begin;
        const size_t size = fd.size();
        double rate = 0.0;
        if (json_field::number(begin, size, "r", rate)) {
            if (!(rate >= 0.0 && rate <= 1.0)) return false;
            out.has_fee_details = true;
            out.fee_rate = rate;
        }
        double exponent = 0.0;
        if (json_field::number(begin, size, "e", exponent)) {
            if (!(exponent >= 0.0 && exponent <= 8.0)) return false;
            out.fee_exponent = exponent;
        } else {
            out.fee_exponent = 1.0;  // published curve when the venue omits it
        }
        bool taker_only = false;
        if (json_field::boolean(begin, size, "to", taker_only))
            out.fee_taker_only = taker_only;
    }
    out.parsed = true;
    return true;
}

struct BookView {
    bool parsed = false;
    char market[K_CONDITION_ID_CHARS]{};
    char asset_id[K_TOKEN_ID_CHARS]{};
    bool has_tick = false;
    uint64_t tick_size = 0;
    bool has_min_size = false;
    uint64_t min_order_size = 0;
    bool has_neg_risk = false;
    bool neg_risk = false;
};

inline bool parse_book_summary(const char* json, size_t length,
                               BookView& out) noexcept {
    out = BookView{};
    if (!json || length == 0 || length > 1048576) return false;
    if (!bounded_json::valid_document(json, length)) return false;
    if (!json_field::string(json, length, "market", out.market, sizeof(out.market)) ||
        !json_field::string(json, length, "asset_id", out.asset_id,
                            sizeof(out.asset_id)))
        return false;
    if (!valid_condition_id(out.market) || !valid_token_id(out.asset_id))
        return false;
    uint64_t fixed = 0;
    if (json_field::fixed6(json, length, "tick_size", fixed) && fixed) {
        out.has_tick = true;
        out.tick_size = fixed;
    }
    if (json_field::fixed6(json, length, "min_order_size", fixed) && fixed) {
        out.has_min_size = true;
        out.min_order_size = fixed;
    }
    bool flag = false;
    if (json_field::boolean(json, length, "neg_risk", flag)) {
        out.has_neg_risk = true;
        out.neg_risk = flag;
    }
    out.parsed = true;
    return true;
}

inline bool parse_tick_size_response(const char* json, size_t length,
                                     uint64_t& tick_out) noexcept {
    tick_out = 0;
    if (!json || length == 0 || length > 4096) return false;
    if (!bounded_json::valid_document(json, length)) return false;
    return json_field::fixed6(json, length, "minimum_tick_size", tick_out) &&
           tick_out != 0;
}

inline bool parse_fee_rate_response(const char* json, size_t length,
                                    double& base_fee_bps_out) noexcept {
    base_fee_bps_out = 0.0;
    if (!json || length == 0 || length > 4096) return false;
    if (!bounded_json::valid_document(json, length)) return false;
    if (!json_field::number(json, length, "base_fee", base_fee_bps_out))
        return false;
    return base_fee_bps_out >= 0.0 && base_fee_bps_out <= 10000.0;
}

// /time returns the venue Unix timestamp; tolerate a bare number or an object
// with a "time" field, in seconds or milliseconds. Returns seconds.
inline bool parse_server_time(const char* json, size_t length,
                              int64_t& unix_seconds_out) noexcept {
    unix_seconds_out = 0;
    if (!json || length == 0 || length > 4096) return false;
    if (!bounded_json::valid_document(json, length)) return false;
    double value = 0.0;
    bool integral = false;
    if (!json_field::number(json, length, "time", value, &integral)) {
        const char* p = json;
        const char* end = json + length;
        while (p < end && json_field::is_space(*p)) ++p;
        json_field::Span bare{p, end};
        if (!json_field::parse_number(bare, value, &integral)) return false;
    }
    if (!integral || !std::isfinite(value) || value <= 0.0) return false;
    if (value >= 1e12) value /= 1000.0;  // milliseconds
    if (value < 1e9 || value > 1e10) return false;
    unix_seconds_out = static_cast<int64_t>(value);
    return true;
}

// ── Cross-source validation ──────────────────────────────────────────────────

struct MarketResolutionInputs {
    const GammaMarketView* gamma = nullptr;
    const ClobMarketDetailsView* clob = nullptr;
    const BookView* book = nullptr;
    const uint64_t* tick_size_probe = nullptr;   // GET /tick-size
    const double* fee_rate_probe = nullptr;      // GET /fee-rate (bps)
    const int64_t* server_time_seconds = nullptr;
    int64_t local_time_seconds = 0;
    int64_t max_clock_offset_ms = 2000;
    // Operator selection. `requested_outcome` matches the venue label
    // case-insensitively; when null the market must expose exactly one token.
    const char* requested_outcome = nullptr;
};

inline void set_detail(MarketResolution& result, const char* text) noexcept {
    if (!text) { result.detail[0] = '\0'; return; }
    const size_t length = std::strlen(text);
    const size_t copied = length < sizeof(result.detail) - 1
                        ? length : sizeof(result.detail) - 1;
    std::memcpy(result.detail, text, copied);
    result.detail[copied] = '\0';
}

inline MarketResolution fail(ResolutionIssue issue, const char* detail) noexcept {
    MarketResolution result;
    result.issue = issue;
    set_detail(result, detail);
    return result;
}

// Validates every venue-controlled parameter across sources and produces the
// runtime market descriptor. Any absent/mismatched/ambiguous value is a hard
// failure: the caller must not trade.
inline MarketResolution resolve_market(const MarketResolutionInputs& in) noexcept {
    if (!in.gamma || !in.clob || !in.book || !in.tick_size_probe || !in.clob->parsed)
        return fail(ResolutionIssue::MISSING_FIELD, "missing source document");
    if (!in.gamma->parsed || !in.book->parsed)
        return fail(ResolutionIssue::MALFORMED_JSON, "unparsed source document");

    MarketRuntime runtime;

    // 1. Market identity: Gamma and the public book must name the same condition.
    if (!valid_condition_id(in.gamma->condition_id))
        return fail(ResolutionIssue::MISSING_FIELD, "gamma conditionId");
    if (std::strcmp(in.gamma->condition_id, in.book->market) != 0)
        return fail(ResolutionIssue::IDENTITY_MISMATCH,
                    "book.market != gamma.conditionId");
    std::memcpy(runtime.condition_id, in.gamma->condition_id,
                std::strlen(in.gamma->condition_id) + 1);
    if (in.gamma->slug[0]) {
        std::memcpy(runtime.market_slug, in.gamma->slug,
                    std::strlen(in.gamma->slug) + 1);
    }

    // 2. Trading status: all four flags are required and must be permissive.
    if (!in.gamma->active || in.gamma->closed || !in.gamma->accepting_orders ||
        !in.gamma->enable_order_book)
        return fail(ResolutionIssue::MARKET_NOT_TRADING,
                    "active/closed/acceptingOrders/enableOrderBook");
    if (in.gamma->archived || in.gamma->restricted)
        return fail(ResolutionIssue::MARKET_NOT_TRADING, "archived/restricted");
    runtime.active = in.gamma->active;
    runtime.closed = in.gamma->closed;
    runtime.accepting_orders = in.gamma->accepting_orders;
    runtime.enable_order_book = in.gamma->enable_order_book;
    runtime.archived = in.gamma->archived;
    runtime.restricted = in.gamma->restricted;

    // 3. Negative-risk flag: Gamma is authoritative for the group flag; the book
    //    echo must agree. No default is assumed when Gamma omits it.
    if (!in.gamma->has_neg_risk || !in.book->has_neg_risk)
        return fail(ResolutionIssue::MISSING_FIELD, "negRisk/neg_risk");
    if (in.gamma->neg_risk != in.book->neg_risk)
        return fail(ResolutionIssue::NEG_RISK_MISMATCH,
                    "gamma.negRisk != book.neg_risk");
    runtime.neg_risk = in.gamma->neg_risk;
    std::memcpy(runtime.exchange,
                runtime.neg_risk ? K_NEG_RISK_EXCHANGE_ADDRESS
                                 : K_STANDARD_EXCHANGE_ADDRESS,
                20);

    // 4. Outcome/token selection. The operator names the outcome (never a token
    //    id in live mode); the venue decides which uint256 is that outcome.
    char desired[32]{};
    if (in.requested_outcome && *in.requested_outcome) {
        const size_t n = std::strlen(in.requested_outcome);
        if (n >= sizeof(desired)) return fail(ResolutionIssue::OUTCOME_NOT_FOUND,
                                              "outcome label too long");
        std::memcpy(desired, in.requested_outcome, n + 1);
    } else if (in.gamma->token_count != 1) {
        return fail(ResolutionIssue::OUTCOME_NOT_FOUND,
                    "outcome label required for multi-outcome market");
    }

    bool found = false;
    for (size_t i = 0; i < in.gamma->token_count && !found; ++i) {
        if (desired[0] && !equals_ascii_ci(in.gamma->outcome_labels[i], desired))
            continue;
        for (size_t j = 0; j < in.clob->token_count; ++j) {
            if (std::strcmp(in.clob->tokens[j].token_id, in.gamma->token_ids[i]) != 0)
                continue;
            // clob-markets labels use the same outcome vocabulary as Gamma.
            if (in.clob->tokens[j].outcome[0] &&
                !equals_ascii_ci(in.clob->tokens[j].outcome,
                                 in.gamma->outcome_labels[i]))
                return fail(ResolutionIssue::IDENTITY_MISMATCH,
                            "clob token outcome != gamma outcome");
            std::memcpy(runtime.token_id_dec, in.clob->tokens[j].token_id,
                        std::strlen(in.clob->tokens[j].token_id) + 1);
            std::memcpy(runtime.outcome_label, in.gamma->outcome_labels[i],
                        std::strlen(in.gamma->outcome_labels[i]) + 1);
            found = true;
            break;
        }
    }
    if (!found) return fail(ResolutionIssue::TOKEN_NOT_FOUND,
                            "selected outcome has no matching CLOB token");
    if (std::strcmp(in.book->asset_id, runtime.token_id_dec) != 0)
        return fail(ResolutionIssue::IDENTITY_MISMATCH,
                    "book.asset_id != selected token");
    if (!parse_uint256_dec(runtime.token_id_dec,
                           std::strlen(runtime.token_id_dec),
                           runtime.token_id_be))
        return fail(ResolutionIssue::MISSING_FIELD, "token id is not uint256");

    // 5. Tick size: three documented sources must agree exactly, and the grid
    //    must be one this build can quantize (fail closed otherwise).
    if (!in.clob->has_tick || !in.book->has_tick)
        return fail(ResolutionIssue::MISSING_FIELD, "tick size missing");
    if (in.clob->tick_size != in.book->tick_size ||
        in.clob->tick_size != *in.tick_size_probe)
        return fail(ResolutionIssue::TICK_MISMATCH, "clob/book/tick-size disagree");
    if (amount_quantum_for_tick(in.clob->tick_size) == 0)
        return fail(ResolutionIssue::TICK_UNSUPPORTED, "venue grid not supported");
    runtime.tick_size = in.clob->tick_size;
    std::memcpy(runtime.tick_source, "clob/book/tick-size", 20);

    // 6. Minimum order size: clob-markets and book must agree; Gamma's
    //    orderMinSize, when present, is a third opinion.
    if (!in.clob->has_min_size || !in.book->has_min_size)
        return fail(ResolutionIssue::MISSING_FIELD, "min order size missing");
    if (in.clob->min_order_size != in.book->min_order_size)
        return fail(ResolutionIssue::MIN_SIZE_MISMATCH, "clob/book disagree");
    if (in.gamma->has_min_size &&
        in.gamma->min_order_size != in.clob->min_order_size)
        return fail(ResolutionIssue::MIN_SIZE_MISMATCH, "gamma disagrees");
    runtime.min_order_size = in.clob->min_order_size;
    std::memcpy(runtime.min_size_source, "clob/book", 10);

    // 7. Fee: prefer fd{r,e}; otherwise the base taker fee in bps with the
    //    published exponent 1. Both missing -> no order is constructible.
    if (in.clob->has_fee_details) {
        runtime.fee_rate = in.clob->fee_rate;
        runtime.fee_exponent = in.clob->fee_exponent;
        runtime.fee_taker_only = in.clob->fee_taker_only;
        runtime.fee_from_fee_details = true;
        std::memcpy(runtime.fee_source, "fd", 3);
        if (in.clob->has_taker_fee &&
            in.clob->taker_fee_bps / 10000.0 > runtime.fee_rate) {
            // The base taker fee is a floor; use the stricter value.
            runtime.fee_rate = in.clob->taker_fee_bps / 10000.0;
            runtime.fee_exponent = 1.0;
            runtime.fee_from_fee_details = false;
            std::memcpy(runtime.fee_source, "tbf", 4);
        }
    } else if (in.clob->has_taker_fee) {
        runtime.fee_rate = in.clob->taker_fee_bps / 10000.0;
        runtime.fee_exponent = 1.0;
        runtime.fee_taker_only = true;
        runtime.fee_from_fee_details = false;
        std::memcpy(runtime.fee_source, "tbf", 4);
    } else {
        return fail(ResolutionIssue::FEE_UNAVAILABLE, "no fd and no tbf");
    }
    if (!std::isfinite(runtime.fee_rate) || runtime.fee_rate < 0.0 ||
        runtime.fee_rate > 1.0)
        return fail(ResolutionIssue::FEE_OUT_OF_RANGE, "fee rate");
    if (!std::isfinite(runtime.fee_exponent) || runtime.fee_exponent <= 0.0 ||
        runtime.fee_exponent > 8.0)
        return fail(ResolutionIssue::FEE_OUT_OF_RANGE, "fee exponent");
    if (in.fee_rate_probe) {
        if (!(in.clob->has_taker_fee))
            return fail(ResolutionIssue::FEE_UNAVAILABLE, "tbf missing");
        const double probe_fraction = *in.fee_rate_probe / 10000.0;
        // The dedicated endpoint must not contradict the market document.
        if (std::fabs(probe_fraction - in.clob->taker_fee_bps / 10000.0) > 1e-9)
            return fail(ResolutionIssue::FEE_OUT_OF_RANGE, "/fee-rate disagrees");
    }

    // 8. Clock: the venue timestamp gates the L2 HMAC and WSS heartbeats.
    if (in.server_time_seconds) {
        if (in.local_time_seconds <= 0)
            return fail(ResolutionIssue::MISSING_FIELD, "local clock unavailable");
        const int64_t offset_ms =
            (*in.server_time_seconds - in.local_time_seconds) * 1000;
        if (in.max_clock_offset_ms <= 0 ||
            (offset_ms > in.max_clock_offset_ms ||
             offset_ms < -in.max_clock_offset_ms))
            return fail(ResolutionIssue::CLOCK_SKEW, "venue/local clock offset");
        runtime.clock_offset_ms = offset_ms;
    } else {
        return fail(ResolutionIssue::MISSING_FIELD, "server time missing");
    }

    runtime.resolved = true;
    MarketResolution result;
    result.issue = ResolutionIssue::NONE;
    result.runtime = runtime;
    set_detail(result, "resolved");
    return result;
}

inline void format_address_hex(const uint8_t address[20], char out[43]) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (size_t i = 0; i < 20; ++i) {
        out[2 + i * 2] = kHex[(address[i] >> 4) & 0xF];
        out[3 + i * 2] = kHex[address[i] & 0xF];
    }
    out[42] = '\0';
}

// Human-readable fee coefficient: fee per share = C × feeRate × p×(1-p)^exponent.
// Mirrors docs.polymarket.com/trading/fees (C in shares, p in price units).
inline double fee_per_share(const MarketRuntime& runtime, double price) noexcept {
    if (!(price > 0.0 && price < 1.0)) return 0.0;
    double component = price * (1.0 - price);
    if (runtime.fee_exponent != 1.0) {
        double powered = 1.0;
        const int steps = static_cast<int>(runtime.fee_exponent);
        for (int i = 0; i < steps; ++i) powered *= component;
        if (static_cast<double>(steps) != runtime.fee_exponent && component > 0.0)
            powered *= std::pow(component, runtime.fee_exponent - steps);
        component = powered;
    }
    const double fee = runtime.fee_rate * component;
    return fee > 0.0 ? fee : 0.0;
}

}  // namespace crowdintel

#endif  // MARKET_METADATA_HPP
