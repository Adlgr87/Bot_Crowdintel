#ifndef RECONCILIATION_HPP
#define RECONCILIATION_HPP

// Phase 5 — REST reconciliation and the READINESS gate.
//
// Why this exists: the journal is the only source of truth for orders that
// outlived a previous process, and the private channel can be disconnected,
// can lag, or can miss events. Before the bot may trade — at startup and after
// any loss of transport (channel reconnect, heartbeat critical, divergence) —
// the account state has to be re-proven against the venue with an
// authenticated query. Until then trading stays disabled (fail closed).
//
// Sources of truth for the wire contract (verified, not invented):
//   * docs.polymarket.com/api-reference/trade/get-user-orders   GET /data/orders
//   * docs.polymarket.com/api-reference/trade/get-single-order-by-id
//                                                              GET /data/order/{id}
//   * docs.polymarket.com/api-reference/trade/get-trades        GET /data/trades
//   * docs.polymarket.com/trading/manage-orders ("GET /auth/ban-status/closed-only")
//   * github.com/Polymarket/clob-client src/endpoints.ts (exact route strings)
//
// L2 signing for GET: the signed message is timestamp + "GET" + path, and the
// query string is NOT part of it. Evidence: the official client builds its
// headers with `requestPath: endpoint` and passes `params` separately to the
// HTTP layer (src/client.ts, isOrderScoring / getEarningsForUserForDay), and
// the docs' example message for GET /data/orders has no query either.
//
// What reconciliation does NOT do:
//   * it never guesses that an order was rejected because a request timed out;
//   * an order with no venue id cannot be looked up (the CLOB has no
//     client-order-id query) and therefore stays UNKNOWN and blocking;
//   * it never applies a fill to an order the journal already made terminal:
//     that contradiction is reported so a human inspects it instead.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "../crypto/secure_zero.hpp"
#include "clob_order_info.hpp"
#include "l2_auth.hpp"
#include "market_config.hpp"
#include "order_ledger.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK)
#include <curl/curl.h>
#include <mutex>
#endif

namespace recon {

enum class Result : uint8_t {
    kReady = 0,          // every journaled order is proven; venue adds nothing
    kBlocked = 1,        // proven divergence or missing evidence: stay out
    kTransportError = 2, // could not ask the venue: outcome is unknown
};

inline const char* result_name(Result result) noexcept {
    switch (result) {
        case Result::kReady: return "READY";
        case Result::kBlocked: return "BLOCKED";
        case Result::kTransportError: return "TRANSPORT_ERROR";
    }
    return "INVALID";
}

struct Report {
    uint64_t reconciliations = 0;
    uint64_t order_pages = 0;
    uint64_t trade_pages = 0;
    uint64_t live_orders = 0;
    uint64_t foreign_live_orders = 0;    // a market other than the tracked one
    uint64_t untracked_live_orders = 0;  // our market, absent from the journal
    uint64_t owner_mismatch = 0;         // venue attributed it to another key
    uint64_t terminal_conflicts = 0;     // terminal locally, still live at venue
    uint64_t orders_present = 0;
    uint64_t orders_canceled = 0;
    uint64_t orders_invalid = 0;
    uint64_t orders_absent = 0;
    uint64_t orders_open = 0;            // proven resting at the venue
    uint64_t orders_unqueryable = 0;     // no venue id: cannot be proven
    uint64_t unsafe_venue_id = 0;        // id that cannot go into a URL path
    uint64_t unknown_status = 0;
    uint64_t malformed_payloads = 0;
    uint64_t trades_seen = 0;
    uint64_t trades_foreign = 0;
    uint64_t unattributed_trades = 0;
    uint64_t truncated_maker_lists = 0;
    bool closed_only = false;
    bool pagination_limit = false;
};

// ── readiness gate ───────────────────────────────────────────────────────────

enum class ReadinessReason : uint8_t {
    kReady = 0,
    kMetadataUnresolved,
    kLedgerUnavailable,
    kReconciliationFailed,
    kUnreconciledOrders,
    kOpenOrdersPresent,
    kAccountStateUnproven,
    kClosedOnlyAccount,
    kUserChannelUnconfigured,
    kHeartbeatUnconfigured,
};

inline const char* readiness_reason_name(ReadinessReason reason) noexcept {
    switch (reason) {
        case ReadinessReason::kReady: return "ready";
        case ReadinessReason::kMetadataUnresolved: return "market_metadata";
        case ReadinessReason::kLedgerUnavailable: return "ledger";
        case ReadinessReason::kReconciliationFailed: return "reconciliation";
        case ReadinessReason::kUnreconciledOrders: return "unreconciled_orders";
        case ReadinessReason::kOpenOrdersPresent: return "open_orders";
        case ReadinessReason::kAccountStateUnproven: return "account_state";
        case ReadinessReason::kClosedOnlyAccount: return "closed_only_account";
        case ReadinessReason::kUserChannelUnconfigured:
            return "user_channel_configuration";
        case ReadinessReason::kHeartbeatUnconfigured:
            return "heartbeat_configuration";
    }
    return "invalid";
}

struct ReadinessInputs {
    bool metadata_resolved = true;
    bool ledger_open = true;
    bool reconciliation_ok = true;
    bool gate_open = true;
    size_t unknown_orders = 0;   // still unproven: the dangerous case
    size_t open_orders = 0;      // proven open at the venue (FAK: not armable)
    bool account_state_ok = true;  // Phase 6: balance + allowances proven
    bool closed_only = false;
    bool user_channel_configured = true;
    bool heartbeat_configured = true;
};

// Pure, dependency-ordered: the first unmet requirement is the reported one.
inline ReadinessReason evaluate_readiness(const ReadinessInputs& in) noexcept {
    if (!in.metadata_resolved) return ReadinessReason::kMetadataUnresolved;
    if (!in.ledger_open) return ReadinessReason::kLedgerUnavailable;
    if (!in.reconciliation_ok) return ReadinessReason::kReconciliationFailed;
    if (!in.gate_open)
        return in.unknown_orders > 0 ? ReadinessReason::kUnreconciledOrders
                                     : ReadinessReason::kOpenOrdersPresent;
    if (!in.account_state_ok) return ReadinessReason::kAccountStateUnproven;
    if (in.closed_only) return ReadinessReason::kClosedOnlyAccount;
    if (!in.user_channel_configured)
        return ReadinessReason::kUserChannelUnconfigured;
    if (!in.heartbeat_configured)
        return ReadinessReason::kHeartbeatUnconfigured;
    return ReadinessReason::kReady;
}

// ── transport ────────────────────────────────────────────────────────────────

#if defined(CROWDINTEL_HAVE_NETWORK)
// Authenticated GET against the CLOB host. The response is bounded; an
// oversized body is a failure (never a partial parse).
class RestTransport {
public:
    static constexpr size_t kBodyCap = 64 * 1024;

    RestTransport(const char* clob_host, const l2auth::Credentials& credentials,
                  const char* tls_pin)
        : credentials_(credentials) {
        static std::once_flag curl_once;
        std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        std::snprintf(host_, sizeof(host_), "%s", clob_host ? clob_host : "");
        secret_len_ = 0;
        if (!l2auth::decode_secret(credentials.secret_b64, secret_,
                                   sizeof(secret_), secret_len_))
            secret_len_ = 0;
        curl_ = curl_easy_init();
        if (!curl_) return;
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 8000L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "identity");
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, "crowdintel-bot/2.1");
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl_, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(curl_, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
        if (tls_pin && *tls_pin)
            curl_easy_setopt(curl_, CURLOPT_PINNEDPUBLICKEY, tls_pin);
    }

    ~RestTransport() {
        if (curl_) curl_easy_cleanup(curl_);
        secure_zero(secret_, sizeof(secret_));
        secret_len_ = 0;
    }

    RestTransport(const RestTransport&) = delete;
    RestTransport& operator=(const RestTransport&) = delete;

    bool usable() const noexcept {
        return curl_ != nullptr && secret_len_ > 0 && host_[0] != '\0';
    }
    const char* last_error() const noexcept { return last_error_; }

    // GETs `path` (+ optional query). `out` receives the body. Returns false
    // when no HTTP response was obtained or the body did not fit.
    bool get(const char* path, const char* query, long& http_code, char* out,
             size_t out_cap, size_t& out_len) noexcept {
        http_code = 0;
        out_len = 0;
        last_error_[0] = '\0';
        if (!usable() || !path || !*path) return fail("transport not usable");
        if (!out || out_cap < 2) return fail("output buffer too small");
        out[0] = '\0';
        char url[640];
        if (query && *query)
            std::snprintf(url, sizeof(url), "%s%s?%s", host_, path, query);
        else
            std::snprintf(url, sizeof(url), "%s%s", host_, path);
        if (std::strlen(url) >= sizeof(url) - 1) return fail("request URL too long");

        char timestamp[24];
        std::snprintf(timestamp, sizeof(timestamp), "%llu",
                      static_cast<unsigned long long>(::time(nullptr)));
        l2auth::Credentials credentials{credentials_.api_key,
                                        credentials_.api_address_hex,
                                        credentials_.secret_b64,
                                        credentials_.passphrase};
        l2auth::Headers headers_text;
        // The signed request path excludes the query string (verified).
        if (!l2auth::build_headers(credentials, secret_, secret_len_, timestamp,
                                   "GET", path, nullptr, 0, headers_text))
            return fail("L2 request signing failed");

        curl_slist* headers = nullptr;
        bool headers_ok = append_header(headers, headers_text.address);
        headers_ok = append_header(headers, headers_text.signature) && headers_ok;
        headers_ok = append_header(headers, headers_text.timestamp) && headers_ok;
        headers_ok = append_header(headers, headers_text.api_key) && headers_ok;
        headers_ok = append_header(headers, headers_text.passphrase) && headers_ok;
        if (!headers_ok) {
            curl_slist_free_all(headers);
            secure_zero(&headers_text, sizeof(headers_text));
            return fail("header allocation failed");
        }

        body_len_ = 0;
        body_overflow_ = false;
        body_[0] = '\0';
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_URL, url);
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        const CURLcode code = curl_easy_perform(curl_);
        curl_slist_free_all(headers);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
        secure_zero(&headers_text, sizeof(headers_text));
        if (code != CURLE_OK) return fail(curl_easy_strerror(code));
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        if (body_overflow_) return fail("CLOB response exceeded bounded buffer");
        out_len = body_len_ < out_cap - 1 ? body_len_ : out_cap - 1;
        std::memcpy(out, body_, out_len);
        out[out_len] = '\0';
        return true;
    }

private:
    bool fail(const char* text) noexcept {
        std::snprintf(last_error_, sizeof(last_error_), "%s", text);
        return false;
    }

    static bool append_header(curl_slist*& list, const char* value) {
        curl_slist* updated = curl_slist_append(list, value);
        if (!updated) return false;
        list = updated;
        return true;
    }

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<RestTransport*>(userdata);
        if (size != 0 && nmemb > SIZE_MAX / size) {
            self->body_overflow_ = true;
            return 0;
        }
        const size_t total = size * nmemb;
        const size_t available = sizeof(self->body_) - self->body_len_ - 1;
        const size_t take = total < available ? total : available;
        if (total > available) self->body_overflow_ = true;
        if (take) {
            std::memcpy(self->body_ + self->body_len_, ptr, take);
            self->body_len_ += take;
            self->body_[self->body_len_] = '\0';
        }
        return total;
    }

    l2auth::Credentials credentials_{};
    char host_[192]{};
    uint8_t secret_[l2auth::kSecretMaxBytes]{};
    size_t secret_len_ = 0;
    CURL* curl_ = nullptr;
    char body_[kBodyCap]{};
    size_t body_len_ = 0;
    bool body_overflow_ = false;
    char last_error_[128]{};
};
#endif  // CROWDINTEL_HAVE_NETWORK

// ── reconciler ───────────────────────────────────────────────────────────────

inline constexpr uint32_t kMaxPages = 12;      // 12 x default page size
inline constexpr size_t kCursorChars = 64;
inline constexpr size_t kPathChars = 160;

// Percent-encodes everything outside the unreserved set so a base64 cursor or
// an id can never break out of the query string.
inline void append_query_param(char* out, size_t cap, const char* key,
                               const char* value) noexcept {
    if (!out || cap == 0) return;
    size_t used = std::strlen(out);
    for (const char* p = key; *p && used + 1 < cap; ++p) out[used++] = *p;
    if (used + 1 < cap) out[used++] = '=';
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (const char* p = value; *p && used + 3 < cap; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == '~';
        if (unreserved) {
            out[used++] = static_cast<char>(c);
        } else {
            out[used++] = '%';
            out[used++] = kHex[c >> 4];
            out[used++] = kHex[c & 0x0F];
        }
    }
    out[used] = '\0';
}

// Venue ids go into a URL path, so only the alphabet they actually use is
// accepted; anything else is refused instead of being escaped blindly.
inline bool is_safe_venue_id(const char* id) noexcept {
    if (!id || id[0] != '0' || (id[1] != 'x' && id[1] != 'X')) return false;
    const size_t length = std::strlen(id);
    if (length < 3 || length > clob_info::kVenueIdChars - 1) return false;
    for (size_t i = 2; i < length; ++i) {
        const char c = id[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

template <typename Transport>
class Reconciler {
public:
    Reconciler(Transport& transport, const MarketConfig& cfg) noexcept
        : transport_(transport), cfg_(cfg) {}

    bool usable() const noexcept {
        return transport_.usable() && cfg_.owner_api_key[0] != '\0';
    }

    // Re-proves every non-terminal journaled order against the venue. Safe to
    // call repeatedly: transitions are evidence-based and idempotent.
    // Contract: `error` always holds either "" or the blocking reason when
    // this returns, so callers can print it unconditionally.
    Result run(cledger::OrderLedger& ledger, char* error,
               size_t error_cap) noexcept {
        report_ = Report{};
        if (error && error_cap > 0) error[0] = '\0';
        if (!usable()) {
            set_error(error, error_cap, "reconciliation is not configured");
            return Result::kBlocked;
        }
        ++report_.reconciliations;
        for (size_t i = 0; i < cledger::kMaxTrackedOrders; ++i) resolved_[i] = false;

        const Result ban = check_ban_status(error, error_cap);
        report_.closed_only = closed_only_;
        if (ban != Result::kReady) return ban;
        if (!walk_orders(ledger, error, error_cap)) return Result::kBlocked;
        if (!walk_trades(ledger, error, error_cap)) return Result::kBlocked;
        if (!resolve_remaining(ledger, error, error_cap)) return Result::kBlocked;

        if (report_.owner_mismatch) {
            set_error(error, error_cap,
                      "venue returned an order attributed to another API key");
            return Result::kBlocked;
        }
        if (report_.foreign_live_orders) {
            set_error(error, error_cap,
                      "venue has live orders in another market for this account");
            return Result::kBlocked;
        }
        if (report_.untracked_live_orders) {
            set_error(error, error_cap,
                      "venue has live orders this journal does not track");
            return Result::kBlocked;
        }
        if (report_.terminal_conflicts) {
            set_error(error, error_cap,
                      "a terminal journaled order is still live at the venue");
            return Result::kBlocked;
        }
        if (report_.unattributed_trades) {
            set_error(error, error_cap,
                      "venue trades reference orders this journal does not track");
            return Result::kBlocked;
        }
        if (report_.orders_unqueryable) {
            set_error(error, error_cap,
                      "journaled order without a venue id: reconcile by hand "
                      "(no client-order-id lookup exists)");
            return Result::kBlocked;
        }
        if (report_.unsafe_venue_id) {
            set_error(error, error_cap,
                      "journaled venue id is not a URL-safe order hash");
            return Result::kBlocked;
        }
        if (report_.unknown_status) {
            set_error(error, error_cap,
                      "venue reported an undocumented order status");
            return Result::kBlocked;
        }
        if (transition_failed_) {
            set_error(error, error_cap,
                      "reconciliation transition rejected by the state machine");
            return Result::kBlocked;
        }
        if (report_.truncated_maker_lists) {
            set_error(error, error_cap,
                      "trade with more maker orders than can be attributed");
            return Result::kBlocked;
        }
        if (report_.pagination_limit) {
            set_error(error, error_cap, "venue pagination limit reached");
            return Result::kBlocked;
        }
        // Every non-terminal order must now be proven by venue evidence. A
        // resting order is proven but keeps the ledger gate closed (this bot
        // trades FAK), which the readiness gate reports as `open_orders`.
        for (size_t i = 0; i < ledger.order_count(); ++i) {
            const cledger::OrderSummary* summary = ledger.summary_at(i);
            if (!summary || cledger::is_terminal(summary->state)) continue;
            if (summary->state == cledger::OrderState::kLive ||
                summary->state == cledger::OrderState::kPartiallyFilled) {
                ++report_.orders_open;
                continue;
            }
            char message[160];
            std::snprintf(message, sizeof(message),
                          "order %s is still %s after the REST check",
                          summary->client_order_id,
                          cledger::order_state_name(summary->state));
            set_error(error, error_cap, message);
            return Result::kBlocked;
        }
        return Result::kReady;
    }

    const Report& report() const noexcept { return report_; }

private:
    // Venue ids are hex hashes; compare them case-insensitively so a checksum
    // variation never looks like a different order.
    static size_t journal_index(const cledger::OrderLedger& ledger,
                                const char* venue_order_id) noexcept {
        if (!venue_order_id || !*venue_order_id) return SIZE_MAX;
        for (size_t i = 0; i < ledger.order_count(); ++i) {
            const cledger::OrderSummary* summary = ledger.summary_at(i);
            if (summary && clob_info::venue_id_equal(summary->venue_order_id,
                                                     venue_order_id))
                return i;
        }
        return SIZE_MAX;
    }

    struct WalkContext {
        Reconciler* self;
        cledger::OrderLedger* ledger;
    };

    static void set_error(char* error, size_t cap, const char* text) noexcept {
        if (error && cap) std::snprintf(error, cap, "%s", text);
    }

    bool blocked(char* error, size_t error_cap, const char* text) noexcept {
        set_error(error, error_cap, text);
        return false;
    }

    Result check_ban_status(char* error, size_t error_cap) noexcept {
        closed_only_ = false;
        char body[512];
        long http_code = 0;
        size_t length = 0;
        if (!transport_.get("/auth/ban-status/closed-only", "", http_code, body,
                            sizeof(body), length)) {
            set_error(error, error_cap, transport_.last_error());
            return Result::kTransportError;
        }
        if (http_code != 200) {
            char message[160];
            std::snprintf(message, sizeof(message),
                          "ban-status query returned HTTP %ld", http_code);
            set_error(error, error_cap, message);
            return http_code >= 500 ? Result::kTransportError : Result::kBlocked;
        }
        bool closed = false;
        if (!clob_info::parse_closed_only(body, length, closed)) {
            set_error(error, error_cap, "ban-status response is not parsable");
            return Result::kBlocked;
        }
        closed_only_ = closed;
        if (closed) {
            set_error(error, error_cap,
                      "account is closed-only: opening orders is not allowed");
            return Result::kBlocked;
        }
        return Result::kReady;
    }

    bool walk_orders(cledger::OrderLedger& ledger, char* error,
                     size_t error_cap) noexcept {
        WalkContext context{this, &ledger};
        char cursor[kCursorChars];
        cursor[0] = '\0';
        for (uint32_t page = 0; page < kMaxPages; ++page) {
            char query[clob_info::kVenueIdChars + 32];
            query[0] = '\0';
            if (cursor[0]) append_query_param(query, sizeof(query), "next_cursor",
                                              cursor);
            char body[Transport::kBodyCap];
            long http_code = 0;
            size_t length = 0;
            if (!transport_.get("/data/orders", query, http_code, body,
                                sizeof(body), length)) {
                set_error(error, error_cap, transport_.last_error());
                return false;
            }
            if (http_code != 200) {
                char message[160];
                std::snprintf(message, sizeof(message),
                              "GET /data/orders returned HTTP %ld", http_code);
                set_error(error, error_cap, message);
                return false;
            }
            ++report_.order_pages;
            const bool walked = clob_info::for_each_page_element(
                body, length, context,
                [](const clob_info::Span& element, WalkContext& c) {
                    return c.self->handle_live_order(element, *c.ledger);
                });
            if (!walked) {
                ++report_.malformed_payloads;
                set_error(error, error_cap, "orders page is malformed");
                return false;
            }
            cursor[0] = '\0';
            bool has_cursor = false;
            bool has_more_present = false;
            bool has_more = false;
            if (!clob_info::page_cursor(body, length, cursor, sizeof(cursor),
                                        has_cursor) ||
                !clob_info::has_more_field(body, length, has_more_present,
                                           has_more)) {
                ++report_.malformed_payloads;
                set_error(error, error_cap, "orders page has no usable cursor");
                return false;
            }
            if (!has_cursor) {
                // No cursor: the envelope must not claim another page either.
                if (has_more_present && has_more) {
                    ++report_.malformed_payloads;
                    set_error(error, error_cap,
                              "orders page claims more pages without a cursor");
                    return false;
                }
                return true;
            }
        }
        report_.pagination_limit = true;
        set_error(error, error_cap, "orders pagination limit reached");
        return false;
    }

    bool handle_live_order(const clob_info::Span& element,
                           cledger::OrderLedger& ledger) noexcept {
        clob_info::VenueOrder order;
        if (!clob_info::parse_open_order(element.begin, element.size(), order)) {
            ++report_.malformed_payloads;
            return false;
        }
        ++report_.live_orders;
        if (cfg_.owner_api_key[0] &&
            std::strcmp(order.owner, cfg_.owner_api_key) != 0) {
            ++report_.owner_mismatch;
            return true;
        }
        if (cfg_.runtime.condition_id[0] &&
            std::strcmp(order.market, cfg_.runtime.condition_id) != 0) {
            ++report_.foreign_live_orders;
            return true;
        }
        const size_t index = journal_index(ledger, order.id);
        if (index == SIZE_MAX) {
            ++report_.untracked_live_orders;
            return true;
        }
        const cledger::OrderSummary* summary = ledger.summary_at(index);
        if (!summary) return true;
        if (cledger::is_terminal(summary->state)) {
            // Terminal locally but the venue still lists it as open (or as a
            // full match when the journal does not say FILLED): contradiction.
            if (order.status == clob_info::VenueOrderStatus::kLive ||
                (order.status == clob_info::VenueOrderStatus::kMatched &&
                 summary->state != cledger::OrderState::kFilled))
                ++report_.terminal_conflicts;
            resolved_[index] = true;
            return true;
        }
        if (!apply_venue_order(ledger, index, order))
            transition_failed_ = true;
        resolved_[index] = true;
        return true;
    }

    bool walk_trades(cledger::OrderLedger& ledger, char* error,
                     size_t error_cap) noexcept {
        WalkContext context{this, &ledger};
        char cursor[kCursorChars];
        cursor[0] = '\0';
        for (uint32_t page = 0; page < kMaxPages; ++page) {
            char query[kCursorChars + 32];
            query[0] = '\0';
            if (cursor[0]) append_query_param(query, sizeof(query), "next_cursor",
                                              cursor);
            char body[Transport::kBodyCap];
            long http_code = 0;
            size_t length = 0;
            if (!transport_.get("/data/trades", query, http_code, body,
                                sizeof(body), length)) {
                set_error(error, error_cap, transport_.last_error());
                return false;
            }
            if (http_code != 200) {
                char message[160];
                std::snprintf(message, sizeof(message),
                              "GET /data/trades returned HTTP %ld", http_code);
                set_error(error, error_cap, message);
                return false;
            }
            ++report_.trade_pages;
            const bool walked = clob_info::for_each_page_element(
                body, length, context,
                [](const clob_info::Span& element, WalkContext& c) {
                    return c.self->handle_trade(element, *c.ledger);
                });
            if (!walked) {
                ++report_.malformed_payloads;
                set_error(error, error_cap, "trades page is malformed");
                return false;
            }
            cursor[0] = '\0';
            bool has_cursor = false;
            bool has_more_present = false;
            bool has_more = false;
            if (!clob_info::page_cursor(body, length, cursor, sizeof(cursor),
                                        has_cursor) ||
                !clob_info::has_more_field(body, length, has_more_present,
                                           has_more)) {
                ++report_.malformed_payloads;
                set_error(error, error_cap, "trades page has no usable cursor");
                return false;
            }
            if (!has_cursor) {
                if (has_more_present && has_more) {
                    ++report_.malformed_payloads;
                    set_error(error, error_cap,
                              "trades page claims more pages without a cursor");
                    return false;
                }
                return true;
            }
        }
        report_.pagination_limit = true;
        set_error(error, error_cap, "trades pagination limit reached");
        return false;
    }

    // A trade only matters if it is ours AND in the tracked market. When it is
    // ours, every order it names must already be journaled: a fill we cannot
    // attribute is an unreconciled account, not a rounding error.
    bool handle_trade(const clob_info::Span& element,
                      cledger::OrderLedger& ledger) noexcept {
        clob_info::VenueTrade trade;
        if (!clob_info::parse_trade(element.begin, element.size(), trade,
                                    cfg_.owner_api_key)) {
            ++report_.malformed_payloads;
            return false;
        }
        if (trade.maker_orders_truncated) ++report_.truncated_maker_lists;
        if (cfg_.runtime.condition_id[0] &&
            std::strcmp(trade.market, cfg_.runtime.condition_id) != 0)
            return true;
        const bool ours =
            (cfg_.owner_api_key[0] && std::strcmp(trade.owner,
                                                  cfg_.owner_api_key) == 0) ||
            trade.maker_orders_ours > 0;
        if (!ours) {
            ++report_.trades_foreign;
            return true;
        }
        ++report_.trades_seen;
        if (trade.taker_order_id[0] &&
            journal_index(ledger, trade.taker_order_id) != SIZE_MAX)
            return true;
        for (uint32_t i = 0; i < trade.maker_orders_kept; ++i) {
            const clob_info::VenueMakerOrder& maker = trade.makers[i];
            if (cfg_.owner_api_key[0] &&
                std::strcmp(maker.owner, cfg_.owner_api_key) != 0)
                continue;
            if (maker.order_id[0] &&
                journal_index(ledger, maker.order_id) != SIZE_MAX)
                return true;
        }
        ++report_.unattributed_trades;
        return true;
    }

    // Second pass: journaled orders the live page did not prove. Orders with a
    // venue id are asked for by id; the rest cannot be queried at all.
    bool resolve_remaining(cledger::OrderLedger& ledger, char* error,
                           size_t error_cap) noexcept {
        for (size_t i = 0; i < ledger.order_count(); ++i) {
            const cledger::OrderSummary* summary = ledger.summary_at(i);
            if (!summary || cledger::is_terminal(summary->state)) continue;
            if (resolved_[i]) continue;
            if (summary->venue_order_id[0] == '\0') {
                ++report_.orders_unqueryable;
                continue;
            }
            if (!is_safe_venue_id(summary->venue_order_id)) {
                ++report_.unsafe_venue_id;
                continue;
            }
            char path[kPathChars];
            std::snprintf(path, sizeof(path), "/data/order/%s",
                          summary->venue_order_id);
            char body[Transport::kBodyCap];
            long http_code = 0;
            size_t length = 0;
            if (!transport_.get(path, "", http_code, body, sizeof(body),
                                length)) {
                set_error(error, error_cap, transport_.last_error());
                return false;
            }
            if (http_code == 404) {
                // The venue returns canceled and fully matched orders from this
                // route, so a 404 is proof that it holds no such order.
                char local[192];
                char client_id[cledger::kClientOrderIdChars];
                std::snprintf(client_id, sizeof(client_id), "%s",
                              summary->client_order_id);
                const cledger::Transition step = ledger.record_transition(
                    client_id, cledger::LedgerEvent::kReconcileAbsent,
                    cledger::Evidence::kVenueRest, summary->venue_order_id, 0,
                    local, sizeof(local));
                ++report_.orders_absent;
                if (step.result == cledger::TransitionResult::kIllegal) {
                    ++report_.terminal_conflicts;
                    return true;
                }
                continue;
            }
            if (http_code != 200) {
                char message[160];
                std::snprintf(message, sizeof(message),
                              "GET /data/order returned HTTP %ld", http_code);
                set_error(error, error_cap, message);
                return false;
            }
            clob_info::VenueOrder order;
            if (!clob_info::parse_open_order(body, length, order)) {
                ++report_.malformed_payloads;
                set_error(error, error_cap, "single order response is malformed");
                return false;
            }
            if (!apply_venue_order(ledger, i, order)) {
                set_error(error, error_cap, "reconciliation transition rejected");
                return false;
            }
            resolved_[i] = true;
        }
        return true;
    }

    bool apply_venue_order(cledger::OrderLedger& ledger, size_t index,
                           const clob_info::VenueOrder& order) noexcept {
        const cledger::OrderSummary* summary = ledger.summary_at(index);
        if (!summary) return false;
        char client_id[cledger::kClientOrderIdChars];
        std::snprintf(client_id, sizeof(client_id), "%s",
                      summary->client_order_id);
        const char* venue_id = summary->venue_order_id[0]
                                   ? summary->venue_order_id
                                   : order.id;
        char local[192];
        cledger::Transition step{};
        using clob_info::VenueOrderStatus;
        switch (order.status) {
            case VenueOrderStatus::kLive:
            case VenueOrderStatus::kMatched:
                step = ledger.record_transition(
                    client_id, cledger::LedgerEvent::kReconcilePresent,
                    cledger::Evidence::kVenueRest, venue_id,
                    order.size_matched_f6, local, sizeof(local));
                ++report_.orders_present;
                break;
            case VenueOrderStatus::kCanceled:
            case VenueOrderStatus::kCanceledMarketResolved:
                // Record the cumulative fill first: a canceled order keeps its
                // fills, and canceling must not erase them.
                if (order.size_matched_f6 > 0)
                    step = ledger.record_transition(
                        client_id, cledger::LedgerEvent::kReconcilePresent,
                        cledger::Evidence::kVenueRest, venue_id,
                        order.size_matched_f6, local, sizeof(local));
                step = ledger.record_transition(
                    client_id, cledger::LedgerEvent::kOrderCanceled,
                    cledger::Evidence::kVenueRest, venue_id, 0, local,
                    sizeof(local));
                ++report_.orders_canceled;
                break;
            case VenueOrderStatus::kInvalid:
                // INVALID is not on the book and never becomes tradable. The
                // state machine only lets REST-proven absence reach REJECTED.
                step = ledger.record_transition(
                    client_id, cledger::LedgerEvent::kReconcileAbsent,
                    cledger::Evidence::kVenueRest, venue_id, 0, local,
                    sizeof(local));
                ++report_.orders_invalid;
                break;
            case VenueOrderStatus::kUnknown:
                ++report_.unknown_status;
                return true;
        }
        if (step.result == cledger::TransitionResult::kIllegal ||
            step.result == cledger::TransitionResult::kNeedsReconcile)
            return false;
        return true;
    }

    Transport& transport_;
    const MarketConfig& cfg_;
    Report report_{};
    bool resolved_[cledger::kMaxTrackedOrders]{};
    bool transition_failed_ = false;
    bool closed_only_ = false;
};

}  // namespace recon

#endif  // RECONCILIATION_HPP
