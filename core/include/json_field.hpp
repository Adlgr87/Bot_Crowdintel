#ifndef JSON_FIELD_HPP
#define JSON_FIELD_HPP

// Bounded, allocation-free semantic field access for already size-limited JSON
// responses (venue REST payloads, recorded fixtures).
//
// Design rules:
//   * syntax is validated first by bounded_json::valid_document();
//   * every lookup is *top-level only* (object depth 1) and rejects duplicate
//     keys instead of silently picking one;
//   * strings are unescaped into fixed buffers and reject unknown escapes;
//   * numbers are parsed from the raw token (no locale-dependent strtod), and
//     fixed-point helpers reject values that are not exact multiples of 1e-6.
//
// This header deliberately does not own policy. Callers decide what a missing
// or duplicated field means for their protocol (fail closed, in this repo).

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "bounded_json.hpp"

namespace json_field {

struct Span {
    const char* begin = nullptr;
    const char* end = nullptr;  // exclusive
    size_t size() const { return static_cast<size_t>(end - begin); }
    bool valid() const { return begin && end && begin <= end; }
};

inline bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Advances `p` past a complete JSON value starting at `p`. Handles nested
// objects/arrays and escaped strings. Returns false on malformed input.
inline bool skip_value(const char*& p, const char* end) noexcept {
    while (p < end && is_space(*p)) ++p;
    if (p == end) return false;
    const char c = *p;
    if (c == '"') {
        ++p;
        while (p < end) {
            if (*p == '\\') {
                ++p;
                if (p == end) return false;
                ++p;
                continue;
            }
            if (*p == '"') { ++p; return true; }
            ++p;
        }
        return false;
    }
    if (c == '{' || c == '[') {
        const char close = c == '{' ? '}' : ']';
        int depth = 0;
        while (p < end) {
            const char d = *p;
            if (d == '"') {
                ++p;
                while (p < end) {
                    if (*p == '\\') { p += (p + 1 < end) ? 2 : 1; continue; }
                    if (*p == '"') break;
                    ++p;
                }
                if (p == end) return false;
                ++p;
                continue;
            }
            if (d == '{' || d == '[') ++depth;
            else if (d == '}' || d == ']') {
                --depth;
                if (depth == 0) { ++p; return d == close; }
            }
            ++p;
        }
        return false;
    }
    // Number / true / false / null: run until a structural character.
    const char* start = p;
    while (p < end && !is_space(*p) && *p != ',' && *p != '}' && *p != ']') ++p;
    return p > start;
}

// Counts top-level (depth 1) occurrences of `key` and captures the first value.
inline size_t locate(const char* json, size_t length, const char* key,
                     const char** first_value = nullptr) noexcept {
    if (first_value) *first_value = nullptr;
    if (!json || !key || !*key) return 0;
    const char* p = json;
    const char* end = json + length;
    size_t count = 0;
    int depth = 0;
    while (p < end) {
        const char c = *p;
        if (c == '"') {
            const char* key_begin = p + 1;
            const char* cursor = key_begin;
            bool escaped = false;
            while (cursor < end) {
                if (escaped) { escaped = false; ++cursor; continue; }
                if (*cursor == '\\') { escaped = true; ++cursor; continue; }
                if (*cursor == '"') break;
                ++cursor;
            }
            if (cursor == end) return count;
            const char* after = cursor + 1;
            while (after < end && is_space(*after)) ++after;
            const bool is_key = depth == 1 && after < end && *after == ':';
            if (is_key && static_cast<size_t>(cursor - key_begin) ==
                          std::strlen(key) &&
                std::memcmp(key_begin, key, std::strlen(key)) == 0) {
                if (count == 0 && first_value) {
                    const char* value = after + 1;
                    while (value < end && is_space(*value)) ++value;
                    *first_value = value;
                }
                ++count;
            }
            p = cursor + 1;
            continue;
        }
        if (c == '{' || c == '[') { ++depth; ++p; continue; }
        if (c == '}' || c == ']') { --depth; ++p; continue; }
        ++p;
    }
    return count;
}

// Returns the unique top-level value span for `key`.
// `duplicate` is set when the key appears more than once (ambiguous -> reject).
inline bool unique(const char* json, size_t length, const char* key,
                   Span& out, bool* duplicate = nullptr) noexcept {
    if (duplicate) *duplicate = false;
    const char* value = nullptr;
    const size_t count = locate(json, length, key, &value);
    if (count == 0 || !value) return false;
    if (count > 1) {
        if (duplicate) *duplicate = true;
        return false;
    }
    const char* cursor = value;
    if (!skip_value(cursor, json + length)) return false;
    out.begin = value;
    out.end = cursor;
    return true;
}

inline bool span_is_string(const Span& s) noexcept {
    return s.valid() && s.size() >= 2 && *s.begin == '"' && s.end[-1] == '"';
}

// Unescapes a JSON string span (including surrounding quotes) into `out`.
// Unknown escapes, embedded NULs, and truncation fail closed.
inline bool unescape(const Span& s, char* out, size_t cap) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!span_is_string(s)) return false;
    const char* p = s.begin + 1;
    const char* end = s.end - 1;
    size_t written = 0;
    while (p < end) {
        unsigned char c = static_cast<unsigned char>(*p++);
        if (c == '\\') {
            if (p == end) return false;
            const char esc = *p++;
            switch (esc) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                default: return false;  // \uXXXX is not needed by these payloads
            }
        }
        if (c == 0) return false;
        if (written + 1 >= cap) return false;
        out[written++] = static_cast<char>(c);
    }
    out[written] = '\0';
    return true;
}

inline bool string(const char* json, size_t length, const char* key,
                   char* out, size_t cap) noexcept {
    Span value;
    if (!unique(json, length, key, value)) return false;
    return unescape(value, out, cap);
}

// Parses the raw numeric token of a span. Rejects NaN/Inf, leading '+',
// hex, whitespace, and trailing garbage. `integral` reports a zero fraction.
inline bool parse_number(const Span& s, double& out, bool* integral = nullptr) noexcept {
    out = 0.0;
    if (integral) *integral = false;
    if (!s.valid() || s.size() == 0 || s.size() > 64) return false;
    const char* p = s.begin;
    const char* end = s.end;
    bool negative = false;
    if (*p == '-') { negative = true; ++p; }
    if (p == end) return false;
    uint64_t int_part = 0;
    bool any_digit = false;
    bool overflow = false;
    while (p < end && *p >= '0' && *p <= '9') {
        any_digit = true;
        const uint64_t digit = static_cast<uint64_t>(*p - '0');
        if (int_part > (UINT64_MAX - digit) / 10) overflow = true;
        else int_part = int_part * 10 + digit;
        ++p;
    }
    if (!any_digit) return false;
    double fraction = 0.0;
    int fraction_digits = 0;
    if (p < end && *p == '.') {
        ++p;
        bool frac_digit = false;
        while (p < end && *p >= '0' && *p <= '9') {
            frac_digit = true;
            if (fraction_digits < 18) {
                fraction = fraction * 10.0 + static_cast<double>(*p - '0');
                ++fraction_digits;
            }
            ++p;
        }
        if (!frac_digit) return false;
    }
    int64_t exponent = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        bool exp_negative = false;
        if (p < end && (*p == '+' || *p == '-')) {
            exp_negative = *p == '-';
            ++p;
        }
        if (p == end || *p < '0' || *p > '9') return false;
        while (p < end && *p >= '0' && *p <= '9') {
            if (exponent < 100000) exponent = exponent * 10 + (*p - '0');
            ++p;
        }
        if (exp_negative) exponent = -exponent;
    }
    if (p != end) return false;
    if (overflow && int_part > (1ULL << 53)) {
        // Values this large cannot be represented exactly; callers that need
        // exactness must use the fixed-point helpers instead.
        return false;
    }
    double scale = 1.0;
    for (int i = 0; i < fraction_digits; ++i) scale *= 10.0;
    double value = static_cast<double>(int_part) + (scale > 1.0 ? fraction / scale : 0.0);
    if (exponent != 0) {
        double factor = 1.0;
        const int64_t magnitude = exponent < 0 ? -exponent : exponent;
        if (magnitude > 308) return false;
        for (int64_t i = 0; i < magnitude; ++i) factor *= 10.0;
        value = exponent < 0 ? value / factor : value * factor;
    }
    out = negative ? -value : value;
    if (integral) *integral = fraction_digits == 0 && exponent >= 0;
    return true;
}

inline bool number(const char* json, size_t length, const char* key,
                   double& out, bool* integral = nullptr) noexcept {
    Span value;
    if (!unique(json, length, key, value)) return false;
    if (span_is_string(value)) return false;  // strings are not numbers
    return parse_number(value, out, integral);
}

inline bool boolean(const char* json, size_t length, const char* key,
                    bool& out) noexcept {
    Span value;
    if (!unique(json, length, key, value)) return false;
    const size_t n = value.size();
    if (n == 4 && std::memcmp(value.begin, "true", 4) == 0) { out = true; return true; }
    if (n == 5 && std::memcmp(value.begin, "false", 5) == 0) { out = false; return true; }
    return false;
}

// Exact fixed-point 1e-6 helper for a value that may be a JSON number
// (0.01) or a JSON string ("0.01"). Fraction digits beyond 6 are rejected.
inline bool parse_fixed6_text(const char* text, size_t length, uint64_t& out) noexcept {
    out = 0;
    if (!text || length == 0 || length > 32) return false;
    size_t i = 0;
    if (text[i] == '+') return false;
    uint64_t whole = 0;
    size_t whole_digits = 0;
    while (i < length && text[i] >= '0' && text[i] <= '9') {
        whole = whole * 10 + static_cast<uint64_t>(text[i] - '0');
        if (whole > (UINT64_MAX / 1000000ULL)) return false;
        ++whole_digits;
        ++i;
    }
    if (whole_digits == 0 || whole_digits > 12) return false;
    uint64_t frac = 0;
    size_t frac_digits = 0;
    if (i < length) {
        if (text[i] != '.') return false;
        ++i;
        while (i < length && text[i] >= '0' && text[i] <= '9') {
            if (frac_digits >= 6) {
                if (text[i] != '0') return false;  // not exact at 1e-6
                ++i;
                continue;
            }
            frac = frac * 10 + static_cast<uint64_t>(text[i] - '0');
            ++frac_digits;
            ++i;
        }
        if (i != length && text[i] != '\0') return false;
    }
    while (frac_digits < 6) { frac *= 10; ++frac_digits; }
    out = whole * 1000000ULL + frac;
    return true;
}

// Accepts number tokens, string tokens, and integral tokens; rejects
// negatives, exponent notation, and anything not exact at 1e-6.
inline bool fixed6(const char* json, size_t length, const char* key,
                   uint64_t& out) noexcept {
    Span value;
    if (!unique(json, length, key, value)) return false;
    char buffer[40];
    if (span_is_string(value)) {
        if (!unescape(value, buffer, sizeof(buffer))) return false;
        return parse_fixed6_text(buffer, std::strlen(buffer), out);
    }
    if (value.size() >= sizeof(buffer)) return false;
    std::memcpy(buffer, value.begin, value.size());
    buffer[value.size()] = '\0';
    return parse_fixed6_text(buffer, value.size(), out);
}

// Parses a top-level array of numeric/string/integral values into fixed point.
template <size_t CAP>
inline size_t fixed6_array(const char* json, size_t length, const char* key,
                           uint64_t (&out)[CAP]) noexcept {
    for (auto& v : out) v = 0;
    Span array;
    if (!unique(json, length, key, array)) return 0;
    if (array.size() < 2 || *array.begin != '[' || array.end[-1] != ']') return 0;
    const char* p = array.begin + 1;
    const char* end = array.end - 1;
    size_t count = 0;
    while (p < end) {
        while (p < end && is_space(*p)) ++p;
        if (p == end) break;
        const char* start = p;
        if (!skip_value(p, end)) return 0;
        Span element{start, p};
        if (count < CAP) {
            char buffer[40];
            if (span_is_string(element)) {
                if (!unescape(element, buffer, sizeof(buffer))) return 0;
                if (!parse_fixed6_text(buffer, std::strlen(buffer), out[count])) return 0;
            } else {
                if (element.size() >= sizeof(buffer)) return 0;
                std::memcpy(buffer, element.begin, element.size());
                buffer[element.size()] = '\0';
                if (!parse_fixed6_text(buffer, element.size(), out[count])) return 0;
            }
            ++count;
        } else {
            return 0;  // more elements than the caller can hold: ambiguous
        }
        while (p < end && is_space(*p)) ++p;
        if (p < end) {
            if (*p != ',') return 0;
            ++p;
        }
    }
    return count;
}

// Parses a top-level array of strings. Used for market condition/token lists.
template <size_t WIDTH>
inline size_t string_array(const char* json, size_t length, const char* key,
                           char (&out)[WIDTH]) noexcept {
    Span array;
    if (!unique(json, length, key, array)) return 0;
    if (array.size() < 2 || *array.begin != '[' || array.end[-1] != ']') return 0;
    const char* p = array.begin + 1;
    const char* end = array.end - 1;
    size_t count = 0;
    while (p < end) {
        while (p < end && is_space(*p)) ++p;
        if (p == end) break;
        const char* start = p;
        if (!skip_value(p, end)) return 0;
        Span element{start, p};
        if (!span_is_string(element)) return 0;
        if (count == 0) {
            if (!unescape(element, out, WIDTH)) return 0;
        } else {
            return 0;  // single-value helper; multi-value callers use array_of
        }
        ++count;
        while (p < end && is_space(*p)) ++p;
        if (p < end) {
            if (*p != ',') return 0;
            ++p;
        }
    }
    return count;
}

// Single-string field whose value is itself a JSON array of strings, wrapped in
// a JSON string (Gamma: "clobTokenIds": "[\"1\",\"2\"]"). Values are written to
// out[0], out[1], ... (row-major, WIDTH bytes per row).
template <size_t WIDTH>
inline size_t embedded_string_array(const char* json, size_t length,
                                    const char* key, char* out, size_t cap) noexcept {
    if (!out || cap == 0 || WIDTH == 0) return 0;
    char buffer[2048];
    if (!string(json, length, key, buffer, sizeof(buffer))) return 0;
    const size_t inner_len = std::strlen(buffer);
    if (inner_len < 2 || !bounded_json::valid_document(buffer, inner_len)) return 0;
    const char* p = buffer;
    const char* end = buffer + inner_len;
    while (p < end && is_space(*p)) ++p;
    if (p == end || *p != '[') return 0;
    ++p;
    const char* last = end - 1;
    while (last > p && is_space(*last)) --last;
    if (*last != ']') return 0;
    size_t count = 0;
    while (p < last) {
        while (p < last && is_space(*p)) ++p;
        if (p == last) break;
        const char* start = p;
        if (!skip_value(p, last)) return 0;
        Span element{start, p};
        if (!span_is_string(element)) return 0;
        if (count >= cap) return 0;
        if (!unescape(element, out + count * WIDTH, WIDTH)) return 0;
        ++count;
        while (p < last && is_space(*p)) ++p;
        if (p < last) {
            if (*p != ',') return 0;
            ++p;
        }
    }
    return count;
}

// Field of the first element of a top-level array of objects, e.g.
// "t": [{"t":"123","o":"Yes"}, ...] -> first_object_string_field(doc, "t", "t").
inline bool first_object_string_field(const char* json, size_t length,
                                      const char* array_key, const char* field_key,
                                      char* out, size_t cap) noexcept {
    Span array;
    if (!unique(json, length, array_key, array)) return false;
    if (array.size() < 2 || *array.begin != '[' || array.end[-1] != ']')
        return false;
    const char* p = array.begin + 1;
    const char* end = array.end - 1;
    while (p < end && is_space(*p)) ++p;
    if (p == end || *p != '{') return false;
    const char* element_begin = p;
    int depth = 0;
    while (p < end) {
        const char c = *p;
        if (c == '"') {
            const char* q = p + 1;
            while (q < end) {
                if (*q == '\\') { q += 2; continue; }
                if (*q == '"') break;
                ++q;
            }
            if (q >= end) return false;
            p = q + 1;
            continue;
        }
        if (c == '{') ++depth;
        else if (c == '}') {
            --depth;
            if (depth == 0) { ++p; break; }
        }
        ++p;
    }
    if (depth != 0) return false;
    Span element{element_begin, p};
    Span value;
    if (!unique(element.begin, element.size(), field_key, value)) return false;
    return unescape(value, out, cap);
}

}  // namespace json_field

#endif  // JSON_FIELD_HPP
