#ifndef MARKET_RESOLVER_HPP
#define MARKET_RESOLVER_HPP

// Phase 1 — REST resolution of the venue metadata that Fase 1 forbids in ENV.
//
// Compiled only with the network transport (CROWDINTEL_HAVE_NETWORK). Every
// request is a plain GET over verified TLS to a documented public endpoint; no
// credentials are used because none of this data is private. Responses are
// bounded and parsed by pure functions in market_metadata.hpp, then
// cross-validated by crowdintel::resolve_market().
//
// This header never logs or stores secrets: it has none.

#include <memory>
#include <mutex>

#if defined(CROWDINTEL_HAVE_NETWORK)

#include <cstdio>
#include <cstring>
#include <ctime>

#include <curl/curl.h>

#include "market_config.hpp"
#include "market_metadata.hpp"

class MarketMetadataResolver {
public:
    MarketMetadataResolver(const char* clob_host, const char* gamma_host,
                           const char* tls_pin, long timeout_ms = 4000)
        : timeout_ms_(timeout_ms) {
        static std::once_flag curl_once;
        std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        curl_ = curl_easy_init();
        std::snprintf(order_host_, sizeof(order_host_), "%s", clob_host ? clob_host : "");
        std::snprintf(gamma_host_, sizeof(gamma_host_), "%s", gamma_host ? gamma_host : "");
        std::snprintf(tls_pin_, sizeof(tls_pin_), "%s", tls_pin ? tls_pin : "");
        body_ = std::make_unique<char[]>(kMaxBody + 1);
    }

    ~MarketMetadataResolver() {
        if (curl_) curl_easy_cleanup(curl_);
    }

    MarketMetadataResolver(const MarketMetadataResolver&) = delete;
    MarketMetadataResolver& operator=(const MarketMetadataResolver&) = delete;

    // Resolves, validates, and installs the runtime descriptor into `cfg`.
    // Returns false and fills `error` on any missing/mismatched/ambiguous
    // venue parameter. `cfg.runtime` is only modified on success.
    bool resolve(MarketConfig& cfg, char* error, size_t error_cap) {
        crowdintel::GammaMarketView gamma;
        if (cfg.market_slug[0] &&
            std::strcmp(cfg.market_slug, "condition-pending") != 0) {
            char url[320];
            std::snprintf(url, sizeof(url), "%s/markets/slug/%s", gamma_host_,
                          cfg.market_slug);
            if (!get(url, 262144)) return fail(error, error_cap, "gamma request failed",
                                               last_error_);
            if (!crowdintel::parse_gamma_market(body_.get(), last_length_, gamma))
                return fail(error, error_cap, "gamma market parse failed", "");
        } else if (cfg.condition_id[0]) {
            char url[384];
            std::snprintf(url, sizeof(url), "%s/markets?condition_ids=%s",
                          gamma_host_, cfg.condition_id);
            if (!get(url, 262144)) return fail(error, error_cap, "gamma request failed",
                                               last_error_);
            if (!crowdintel::parse_gamma_market_list(body_.get(), last_length_, gamma))
                return fail(error, error_cap, "gamma market list parse failed", "");
        } else {
            return fail(error, error_cap,
                        "no market selector: BOT_MARKET_SLUG or BOT_CONDITION_ID", "");
        }
        if (cfg.condition_id[0] &&
            std::strcmp(cfg.condition_id, gamma.condition_id) != 0)
            return fail(error, error_cap, "gamma condition id mismatch",
                        gamma.condition_id);

        char url[320];
        crowdintel::ClobMarketDetailsView clob;
        std::snprintf(url, sizeof(url), "%s/clob-markets/%s", order_host_,
                      gamma.condition_id);
        if (!get(url, 262144)) return fail(error, error_cap, "clob-markets failed",
                                           last_error_);
        if (!crowdintel::parse_clob_market_details(body_.get(), last_length_, clob))
            return fail(error, error_cap, "clob-markets parse failed", "");

        // Token selection is needed before the per-token probes.
        char token_id[crowdintel::K_TOKEN_ID_CHARS]{};
        if (!select_token(gamma, clob, cfg.outcome, token_id, error, error_cap))
            return false;

        uint64_t tick_probe = 0;
        std::snprintf(url, sizeof(url), "%s/tick-size?token_id=%s", order_host_,
                      token_id);
        if (!get(url, 8192)) return fail(error, error_cap, "tick-size failed",
                                         last_error_);
        if (!crowdintel::parse_tick_size_response(body_.get(), last_length_,
                                                  tick_probe))
            return fail(error, error_cap, "tick-size parse failed", "");

        double fee_probe = 0.0;
        std::snprintf(url, sizeof(url), "%s/fee-rate?token_id=%s", order_host_,
                      token_id);
        if (!get(url, 8192)) return fail(error, error_cap, "fee-rate failed",
                                         last_error_);
        if (!crowdintel::parse_fee_rate_response(body_.get(), last_length_,
                                                 fee_probe))
            return fail(error, error_cap, "fee-rate parse failed", "");

        crowdintel::BookView book;
        std::snprintf(url, sizeof(url), "%s/book?token_id=%s", order_host_,
                      token_id);
        if (!get(url, 1048576)) return fail(error, error_cap, "book failed",
                                            last_error_);
        if (!crowdintel::parse_book_summary(body_.get(), last_length_, book))
            return fail(error, error_cap, "book parse failed", "");

        int64_t server_time = 0;
        std::snprintf(url, sizeof(url), "%s/time", order_host_);
        if (!get(url, 4096)) return fail(error, error_cap, "server time failed",
                                         last_error_);
        if (!crowdintel::parse_server_time(body_.get(), last_length_, server_time))
            return fail(error, error_cap, "server time parse failed", "");

        timespec now{};
        clock_gettime(CLOCK_REALTIME, &now);

        crowdintel::MarketResolutionInputs inputs;
        inputs.gamma = &gamma;
        inputs.clob = &clob;
        inputs.book = &book;
        inputs.tick_size_probe = &tick_probe;
        inputs.fee_rate_probe = &fee_probe;
        inputs.server_time_seconds = &server_time;
        inputs.local_time_seconds = static_cast<int64_t>(now.tv_sec);
        inputs.max_clock_offset_ms = cfg.max_clock_offset_ms;
        inputs.requested_outcome = cfg.outcome[0] ? cfg.outcome : nullptr;

        const crowdintel::MarketResolution result =
            crowdintel::resolve_market(inputs);
        if (!result.ok())
            return fail(error, error_cap,
                        crowdintel::resolution_issue_name(result.issue),
                        result.detail);

        cfg.runtime = result.runtime;
        // The slug is venue-owned metadata; when the operator selected by
        // condition id the alpha routing identity is bound after resolution.
        if (cfg.market_slug[0] == '\0' ||
            std::strcmp(cfg.market_slug, "condition-pending") == 0) {
            if (cfg.runtime.market_slug[0]) {
                std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "%s",
                              cfg.runtime.market_slug);
                cfg.market_hash = alpha_hash_bytes(
                    cfg.market_slug, std::strlen(cfg.market_slug));
            }
        }
        return true;
    }

private:
    static constexpr size_t kMaxBody = 1048576;

    static bool fail(char* error, size_t cap, const char* what,
                     const char* extra) {
        std::snprintf(error, cap, "%s%s%s", what, extra && *extra ? ": " : "",
                      extra && *extra ? extra : "");
        return false;
    }

    static bool select_token(const crowdintel::GammaMarketView& gamma,
                             const crowdintel::ClobMarketDetailsView& clob,
                             const char* requested_outcome, char* out,
                             char* error, size_t error_cap) {
        out[0] = '\0';
        const char* desired = requested_outcome && *requested_outcome
                                  ? requested_outcome : nullptr;
        if (!desired && gamma.token_count != 1)
            return fail(error, error_cap,
                        "multi-outcome market requires BOT_OUTCOME", "");
        for (size_t i = 0; i < gamma.token_count; ++i) {
            if (desired && !crowdintel::equals_ascii_ci(gamma.outcome_labels[i],
                                                        desired))
                continue;
            for (size_t j = 0; j < clob.token_count; ++j) {
                if (std::strcmp(clob.tokens[j].token_id, gamma.token_ids[i]) == 0) {
                    std::snprintf(out, crowdintel::K_TOKEN_ID_CHARS, "%s",
                                  clob.tokens[j].token_id);
                    return true;
                }
            }
        }
        return fail(error, error_cap, "outcome has no CLOB token", "");
    }

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<MarketMetadataResolver*>(userdata);
        if (size != 0 && nmemb > SIZE_MAX / size) {
            self->overflow_ = true;
            return 0;
        }
        const size_t total = size * nmemb;
        if (self->last_length_ + total > self->limit_) {
            self->overflow_ = true;
            return 0;
        }
        std::memcpy(self->body_.get() + self->last_length_, ptr, total);
        self->last_length_ += total;
        self->body_[self->last_length_] = '\0';
        return total;
    }

    bool get(const char* url, size_t limit) {
        if (!curl_) { std::snprintf(last_error_, sizeof(last_error_), "curl init"); return false; }
        last_length_ = 0;
        overflow_ = false;
        limit_ = limit < kMaxBody ? limit : kMaxBody;
        body_[0] = '\0';
        curl_easy_setopt(curl_, CURLOPT_URL, url);
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, timeout_ms_);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "identity");
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, "crowdintel-metadata/1.0");
        if (tls_pin_[0]) curl_easy_setopt(curl_, CURLOPT_PINNEDPUBLICKEY, tls_pin_);
        const CURLcode code = curl_easy_perform(curl_);
        if (code != CURLE_OK) {
            std::snprintf(last_error_, sizeof(last_error_), "%s",
                          curl_easy_strerror(code));
            return false;
        }
        long http_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code < 200 || http_code >= 300) {
            std::snprintf(last_error_, sizeof(last_error_), "HTTP %ld", http_code);
            return false;
        }
        if (overflow_) {
            std::snprintf(last_error_, sizeof(last_error_), "response too large");
            return false;
        }
        return true;
    }

    CURL* curl_ = nullptr;
    std::unique_ptr<char[]> body_;
    size_t last_length_ = 0;
    size_t limit_ = kMaxBody;
    bool overflow_ = false;
    long timeout_ms_ = 4000;
    char order_host_[160]{};
    char gamma_host_[160]{};
    char tls_pin_[128]{};
    char last_error_[96]{};
};

#endif  // CROWDINTEL_HAVE_NETWORK

#endif  // MARKET_RESOLVER_HPP
