#ifndef POLYMARKET_ORDER_HPP
#define POLYMARKET_ORDER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Polymarket CLOB V2 order model.
//
// All quantities are uint64 fixed-point ×1e6 (USDC and shares both use 6
// decimals on Polygon). tokenId is a uint256 kept as a 32-byte big-endian
// value plus its decimal string (computed once at startup — uint256 cannot
// live in a uint64).
//
// Amount math (canonical, mirrors py-clob-client-v2):
//   BUY : makerAmount = round(price × size)   [USDC], takerAmount = size [shares]
//   SELL: makerAmount = size [shares],        takerAmount = round(price × size) [USDC]
//   where price and size are raw (price_u×size_u/1e6, computed in __int128).
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ── Signed struct (V2) ───────────────────────────────────────────────────────
struct OrderV2 {
    uint64_t salt;                       // random 56-bit (JSON-safe integer)
    uint8_t  maker[20];                  // funder / deposit wallet
    uint8_t  signer[20];                 // address of the signing EOA
    uint8_t  token_id[32];               // uint256 big-endian (ERC-1155 position id)
    uint64_t maker_amount;               // raw 6-decimal units
    uint64_t taker_amount;               // raw 6-decimal units
    uint8_t  side;                       // 0 = BUY, 1 = SELL (in the signed payload)
    uint8_t  signature_type;             // 0 EOA, 1 POLY_PROXY, 2 POLY_GNOSIS_SAFE, 3 POLY_1271
    uint64_t timestamp_ms;               // uniqueness source (V2 removed nonces)
    uint8_t  metadata[32] = {0};         // bytes32 (zero)
    uint8_t  builder[32]  = {0};         // bytes32 (zero unless builder code)
};

// Side codes
inline constexpr uint8_t K_SIDE_BUY  = 0;
inline constexpr uint8_t K_SIDE_SELL = 1;

// Result of a submission (shared by the real and mock clients).
struct SubmitResult {
    bool     ok;
    long     http_code;
    char     order_id[80];   // CLOB orderID when the response carries one
};

// ── ABI encoding for hashing (11 static slots × 32 bytes) ────────────────────
inline void abi_encode_order(const OrderV2& o, uint8_t out[32 * 11]) {
    auto put_u64 = [](uint8_t* slot, uint64_t v) {
        std::memset(slot, 0, 24);
        for (int i = 0; i < 8; ++i) slot[31 - i] = (uint8_t)(v >> (8 * i));
    };
    auto put_addr = [](uint8_t* slot, const uint8_t a[20]) {
        std::memset(slot, 0, 12);
        std::memcpy(slot + 12, a, 20);
    };
    auto put_32 = [](uint8_t* slot, const uint8_t v[32]) {
        std::memcpy(slot, v, 32);
    };
    put_u64 (out + 0,   o.salt);
    put_addr(out + 32,  o.maker);
    put_addr(out + 64,  o.signer);
    put_32 (out + 96,   o.token_id);
    put_u64 (out + 128, o.maker_amount);
    put_u64 (out + 160, o.taker_amount);
    put_u64 (out + 192, o.side);           // uint8 ABI-encoded into a 32-byte slot
    put_u64 (out + 224, o.signature_type); // ditto
    put_u64 (out + 256, o.timestamp_ms);
    put_32 (out + 288,  o.metadata);
    put_32 (out + 320,  o.builder);
}

// ── Decimal/fixed-point helpers ──────────────────────────────────────────────
// "0.485" / ".48" / "30" → uint64 ×1e6. Returns false on malformed input.
inline bool parse_fixed1e6(const char* s, size_t len, uint64_t& out) {
    if (!s || len == 0 || len > 24) return false;
    uint64_t int_part = 0;
    size_t i = 0;
    bool any = false;
    for (; i < len && s[i] != '.'; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        int_part = int_part * 10 + (uint64_t)(s[i] - '0');
        any = true;
    }
    uint64_t frac = 0;
    int frac_digits = 0;
    if (i < len && s[i] == '.') {
        ++i;
        for (; i < len; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            if (frac_digits < 6) {
                frac = frac * 10 + (uint64_t)(s[i] - '0');
                ++frac_digits;
            } else if (s[i] != '0') {
                return false;  // beyond 1e6 precision — reject, do not truncate
            }
        }
    }
    if (!any && frac_digits == 0) return false;
    for (int d = frac_digits; d < 6; ++d) frac *= 10;
    out = int_part * 1000000ULL + frac;
    return true;
}

// Round a fixed-point price to the tick grid, clamped to [tick, 1−tick].
inline uint64_t round_price_to_tick(uint64_t price, uint64_t tick) {
    if (tick == 0) tick = 10000;  // 0.01 default — never divide by zero
    const uint64_t max_p = 1000000ULL - tick;
    uint64_t p = (price / tick) * tick;
    if (p < tick) p = tick;
    if (p > max_p) p = max_p;
    return p;
}

// price_u × size_u / 1e6 in __int128 (never overflows for sane orders).
inline uint64_t product_scaled(uint64_t price_u, uint64_t size_u) {
    const unsigned __int128 v =
        (unsigned __int128)price_u * (unsigned __int128)size_u / 1000000ULL;
    return (uint64_t)v;
}

// Fill maker/taker amounts for a side. Returns false on degenerate size.
inline bool compute_amounts(uint8_t side, uint64_t price_u, uint64_t size_u,
                            uint64_t& maker_amount, uint64_t& taker_amount) {
    if (size_u == 0 || price_u == 0 || price_u >= 1000000ULL) return false;
    if (side == K_SIDE_BUY) {
        maker_amount = product_scaled(price_u, size_u);  // USDC
        taker_amount = size_u;                           // shares
    } else {
        maker_amount = size_u;                           // shares
        taker_amount = product_scaled(price_u, size_u);  // USDC
    }
    return maker_amount > 0 && taker_amount > 0;
}

// ── uint256 decimal (tokenId) ↔ 32-byte BE ───────────────────────────────────
// Parses a decimal string (up to 78 digits) into 32-byte big-endian.
inline bool parse_uint256_dec(const char* s, size_t len, uint8_t out32[32]) {
    std::memset(out32, 0, 32);
    if (len == 0 || len > 78) return false;
    // Schoolbook: value = value*10 + digit over 4×64-bit limbs (little-endian).
    uint64_t limbs[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        const uint64_t d = (uint64_t)(s[i] - '0');
        // ×10 with carry
        unsigned __int128 carry = d;
        for (int l = 0; l < 4; ++l) {
            const unsigned __int128 v =
                (unsigned __int128)limbs[l] * 10ULL + carry;
            limbs[l] = (uint64_t)v;
            carry = v >> 64;
        }
        if (carry) return false;  // overflow past 256 bits
    }
    for (int l = 0; l < 4; ++l)
        for (int b = 0; b < 8; ++b)
            out32[31 - (l * 8 + b)] = (uint8_t)(limbs[l] >> (8 * b));
    return true;
}

// ── Hex helpers ──────────────────────────────────────────────────────────────
inline char hex_lo(uint8_t b) { return (char)("0123456789abcdef"[b & 0xF]); }
inline char hex_hi(uint8_t b) { return (char)("0123456789abcdef"[b >> 4]); }

inline size_t bytes_to_hex(const uint8_t* in, size_t n, char* out) {
    for (size_t i = 0; i < n; ++i) {
        out[i * 2]     = hex_hi(in[i]);
        out[i * 2 + 1] = hex_lo(in[i]);
    }
    return n * 2;
}

inline bool parse_hex_bytes(const char* s, size_t len, uint8_t* out, size_t out_len) {
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; len -= 2; }
    if (len != out_len * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out_len; ++i) {
        const int hi = nib(s[i * 2]), lo = nib(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// ── u64 → decimal digits (returns length) ────────────────────────────────────
inline size_t u64_to_dec(uint64_t v, char* out) {
    char tmp[20];
    size_t n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    return n;
}

// ── Wire body (exact field set/order of the official V2 "Place Orders" doc) ──
// {"deferExec":false,"order":{"builder":...,"expiration":"0","maker":...,
//  "makerAmount":...,"metadata":...,"salt":...,"side":"BUY","signature":...,
//  "signatureType":N,"signer":...,"takerAmount":...,"timestamp":...,
//  "tokenId":...},"orderType":"GTC","owner":"..."}
struct WireBody {
    char   buf[1536];
    size_t len;
};

inline bool build_wire_body(const OrderV2& o, const uint8_t sig65[65],
                            const char* token_id_dec,   // precomputed decimal string
                            const char* maker_hex,      // precomputed "0x..." (40)
                            const char* signer_hex,     // precomputed "0x..." (40)
                            const char* owner_api_key,
                            const char* order_type,     // GTC | GTD | FOK | FAK
                            WireBody& out) {
    char* p = out.buf;
    *p++ = '{';
    // deferExec
    std::memcpy(p, "\"deferExec\":false,\"order\":{", 27); p += 27;
    // builder
    std::memcpy(p, "\"builder\":\"0x", 13); p += 13;
    p += bytes_to_hex(o.builder, 32, p);
    *p++ = '"';
    // expiration (wire-only field for GTD handling; not part of signed struct)
    std::memcpy(p, ",\"expiration\":\"0\"", 17); p += 17;
    // maker
    std::memcpy(p, ",\"maker\":\"", 10); p += 10;
    std::memcpy(p, maker_hex, 42); p += 42;   // includes 0x
    *p++ = '"';
    // makerAmount
    std::memcpy(p, ",\"makerAmount\":\"", 16); p += 16;
    p += u64_to_dec(o.maker_amount, p);
    *p++ = '"';
    // metadata
    std::memcpy(p, ",\"metadata\":\"0x", 15); p += 15;
    p += bytes_to_hex(o.metadata, 32, p);
    *p++ = '"';
    // salt (JSON integer)
    std::memcpy(p, ",\"salt\":", 8); p += 8;
    p += u64_to_dec(o.salt, p);
    // side
    if (o.side == K_SIDE_BUY) { std::memcpy(p, ",\"side\":\"BUY\"", 13); p += 13; }
    else                      { std::memcpy(p, ",\"side\":\"SELL\"", 14); p += 14; }
    // signature
    std::memcpy(p, ",\"signature\":\"0x", 16); p += 16;
    p += bytes_to_hex(sig65, 65, p);
    *p++ = '"';
    // signatureType (JSON integer)
    std::memcpy(p, ",\"signatureType\":", 17); p += 17;
    p += u64_to_dec(o.signature_type, p);
    // signer
    std::memcpy(p, ",\"signer\":\"", 11); p += 11;
    std::memcpy(p, signer_hex, 42); p += 42;
    *p++ = '"';
    // takerAmount
    std::memcpy(p, ",\"takerAmount\":\"", 16); p += 16;
    p += u64_to_dec(o.taker_amount, p);
    *p++ = '"';
    // timestamp (ms, string)
    std::memcpy(p, ",\"timestamp\":\"", 14); p += 14;
    p += u64_to_dec(o.timestamp_ms, p);
    *p++ = '"';
    // tokenId (decimal string)
    std::memcpy(p, ",\"tokenId\":\"", 12); p += 12;
    const size_t tl = std::strlen(token_id_dec);
    std::memcpy(p, token_id_dec, tl); p += tl;
    *p++ = '"';
    // close order + wrapper
    std::memcpy(p, "},\"orderType\":\"", 15); p += 15;
    const size_t ol = std::strlen(order_type);
    std::memcpy(p, order_type, ol); p += ol;
    *p++ = '"';
    std::memcpy(p, ",\"owner\":\"", 10); p += 10;
    const size_t ow = std::strlen(owner_api_key);
    std::memcpy(p, owner_api_key, ow); p += ow;
    std::memcpy(p, "\"}", 2); p += 2;

    out.len = (size_t)(p - out.buf);
    return out.len < sizeof(out.buf);
}

#endif // POLYMARKET_ORDER_HPP
