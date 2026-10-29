#ifndef CROWDINTEL_CHAINLINK_STREAM_HPP
#define CROWDINTEL_CHAINLINK_STREAM_HPP

// ─────────────────────────────────────────────────────────────────────────────
// chainlink_stream.hpp — Chainlink Data Streams ingestion + TWAP-60s replica.
//
// WHY THIS SHAPE
// Polymarket's crypto up/down markets do not settle on Binance spot. Verified
// live from the Gamma market object:
//
//   cryptoMarketConfig: {"id":"btc-15m-twap-60","asset":"btc","duration":"15m",
//                        "twapEnabled":true,"twapLookbackSeconds":60}
//   resolutionSource:   https://data.chain.link/streams/btc-usd-twap-60s-streams
//
// So the settlement truth is a 60-second TWAP of the Chainlink BTC/USD stream.
// Two consequences that define the whole strategy:
//
//   1. The `twap-60s` REPORT is the label, not the signal. Chainlink's own docs
//      put DON consensus + delivery at 1-2 s, so by the time the report lands
//      the market has already repriced. You cannot beat an oracle with itself.
//
//   2. The predictive edge comes from the UNDERLYING `btc-usd` stream: same
//      series, sub-second updates, zero basis risk versus the settlement value.
//      Reconstructing the 60 s TWAP locally from that stream lets you publish
//      an opinion roughly 1 s before the DON does.
//
// This header therefore models one thing — a report sample — and derives the
// time-weighted 60 s mean from it. It deliberately does not decide which
// transport delivered the sample: replays (offline research, CI) and the live
// puller produce identical records, so the model and its tests never depend on
// network access.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/bounded_json.hpp"

namespace crowdintel {

// ── Report sample ────────────────────────────────────────────────────────────
struct ChainlinkReport {
    uint64_t observations_timestamp = 0;  // DON observation time (seconds)
    uint64_t valid_from_timestamp = 0;
    uint64_t expires_at = 0;
    double benchmark_price = 0.0;  // normalised to USD
    double bid = 0.0;
    double ask = 0.0;
    uint64_t feed_hash = 0;  // fnv1a of the feed id, for cheap identity checks
};

enum class ChainlinkParse : uint8_t {
    Ok = 0,
    NotJson = 1,        // failed structural validation
    MissingField = 2,   // a required key is absent
    BadNumber = 3,      // a numeric field is not a finite number
    OutOfOrder = 4,     // observation older than the last accepted sample
    NotWarm = 5         // TWAP window has not accumulated enough coverage yet
};

// ── Scaled-price helper ──────────────────────────────────────────────────────
// Data Streams reports carry prices as fixed-point integers (18 decimals by
// default, transported as strings). Fixtures and human-written tests use plain
// decimals. Accept both: a token containing '.' is parsed as a decimal,
// otherwise it is divided by `scale`.
inline bool parse_scaled_price(const char* text, size_t len, double scale,
                               double& out) noexcept {
    if (!text || len == 0 || len > 64) return false;
    bool has_dot = false;
    for (size_t i = 0; i < len; ++i) {
        const char c = text[i];
        if (c == '.') {
            has_dot = true;
        } else if ((c < '0' || c > '9') && c != '-' && c != '+') {
            return false;  // reject exponents, whitespace, anything exotic
        }
    }
    char buf[65];
    std::memcpy(buf, text, len);
    buf[len] = '\0';
    char* end = nullptr;
    const double parsed = std::strtod(buf, &end);
    if (!end || *end != '\0' || !(parsed == parsed)) return false;  // NaN guard
    if (has_dot) {
        out = parsed;
        return true;
    }
    if (!(scale > 0.0)) return false;
    out = parsed / scale;
    return true;
}

// ── Minimal strict JSON field readers ────────────────────────────────────────
// The report object is flat, so a linear scan per key is exact and allocation
// free. A missing key is reported, never defaulted to zero: a silently zero
// price would look like a real quote and feed the model garbage.
inline const char* clr_skip_ws(const char* p, const char* end) noexcept {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

inline bool clr_find_key_value(const char* begin, const char* end,
                               const char* key, size_t key_len,
                               const char*& value, const char*& value_end) noexcept {
    for (const char* p = begin; p + key_len + 2 <= end; ++p) {
        if (*p != '"') continue;
        const char* kmatch = p + 1;
        if (static_cast<size_t>(end - kmatch) < key_len) break;
        if (std::memcmp(kmatch, key, key_len) != 0) continue;
        const char* after = kmatch + key_len;
        if (after >= end || *after != '"') continue;
        ++after;
        after = clr_skip_ws(after, end);
        if (after >= end || *after != ':') continue;
        after = clr_skip_ws(after + 1, end);
        if (after >= end) return false;
        const char* vend = after;
        if (*vend == '"') {
            ++vend;
            while (vend < end && *vend != '"') ++vend;
            if (vend >= end) return false;
            ++vend;
        } else {
            while (vend < end && *vend != ',' && *vend != '}' && *vend != ']' &&
                   *vend != ' ' && *vend != '\n' && *vend != '\r' && *vend != '\t')
                ++vend;
        }
        value = after;
        value_end = vend;
        return true;
    }
    return false;
}

inline bool clr_read_number(const char* begin, const char* end, const char* key,
                            size_t key_len, double scale, double& out) noexcept {
    const char* v = nullptr;
    const char* ve = nullptr;
    if (!clr_find_key_value(begin, end, key, key_len, v, ve)) return false;
    if (v < ve && *v == '"') {
        ++v;
        --ve;
    }
    return parse_scaled_price(v, static_cast<size_t>(ve - v), scale, out);
}

inline bool clr_read_u64(const char* begin, const char* end, const char* key,
                         size_t key_len, uint64_t& out) noexcept {
    const char* v = nullptr;
    const char* ve = nullptr;
    if (!clr_find_key_value(begin, end, key, key_len, v, ve)) return false;
    if (v < ve && *v == '"') {
        ++v;
        --ve;
    }
    if (v >= ve) return false;
    uint64_t acc = 0;
    for (const char* p = v; p < ve; ++p) {
        if (*p < '0' || *p > '9') return false;
        if (acc > (UINT64_MAX - 9) / 10) return false;
        acc = acc * 10 + static_cast<uint64_t>(*p - '0');
    }
    out = acc;
    return true;
}

// ── Report parser ────────────────────────────────────────────────────────────
// `price_scale` is 1e18 for live Data Streams payloads; pass 1.0 for fixtures
// written with plain decimals.
inline ChainlinkParse parse_chainlink_report(const char* json, size_t len,
                                             double price_scale,
                                             ChainlinkReport& out) noexcept {
    if (!json || len == 0) return ChainlinkParse::NotJson;
    if (!bounded_json::valid_document(json, len)) return ChainlinkParse::NotJson;

    const char* begin = json;
    const char* end = json + len;

    ChainlinkReport parsed{};
    // observationsTimestamp is the only field the model cannot work without:
    // without a clock the sample has no place on the time axis.
    if (!clr_read_u64(begin, end, "observationsTimestamp", 21,
                      parsed.observations_timestamp))
        return ChainlinkParse::MissingField;

    // benchmarkPrice may appear as "benchmarkPrice" (report object) or, in
    // some payloads, as "price". Accept either, never default.
    if (!clr_read_number(begin, end, "benchmarkPrice", 14, price_scale,
                         parsed.benchmark_price) &&
        !clr_read_number(begin, end, "price", 5, price_scale,
                         parsed.benchmark_price))
        return ChainlinkParse::MissingField;

    if (!(parsed.benchmark_price > 0.0)) return ChainlinkParse::BadNumber;

    // Optional fields: present in production, absent in minimal fixtures.
    (void)clr_read_number(begin, end, "bid", 3, price_scale, parsed.bid);
    (void)clr_read_number(begin, end, "ask", 3, price_scale, parsed.ask);
    (void)clr_read_u64(begin, end, "validFromTimestamp", 18,
                       parsed.valid_from_timestamp);
    (void)clr_read_u64(begin, end, "expiresAt", 9, parsed.expires_at);

    // Feed identity: a cheap hash so a caller can reject the wrong asset without
    // string handling.
    const char* fv = nullptr;
    const char* fe = nullptr;
    if (clr_find_key_value(begin, end, "feedID", 6, fv, fe)) {
        if (fv < fe && *fv == '"') {
            ++fv;
            --fe;
        }
        uint64_t hash = 1469598103934665603ULL;  // fnv1a-64 offset basis
        for (const char* p = fv; p < fe; ++p) {
            hash ^= static_cast<uint8_t>(*p);
            hash *= 1099511628211ULL;
        }
        parsed.feed_hash = hash;
    }

    out = parsed;
    return ChainlinkParse::Ok;
}

// ── Time-weighted TWAP over a sliding window ─────────────────────────────────
// The settlement value is a 60 s TWAP, so a simple arithmetic mean of samples is
// wrong whenever the DON's cadence is uneven (it is: reports are not
// clock-aligned). Integrate price over time instead: sum(p_i * dt_i) / sum(dt_i).
class TwapWindow {
public:
    static constexpr size_t CAPACITY = 512;

    void configure(uint64_t lookback_ns) noexcept {
        lookback_ns_ = lookback_ns;
        reset();
    }

    void reset() noexcept {
        head_ = 0;
        count_ = 0;
        last_ts_ns_ = 0;
    }

    // Returns false for an out-of-order sample (a replay that jumps backwards
    // would silently corrupt the integral, so it is refused rather than clipped).
    bool push(uint64_t ts_ns, double price) noexcept {
        if (!(price > 0.0)) return false;
        if (count_ != 0 && ts_ns <= last_ts_ns_) return false;
        samples_[head_].ts_ns = ts_ns;
        samples_[head_].price = price;
        head_ = (head_ + 1) % CAPACITY;
        if (count_ < CAPACITY) ++count_;
        last_ts_ns_ = ts_ns;
        return true;
    }

    // Coverage check: without roughly a full lookback of history the integral
    // describes a shorter window than the venue settles on.
    bool ready(uint64_t now_ns) const noexcept {
        if (count_ < 2) return false;
        const uint64_t oldest = oldest_ts();
        return now_ns > oldest && (now_ns - oldest) >= lookback_ns_;
    }

    // Time-weighted mean over [now - lookback, now]. Samples older than the
    // window contribute nothing; the first in-window interval is clipped.
    double twap(uint64_t now_ns) const noexcept {
        // Only a query BEFORE the newest sample is unanswerable; querying at
        // exactly last_ts is normal (the last sample simply spans zero time).
        if (count_ < 2 || now_ns < last_ts_ns_) return 0.0;
        const uint64_t start = now_ns > lookback_ns_ ? now_ns - lookback_ns_ : 0;

        double integral = 0.0;
        uint64_t covered = 0;
        for (size_t i = 0; i < count_; ++i) {
            const size_t idx =
                (head_ + CAPACITY - count_ + i) % CAPACITY;  // oldest first
            const uint64_t t0 = samples_[idx].ts_ns;
            const uint64_t t1 = (i + 1 < count_)
                                    ? samples_[(idx + 1) % CAPACITY].ts_ns
                                    : now_ns;
            if (t1 <= start) continue;          // entirely before the window
            if (t0 >= now_ns) break;            // entirely after "now"
            const uint64_t lo = t0 > start ? t0 : start;
            const uint64_t hi = t1 < now_ns ? t1 : now_ns;
            if (hi <= lo) continue;
            const double dt = static_cast<double>(hi - lo);
            integral += samples_[idx].price * dt;
            covered += (hi - lo);
        }
        if (covered == 0) return 0.0;
        return integral / static_cast<double>(covered);
    }

    size_t samples() const noexcept { return count_; }
    uint64_t last_ts() const noexcept { return last_ts_ns_; }

private:
    struct Sample {
        uint64_t ts_ns;
        double price;
    };

    uint64_t oldest_ts() const noexcept {
        if (count_ == 0) return 0;
        return samples_[(head_ + CAPACITY - count_) % CAPACITY].ts_ns;
    }

    Sample samples_[CAPACITY]{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint64_t last_ts_ns_ = 0;
    uint64_t lookback_ns_ = 60'000'000'000ULL;
};

// ── Replay source ────────────────────────────────────────────────────────────
// One JSON report per line (NDJSON). This is what the research harness and the
// unit tests feed the model, so no strategy decision depends on live network
// access being available at test time.
class ChainlinkReplay {
public:
    ~ChainlinkReplay() {
        if (file_) std::fclose(file_);
    }
    ChainlinkReplay() = default;
    ChainlinkReplay(const ChainlinkReplay&) = delete;
    ChainlinkReplay& operator=(const ChainlinkReplay&) = delete;

    bool open(const char* path) noexcept {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
        file_ = std::fopen(path, "r");
        return file_ != nullptr;
    }

    // Reads the next non-empty line and parses it. Returns Ok on success,
    // NotJson at end of file, and the parser's verdict otherwise.
    ChainlinkParse next(double price_scale, ChainlinkReport& out) noexcept {
        if (!file_) return ChainlinkParse::NotJson;
        while (std::fgets(line_, sizeof(line_), file_)) {
            size_t len = std::strlen(line_);
            while (len > 0 && (line_[len - 1] == '\n' || line_[len - 1] == '\r'))
                line_[--len] = '\0';
            if (len == 0 || line_[0] == '#') continue;
            return parse_chainlink_report(line_, len, price_scale, out);
        }
        return ChainlinkParse::NotJson;
    }

private:
    std::FILE* file_ = nullptr;
    char line_[4096]{};
};

}  // namespace crowdintel

#endif  // CROWDINTEL_CHAINLINK_STREAM_HPP
