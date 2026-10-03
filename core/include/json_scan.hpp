#ifndef JSON_SCAN_HPP
#define JSON_SCAN_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Allocation-free JSON field scanner for bounded transport buffers.
//
// Scope: this is not a general JSON DOM.  It answers the questions the venue
// adapters need — "what is the value of key K inside this byte range", "where
// does the object/array for key K start and end", "does K appear more than
// once" — over a buffer whose length is already bounded by the transport.
// Syntax validation stays in bounded_json; every accessor here refuses
// ambiguous input instead of guessing:
//   * keys are matched as complete JSON strings followed by ':';
//   * strings are walked with escape awareness, so a key inside a string
//     literal never matches;
//   * ranges can be narrowed to a located object/array, which keeps a
//     same-named nested key from being read as an outer field;
//   * duplicate keys in the scanned range are rejected, not resolved.
// Numbers are returned as their literal text so callers can apply exact
// decimal → fixed-point conversion (never binary floating point).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace json_scan {

enum class Kind : uint8_t { NONE = 0, OBJECT, ARRAY, STRING, NUMBER, BOOL, NULL_VALUE };

inline bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Advances past a JSON string starting at data[i] == '"'.  Returns the index
// just past the closing quote, or 0 on malformed input.
inline size_t skip_string(const char* data, size_t len, size_t i) noexcept {
    if (i >= len || data[i] != '"') return 0;
    ++i;
    while (i < len) {
        const char c = data[i];
        if (c == '\\') {
            if (i + 1 >= len) return 0;
            const char esc = data[i + 1];
            if (esc == 'u') {
                for (int k = 2; k < 6; ++k)
                    if (i + static_cast<size_t>(k) >= len) return 0;
                i += 6;
                continue;
            }
            if (esc != '"' && esc != '\\' && esc != '/' && esc != 'b' &&
                esc != 'f' && esc != 'n' && esc != 'r' && esc != 't')
                return 0;
            i += 2;
            continue;
        }
        if (c == '"') return i + 1;
        ++i;
    }
    return 0;
}

// Advances past the value starting at data[i] (any JSON type).  Returns the
// index just past the value, or 0 on malformed input.
inline size_t skip_value(const char* data, size_t len, size_t i) noexcept {
    while (i < len && is_space(data[i])) ++i;
    if (i >= len) return 0;
    switch (data[i]) {
        case '"': return skip_string(data, len, i);
        case '{':
        case '[': {
            const char open = data[i];
            const char close = open == '{' ? '}' : ']';
            size_t depth = 0;
            while (i < len) {
                const char c = data[i];
                if (c == '"') {
                    const size_t after = skip_string(data, len, i);
                    if (!after) return 0;
                    i = after;
                    continue;
                }
                if (c == open) ++depth;
                else if (c == close) {
                    --depth;
                    ++i;
                    if (depth == 0) return i;
                    continue;
                }
                ++i;
            }
            return 0;
        }
        default: {
            size_t j = i;
            while (j < len) {
                const char c = data[j];
                if (c == ',' || c == '}' || c == ']' || is_space(c)) break;
                ++j;
            }
            return j == i ? 0 : j;
        }
    }
}

// Counts occurrences of "key": in the range.  Used to reject documents whose
// duplicate keys make the venue's intent ambiguous (an observed CLOB failure
// mode is a doubled "event_type").
inline size_t count_key(const char* data, size_t len, const char* key) noexcept {
    if (!data || !key) return 0;
    const size_t key_len = std::strlen(key);
    if (key_len == 0 || key_len + 3 > len) return 0;
    size_t matches = 0;
    size_t i = 0;
    while (i < len) {
        const char c = data[i];
        if (c == '"') {
            const size_t after = skip_string(data, len, i);
            if (!after) return matches;
            // Only a top-level (unescaped-delimited) string can be a key here.
            if (after - i == key_len + 2 &&
                std::memcmp(data + i + 1, key, key_len) == 0) {
                size_t j = after;
                while (j < len && is_space(data[j])) ++j;
                if (j < len && data[j] == ':') ++matches;
            }
            i = after;
            continue;
        }
        ++i;
    }
    return matches;
}

// Locates `key` in the range and reports the raw value span plus its kind.
inline bool find_value(const char* data, size_t len, const char* key,
                       size_t& value_start, size_t& value_end,
                       Kind& kind) noexcept {
    value_start = 0;
    value_end = 0;
    kind = Kind::NONE;
    if (!data || !key || len == 0) return false;
    const size_t key_len = std::strlen(key);
    if (key_len == 0) return false;
    size_t i = 0;
    while (i < len) {
        const char c = data[i];
        if (c != '"') { ++i; continue; }
        const size_t after = skip_string(data, len, i);
        if (!after) return false;
        const size_t literal = after - i;
        const bool key_matches = literal == key_len + 2 &&
                                 std::memcmp(data + i + 1, key, key_len) == 0;
        if (!key_matches) { i = after; continue; }
        size_t j = after;
        while (j < len && is_space(data[j])) ++j;
        if (j >= len || data[j] != ':') { i = after; continue; }
        ++j;
        while (j < len && is_space(data[j])) ++j;
        if (j >= len) return false;
        const size_t end = skip_value(data, len, j);
        if (!end) return false;
        value_start = j;
        value_end = end;
        switch (data[j]) {
            case '{': kind = Kind::OBJECT; break;
            case '[': kind = Kind::ARRAY; break;
            case '"': kind = Kind::STRING; break;
            case 't':
            case 'f': kind = Kind::BOOL; break;
            case 'n': kind = Kind::NULL_VALUE; break;
            default: kind = Kind::NUMBER; break;
        }
        return true;
    }
    return false;
}

inline bool find_value(const char* data, size_t len, const char* key,
                       size_t& value_start, size_t& value_end) noexcept {
    Kind kind = Kind::NONE;
    return find_value(data, len, key, value_start, value_end, kind);
}

// Narrows the scan range to the object value of `key`.
inline bool find_object(const char* data, size_t len, const char* key,
                        size_t& start, size_t& end) noexcept {
    Kind kind = Kind::NONE;
    if (!find_value(data, len, key, start, end, kind)) return false;
    return kind == Kind::OBJECT;
}

inline bool find_array(const char* data, size_t len, const char* key,
                       size_t& start, size_t& end) noexcept {
    Kind kind = Kind::NONE;
    if (!find_value(data, len, key, start, end, kind)) return false;
    return kind == Kind::ARRAY;
}

// Returns the span of the `index`-th element of the array in [start,end).
inline bool array_element(const char* data, size_t len, size_t start, size_t end,
                          size_t index, size_t& elem_start,
                          size_t& elem_end) noexcept {
    elem_start = 0;
    elem_end = 0;
    if (start >= end || start >= len || end > len || data[start] != '[') return false;
    size_t i = start + 1;
    size_t seen = 0;
    while (i < end) {
        while (i < end && (is_space(data[i]) || data[i] == ',')) ++i;
        if (i >= end || data[i] == ']') return false;
        const size_t after = skip_value(data, len, i);
        if (!after || after > end) return false;
        if (seen == index) {
            elem_start = i;
            elem_end = after;
            return true;
        }
        ++seen;
        i = after;
    }
    return false;
}

inline size_t array_count(const char* data, size_t len, size_t start,
                          size_t end) noexcept {
    if (start >= end || start >= len || end > len || data[start] != '[') return 0;
    size_t i = start + 1;
    size_t count = 0;
    while (i < end) {
        while (i < end && (is_space(data[i]) || data[i] == ',')) ++i;
        if (i >= end || data[i] == ']') break;
        const size_t after = skip_value(data, len, i);
        if (!after || after > end) return count;
        ++count;
        i = after;
    }
    return count;
}

// ── Scalar readers (range-scoped) ───────────────────────────────────────────
inline bool get_string(const char* data, size_t len, const char* key, char* out,
                       size_t cap) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    size_t start = 0;
    size_t end = 0;
    Kind kind = Kind::NONE;
    if (!find_value(data, len, key, start, end, kind)) return false;
    if (kind != Kind::STRING || end - start < 2) return false;
    const size_t body = end - start - 2;  // strip the quotes
    if (body + 1 > cap) return false;
    // Reject escapes: identifiers from the venue are plain ASCII and an
    // escaped payload would need decoding this scanner does not do.
    for (size_t i = start + 1; i + 1 < end; ++i)
        if (data[i] == '\\') return false;
    std::memcpy(out, data + start + 1, body);
    out[body] = '\0';
    return true;
}

inline bool get_bool(const char* data, size_t len, const char* key,
                     bool& out) noexcept {
    size_t start = 0;
    size_t end = 0;
    Kind kind = Kind::NONE;
    if (!find_value(data, len, key, start, end, kind)) return false;
    if (kind != Kind::BOOL) return false;
    const size_t span = end - start;
    if (span == 4 && std::memcmp(data + start, "true", 4) == 0) { out = true; return true; }
    if (span == 5 && std::memcmp(data + start, "false", 5) == 0) { out = false; return true; }
    return false;
}

inline bool get_number_raw(const char* data, size_t len, const char* key,
                           const char*& out, size_t& out_len) noexcept {
    size_t start = 0;
    size_t end = 0;
    Kind kind = Kind::NONE;
    if (!find_value(data, len, key, start, end, kind)) return false;
    if (kind != Kind::NUMBER) return false;
    out = data + start;
    out_len = end - start;
    return true;
}

inline bool get_i64(const char* data, size_t len, const char* key,
                    int64_t& out) noexcept {
    const char* text = nullptr;
    size_t text_len = 0;
    if (!get_number_raw(data, len, key, text, text_len)) return false;
    if (text_len == 0 || text_len > 19) return false;
    size_t i = 0;
    bool negative = false;
    if (text[0] == '-') { negative = true; i = 1; }
    if (i >= text_len) return false;
    int64_t value = 0;
    for (; i < text_len; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const int64_t digit = text[i] - '0';
        if (value > (INT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = negative ? -value : value;
    return true;
}

inline bool get_u64(const char* data, size_t len, const char* key,
                    uint64_t& out) noexcept {
    const char* text = nullptr;
    size_t text_len = 0;
    if (!get_number_raw(data, len, key, text, text_len)) return false;
    if (text_len == 0 || text_len > 20) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < text_len; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// ── Exact decimal text → fixed point ────────────────────────────────────────
// The venue sends prices, sizes and tick sizes as decimal strings ("0.01",
// ".49", "5").  Conversion must be exact: anything that cannot be represented
// in the requested scale is rejected rather than rounded, because a silently
// rounded price or tick size produces an order the venue rejects or, worse,
// accepts at the wrong price.
inline bool parse_fixed(const char* text, size_t len, uint64_t scale,
                        uint64_t& out) noexcept {
    if (!text || len == 0 || scale == 0) return false;
    // `scale` must be a power of ten; its exponent is the number of decimals
    // the fixed-point representation carries.
    size_t scale_digits = 0;
    {
        uint64_t remaining = scale;
        while (remaining > 1) {
            if (remaining % 10 != 0) return false;
            remaining /= 10;
            ++scale_digits;
        }
    }
    size_t i = 0;
    if (text[i] == '-') return false;  // negative prices/sizes are never valid
    if (text[i] == '+') ++i;
    uint64_t whole = 0;
    bool digits = false;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
        if (whole > (UINT64_MAX - digit) / 10) return false;
        whole = whole * 10 + digit;
        digits = true;
        ++i;
    }
    uint64_t fraction = 0;
    size_t fraction_digits = 0;
    if (i < len && text[i] == '.') {
        ++i;
        while (i < len && text[i] >= '0' && text[i] <= '9') {
            const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
            if (fraction_digits < scale_digits) {
                if (fraction > (UINT64_MAX - digit) / 10) return false;
                fraction = fraction * 10 + digit;
                ++fraction_digits;
            } else if (digit != 0) {
                // More precision than the scale can hold.  Reject instead of
                // rounding: a rounded price/tick size is a wrong order.
                return false;
            }
            digits = true;
            ++i;
        }
        if (i != len) return false;  // exponents / trailing garbage rejected
        while (fraction_digits < scale_digits) {
            fraction *= 10;
            ++fraction_digits;
        }
    } else if (i != len) {
        return false;
    }
    if (!digits) return false;
    uint64_t factor = 1;
    for (size_t k = 0; k < scale_digits; ++k) factor *= 10;
    if (whole > UINT64_MAX / factor) return false;
    const uint64_t scaled_whole = whole * factor;
    if (fraction > UINT64_MAX - scaled_whole) return false;
    out = scaled_whole + fraction;
    return true;
}

inline bool get_fixed(const char* data, size_t len, const char* key, uint64_t scale,
                      uint64_t& out) noexcept {
    const char* text = nullptr;
    size_t text_len = 0;
    if (!get_number_raw(data, len, key, text, text_len)) {
        // Venues quote these values either as numbers or as strings; accept
        // both but never silently coerce one into the other.
        char buffer[64];
        if (!get_string(data, len, key, buffer, sizeof(buffer))) return false;
        return parse_fixed(buffer, std::strlen(buffer), scale, out);
    }
    return parse_fixed(text, text_len, scale, out);
}

// Validates that a text is a plain non-negative decimal integer (token ids are
// uint256 decimal strings that do not fit in 64 bits, so they stay as text).
inline bool is_decimal_integer(const char* text, size_t len) noexcept {
    if (!text || len == 0 || len > 78) return false;
    for (size_t i = 0; i < len; ++i)
        if (text[i] < '0' || text[i] > '9') return false;
    return true;
}

inline bool is_hex_bytes(const char* text, size_t len, size_t expected_bytes) noexcept {
    if (!text || len != expected_bytes * 2 + 2) return false;
    if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return false;
    for (size_t i = 2; i < len; ++i) {
        const char c = text[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                        (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

}  // namespace json_scan

#endif  // JSON_SCAN_HPP
