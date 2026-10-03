#ifndef ACCOUNT_STATE_HPP
#define ACCOUNT_STATE_HPP

// Phase 6 — account state: balance and allowances as the venue reports them.
//
// Why this exists: the CLOB rejects an order with "not enough balance /
// allowance" (docs.polymarket.com/resources/error-codes). That rejection is
// expensive — the intent is already journaled and the order is already
// unproven — so the account has to be checked *before* arming, and the answer
// has to come from the venue, never from a local assumption.
//
// Wire contract (verified, not invented):
//   docs.polymarket.com/api-spec/clob-openapi.yaml, GET /balance-allowance
//     required: [balance, allowances]
//     balance    string  "Balance amount in fixed-math with 6 decimals"
//     allowances object  "Map of spender addresses to allowance amounts"
//   Same spec documents the query parameters asset_type (COLLATERAL |
//   CONDITIONAL), token_id (defaults to "-1" for the ERC20 collateral) and
//   signature_type (0=EOA, 1=POLY_PROXY, 2=POLY_GNOSIS_SAFE; default 0), and
//   the errors "Invalid asset type" / "Invalid signature_type" as HTTP 400.
//   The published example for `balance` is '1000000000000000000' (1e18), which
//   cannot be a 6-decimal fixed value of a plausible balance: the example is
//   illustrative, the *description* is the contract. Values are therefore
//   parsed as exact 6-decimal fixed point and anything not representable that
//   way is rejected rather than rounded.
//
// Spender addresses: the keys of `allowances` are the contracts approved to
// move the asset. The repo already carries the canonical exchange addresses
// (core/crypto/eip712_signer.hpp:65-70) and the resolver installs the selected
// one in MarketConfig::runtime.exchange, so nothing here has to guess an
// address.
//
// What this file does NOT do: it does not decide tradability. It parses the
// venue's answer and answers one question per asset: is there enough of it, and
// is the spender that the order will name actually approved for it.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/json_field.hpp"

namespace account {

// The active exchange plus, at most, the other exchange, an adapter and a
// couple of spares. More spenders than this means the payload is not the shape
// this build understands: it is rejected instead of partially trusted.
inline constexpr size_t kMaxSpenders = 8;
inline constexpr size_t kAddressChars = 43;  // 0x + 40 hex + NUL

struct Spender {
    char address[kAddressChars]{};  // lowercase 0x + 40 hex
    uint64_t amount_f6 = 0;         // 6-decimal fixed point
    bool unlimited = false;         // the venue's "max" sentinel
};

struct State {
    uint64_t balance_f6 = 0;
    Spender spenders[kMaxSpenders]{};
    size_t spender_count = 0;       // parsed entries (only on success)
    uint32_t declared_spenders = 0; // entries seen on the wire
    bool unlimited_sentinel = false;
};

// ── small strict helpers ─────────────────────────────────────────────────────

inline bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

inline char lower_hex(char c) noexcept {
    return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Canonicalizes an EVM address: exactly 0x + 40 hex digits, lowercase.
inline bool canonical_address(const char* text, char out[kAddressChars]) noexcept {
    if (!text) return false;
    // Length first: callers may hold shorter literals, and a length check also
    // rejects anything that is not exactly one address.
    if (std::strlen(text) != 42) return false;
    if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return false;
    for (size_t i = 0; i < 40; ++i)
        if (!is_hex_digit(text[2 + i])) return false;
    out[0] = '0';
    out[1] = 'x';
    for (size_t i = 0; i < 40; ++i) out[2 + i] = lower_hex(text[2 + i]);
    out[42] = '\0';
    return true;
}

inline bool address_equal(const char* a, const char* b) noexcept {
    if (!a || !b) return false;
    size_t i = 0;
    for (; a[i] && b[i]; ++i) {
        if (a[i] != b[i]) return false;
    }
    return a[i] == '\0' && b[i] == '\0';
}

// A quoted string token at `p` ("..." with no interior escapes for the shapes
// this endpoint returns). Advances `p` past the closing quote.
inline bool take_string(const char*& p, const char* end, char* out,
                        size_t cap) noexcept {
    if (p >= end || *p != '"') return false;
    const char* start = p;
    ++p;
    bool closed = false;
    while (p < end) {
        if (*p == '\\') {
            p += 2;
            continue;
        }
        if (*p == '"') {
            closed = true;
            break;
        }
        ++p;
    }
    if (!closed) return false;
    json_field::Span span{start, p + 1};
    ++p;
    return json_field::unescape(span, out, cap);
}

inline void skip_spaces(const char*& p, const char* end) noexcept {
    while (p < end && json_field::is_space(*p)) ++p;
}

// Parses the `allowances` object: { "0x…": "123", "0x…": "max" }.
// Rejects a missing/duplicated key, a non-object, a malformed address, a
// repeated spender (case variants included), an unparsable amount, and more
// entries than kMaxSpenders.
inline bool parse_allowances(const char* json, size_t length,
                             State& state) noexcept {
    json_field::Span object;
    if (!json_field::unique(json, length, "allowances", object)) return false;
    if (!object.valid() || object.size() < 2 || *object.begin != '{' ||
        object.end[-1] != '}')
        return false;

    const char* p = object.begin + 1;
    const char* end = object.end - 1;
    bool need_pair = true;
    bool any = false;
    while (true) {
        skip_spaces(p, end);
        if (p == end) return !need_pair || !any;
        if (!need_pair) {
            if (*p != ',') return false;
            ++p;
            need_pair = true;
            continue;
        }

        char address_raw[64];
        if (!take_string(p, end, address_raw, sizeof(address_raw))) return false;
        skip_spaces(p, end);
        if (p >= end || *p != ':') return false;
        ++p;
        skip_spaces(p, end);

        char value_raw[40];
        if (!take_string(p, end, value_raw, sizeof(value_raw))) return false;

        ++state.declared_spenders;
        if (state.declared_spenders > kMaxSpenders) return false;

        Spender entry;
        if (!canonical_address(address_raw, entry.address)) return false;
        for (size_t i = 0; i < state.spender_count; ++i)
            if (address_equal(state.spenders[i].address, entry.address))
                return false;  // duplicate spender: ambiguous evidence

        if (std::strcmp(value_raw, "max") == 0) {
            entry.unlimited = true;
            entry.amount_f6 = UINT64_MAX;
            state.unlimited_sentinel = true;
        } else {
            if (!json_field::parse_fixed6_text(value_raw,
                                               std::strlen(value_raw),
                                               entry.amount_f6))
                return false;
        }
        state.spenders[state.spender_count++] = entry;
        need_pair = false;
        any = true;
    }
}

// Parses one BalanceAllowanceResponse. Returns false on any deviation from the
// documented shape; `out` is only meaningful when true.
inline bool parse_balance_allowance(const char* json, size_t length,
                                    State& out) noexcept {
    if (!json || length == 0) return false;
    State state;
    if (!json_field::fixed6(json, length, "balance", state.balance_f6))
        return false;
    if (!parse_allowances(json, length, state)) return false;
    out = state;
    return true;
}

inline bool find_allowance(const State& state, const char* address_hex,
                           uint64_t& amount_f6, bool& unlimited) noexcept {
    amount_f6 = 0;
    unlimited = false;
    if (!address_hex || address_hex[0] == '\0') return false;
    char canonical[kAddressChars];
    if (!canonical_address(address_hex, canonical)) return false;
    for (size_t i = 0; i < state.spender_count; ++i) {
        if (address_equal(state.spenders[i].address, canonical)) {
            amount_f6 = state.spenders[i].amount_f6;
            unlimited = state.spenders[i].unlimited;
            return true;
        }
    }
    return false;
}

}  // namespace account

#endif  // ACCOUNT_STATE_HPP
