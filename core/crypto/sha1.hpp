#ifndef SHA1_HPP
#define SHA1_HPP

// ─────────────────────────────────────────────────────────────────────────────
// SHA-1 — used ONLY for the RFC 6455 WebSocket handshake, where the client must
// validate Sec-WebSocket-Accept = base64(SHA1(key ‖ GUID)).  It is not used for
// any security decision, signature or integrity check: those use SHA-256 and
// secp256k1 (see sha256_engine.hpp / eip712_signer.hpp).
//
// Extracted verbatim from the validated market listener so that both WebSocket
// clients share one implementation, and pinned by a known-answer test using the
// RFC 6455 §1.3 vector and the FIPS 180-1 "abc" vector.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstring>

inline void sha1_block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = static_cast<uint32_t>(p[i * 4]) << 24 |
               static_cast<uint32_t>(p[i * 4 + 1]) << 16 |
               static_cast<uint32_t>(p[i * 4 + 2]) << 8 |
               static_cast<uint32_t>(p[i * 4 + 3]);
    for (int i = 16; i < 80; ++i) {
        const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = (v << 1) | (v >> 31);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    size_t offset = 0;
    while (offset + 64 <= len) {
        sha1_block(h, data + offset);
        offset += 64;
    }
    uint8_t tail[128]{};
    const size_t remaining = len - offset;
    if (remaining) std::memcpy(tail, data + offset, remaining);
    tail[remaining] = 0x80;
    const size_t final_offset = remaining >= 56 ? 120 : 56;
    for (int i = 0; i < 8; ++i)
        tail[final_offset + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    sha1_block(h, tail);
    if (remaining >= 56) sha1_block(h, tail + 64);
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

#endif  // SHA1_HPP
