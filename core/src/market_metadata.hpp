#ifndef CROWDINTEL_MARKET_METADATA_HPP
#define CROWDINTEL_MARKET_METADATA_HPP

// ─────────────────────────────────────────────────────────────────────────────
// market_metadata.hpp — read the maker sleeve's parameters from the venue.
//
// WHY
// Motor C's three inputs — the reward pool, the band you must quote inside, and
// the minimum size that qualifies — are published per market in the Gamma
// object. Hand-typing them into BOT_MK_* environment variables is a silent
// failure waiting to happen: a stale band means quotes placed outside
// eligibility, which earns nothing while still carrying full adverse risk.
//
// Verified live (2026-10-29) on a real market:
//
//   "rewardsMaxSpread": 3.5,          // FRACTIONAL CENTS, not an integer
//   "rewardsMinSize": 200,            // shares
//   "clobRewards": [{"rewardsDailyRate": 3000, "startDate": "2026-10-27",
//                    "endDate": "2500-12-31", ...}]
//
// `rewardsMaxSpread` being fractional is the trap: 3.5 is 350 basis points, and
// carrying it as an integer of cents would silently round the eligibility band
// by up to 15%.
//
// The parser is pure and bounded so it can be tested against captured payloads;
// the fetch is cold path (one call before the tick loop starts).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../include/bounded_json.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK) && !defined(CROWDINTEL_FORCE_MOCK)
#include <curl/curl.h>
#endif

namespace crowdintel {

struct MarketRewards {
    uint64_t daily_rate_usdc = 0;  // sum of clobRewards[].rewardsDailyRate
    uint32_t max_spread_bps = 0;   // rewardsMaxSpread in cents -> basis points
    uint64_t min_size_shares = 0;  // rewardsMinSize, 1e6 fixed point
    bool has_program = false;      // at least one positive daily rate
    // Market mechanics, not rewards. These decide whether an order is even
    // accepted: quoting off-tick on a 0.001 market with a 0.01 default makes the
    // venue reject everything, and a hand-typed fee category can be wrong.
    uint64_t tick_size_units = 0;  // orderPriceMinTickSize, 1e6 fixed point
    double taker_fee_rate = -1.0;  // feeSchedule.rate, negative = absent
};

enum class RewardParse : uint8_t {
    Ok = 0,
    NotJson,
    MissingFields,  // the market object has no reward fields at all
    BadNumber
};

inline const char* reward_parse_name(RewardParse r) noexcept {
    switch (r) {
        case RewardParse::Ok:            return "ok";
        case RewardParse::NotJson:       return "not_json";
        case RewardParse::MissingFields: return "missing_fields";
        case RewardParse::BadNumber:     return "bad_number";
    }
    return "unknown";
}

// ── Minimal strict readers (flat scan; the object is small and shallow) ─────

inline const char* mm_skip_ws(const char* p, const char* end) noexcept {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

// Finds "key": and returns the value span (quotes stripped for strings).
inline bool mm_find(const char* begin, const char* end, const char* key,
                    size_t key_len, const char*& value,
                    const char*& value_end) noexcept {
    for (const char* p = begin; p + key_len + 2 <= end; ++p) {
        if (*p != '"') continue;
        const char* k = p + 1;
        if (static_cast<size_t>(end - k) < key_len) break;
        if (std::memcmp(k, key, key_len) != 0) continue;
        const char* after = k + key_len;
        if (after >= end || *after != '"') continue;
        ++after;
        after = mm_skip_ws(after, end);
        if (after >= end || *after != ':') continue;
        after = mm_skip_ws(after + 1, end);
        if (after >= end) return false;
        const char* ve = after;
        if (*ve == '"') {
            ++ve;
            while (ve < end && *ve != '"') ++ve;
            if (ve >= end) return false;
            ++ve;
        } else {
            while (ve < end && *ve != ',' && *ve != '}' && *ve != ']' &&
                   *ve != ' ' && *ve != '\n' && *ve != '\r' && *ve != '\t')
                ++ve;
        }
        value = after;
        value_end = ve;
        return true;
    }
    return false;
}

inline bool mm_read_double(const char* begin, const char* end, const char* key,
                           size_t key_len, double& out) noexcept {
    const char* v = nullptr;
    const char* ve = nullptr;
    if (!mm_find(begin, end, key, key_len, v, ve)) return false;
    if (v < ve && *v == '"') {
        ++v;
        --ve;
    }
    if (v >= ve || (ve - v) > 32) return false;
    char buf[33];
    std::memcpy(buf, v, static_cast<size_t>(ve - v));
    buf[ve - v] = '\0';
    char* stop = nullptr;
    const double parsed = std::strtod(buf, &stop);
    if (!stop || *stop != '\0' || !(parsed == parsed)) return false;  // NaN guard
    out = parsed;
    return true;
}

inline bool mm_read_u64(const char* begin, const char* end, const char* key,
                        size_t key_len, uint64_t& out) noexcept {
    double d = 0.0;
    if (!mm_read_double(begin, end, key, key_len, d)) return false;
    if (d < 0.0 || d > 1.0e15) return false;
    out = static_cast<uint64_t>(d);
    return true;
}

// Returns the span of `"key":{...}` including the braces. A flat search for
// "rate" would be a guess; the fee lives inside feeSchedule and must be read
// from there.
inline bool mm_find_object(const char* begin, const char* end, const char* key,
                           size_t key_len, const char*& obj_begin,
                           const char*& obj_end) noexcept {
    const char* v = nullptr;
    const char* ve = nullptr;
    if (!mm_find(begin, end, key, key_len, v, ve)) return false;
    const char* open = nullptr;
    for (const char* p = v; p < ve; ++p) {
        if (*p == '{') {
            open = p;
            break;
        }
    }
    if (!open) return false;
    int depth = 0;
    for (const char* p = open; p < end; ++p) {
        if (*p == '{') ++depth;
        else if (*p == '}') {
            --depth;
            if (depth == 0) {
                obj_begin = open + 1;
                obj_end = p;
                return true;
            }
        }
    }
    return false;
}

// ── Parser ──────────────────────────────────────────────────────────────────
// Accepts either a single market object or an array of them (Gamma returns an
// array for /markets?slug=…); the first object that carries reward fields wins.
inline RewardParse parse_market_rewards(const char* json, size_t len,
                                        MarketRewards& out) noexcept {
    if (!json || len == 0) return RewardParse::NotJson;
    if (!bounded_json::valid_document(json, len)) return RewardParse::NotJson;

    const char* begin = json;
    const char* end = json + len;

    MarketRewards parsed{};

    // rewardsMaxSpread: fractional cents -> basis points. 3.5c == 350 bps.
    double max_spread_cents = 0.0;
    const bool has_band =
        mm_read_double(begin, end, "rewardsMaxSpread", 16, max_spread_cents);
    if (!has_band) return RewardParse::MissingFields;
    if (max_spread_cents < 0.0 || max_spread_cents > 100.0)
        return RewardParse::BadNumber;
    // Round to the nearest basis point: 3.5 * 100 = 350 exactly, and a value
    // like 3.55 becomes 355 rather than truncating to 355 anyway.
    parsed.max_spread_bps =
        static_cast<uint32_t>(max_spread_cents * 100.0 + 0.5);

    uint64_t min_size = 0;
    if (!mm_read_u64(begin, end, "rewardsMinSize", 14, min_size))
        return RewardParse::MissingFields;
    parsed.min_size_shares = min_size * 1000000ULL;  // shares -> 1e6 scale

    // clobRewards is an array of objects; sum every rates field present.
    // Date windows are not filtered here on purpose: the observed values are
    // open-ended ("2500-12-31"), and a market with a future start date would be
    // one the operator has not enabled yet, not one this parser should guess at.
    uint64_t total_rate = 0;
    const char* cursor = begin;
    while (true) {
        const char* v = nullptr;
        const char* ve = nullptr;
        if (!mm_find(cursor, end, "rewardsDailyRate", 16, v, ve)) break;
        if (v < ve && *v == '"') {
            ++v;
            --ve;
        }
        if (v < ve && (ve - v) <= 24) {
            char buf[25];
            std::memcpy(buf, v, static_cast<size_t>(ve - v));
            buf[ve - v] = '\0';
            char* stop = nullptr;
            const double rate = std::strtod(buf, &stop);
            if (stop && *stop == '\0' && rate > 0.0 && rate < 1.0e12)
                total_rate += static_cast<uint64_t>(rate);
        }
        // Advance past this occurrence to find the next array entry.
        cursor = (ve < end) ? ve : end;
        if (cursor >= end) break;
        ++cursor;
    }
    parsed.daily_rate_usdc = total_rate;
    parsed.has_program = total_rate > 0 && parsed.max_spread_bps > 0 &&
                         parsed.min_size_shares > 0;

    // orderPriceMinTickSize: 0.001 on the up/down markets, 0.01 elsewhere.
    double tick = 0.0;
    if (mm_read_double(begin, end, "orderPriceMinTickSize", 21, tick) &&
        tick > 0.0 && tick < 1.0) {
        const double units = tick * 1000000.0;
        const uint64_t u = static_cast<uint64_t>(units + 0.5);
        if (u > 0 && 1000000ULL % u == 0) parsed.tick_size_units = u;
    }

    // feeSchedule.rate, read from inside the nested object only.
    const char* fs_begin = nullptr;
    const char* fs_end = nullptr;
    if (mm_find_object(begin, end, "feeSchedule", 11, fs_begin, fs_end)) {
        double rate = -1.0;
        if (mm_read_double(fs_begin, fs_end, "rate", 4, rate) && rate >= 0.0 &&
            rate <= 1.0)
            parsed.taker_fee_rate = rate;
    }

    out = parsed;
    return RewardParse::Ok;
}

#if defined(CROWDINTEL_HAVE_NETWORK) && !defined(CROWDINTEL_FORCE_MOCK)
namespace metadata_detail {
struct Sink {
    char* buf;
    size_t cap;
    size_t len;
    bool overflow;
};
inline size_t write_cb(char* data, size_t size, size_t nmemb, void* user) {
    auto* sink = static_cast<Sink*>(user);
    const size_t n = size * nmemb;
    if (!sink || !sink->buf || sink->len + n >= sink->cap) {
        if (sink) sink->overflow = true;
        return 0;
    }
    std::memcpy(sink->buf + sink->len, data, n);
    sink->len += n;
    sink->buf[sink->len] = '\0';
    return n;
}
}  // namespace metadata_detail

// Cold path only: one authenticated-less GET at startup. Returns RewardParse so
// the caller can distinguish "no program" from "fetch failed" and keep the
// operator-supplied values in the latter case.
inline RewardParse fetch_market_rewards(const char* gamma_host,
                                        const char* market_slug,
                                        MarketRewards& out) noexcept {
    if (!gamma_host || !*gamma_host || !market_slug || !*market_slug)
        return RewardParse::MissingFields;

    static char body[32768];
    char url[512];
    const int n = std::snprintf(url, sizeof(url),
                                "%s/markets?limit=1&slug=%s", gamma_host,
                                market_slug);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(url))
        return RewardParse::MissingFields;

    CURL* curl = curl_easy_init();
    if (!curl) return RewardParse::NotJson;
    metadata_detail::Sink sink{body, sizeof(body), 0, false};
    body[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "crowdintel-bot/2.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, metadata_detail::write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const CURLcode code = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK || sink.overflow || sink.len == 0 || http_code < 200 ||
        http_code >= 300)
        return RewardParse::NotJson;

    return parse_market_rewards(body, sink.len, out);
}
#endif  // CROWDINTEL_HAVE_NETWORK

}  // namespace crowdintel

#endif  // CROWDINTEL_MARKET_METADATA_HPP
