#ifndef L2_AUTH_HPP
#define L2_AUTH_HPP

// L2 request authentication (HMAC) for the Polymarket CLOB API.
//
// Documented contract (docs.polymarket.com, "API Authentication" and every
// authenticated endpoint page):
//
//   message = timestamp + METHOD + path [+ exact request body]
//   signature = urlsafeBase64WithPadding(
//                   HMAC-SHA256(base64Decode(api_secret), message))
//
// The five headers are POLY_ADDRESS, POLY_API_KEY, POLY_PASSPHRASE,
// POLY_SIGNATURE and POLY_TIMESTAMP. The body must be byte-identical to what
// is sent over the wire: the signature covers the serialization, not the
// meaning.
//
// This header is pure and allocation-free so it can be shared by the order
// client and the heartbeat sender (duplicating signature code would be a
// security liability) and unit tested with published vectors.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"

namespace l2auth {

inline constexpr size_t kSecretMaxBytes = 128;
inline constexpr size_t kSignatureB64Chars = 44;  // 32 bytes -> base64url (43 + '=')

struct Credentials {
    const char* api_key = nullptr;
    const char* api_address_hex = nullptr;
    const char* secret_b64 = nullptr;   // base64url secret as delivered
    const char* passphrase = nullptr;
};

// Decodes and validates the API secret. Returns false for anything that is not
// a bounded, decodable base64url string.
inline bool decode_secret(const char* secret_b64, uint8_t* out, size_t cap,
                          size_t& out_len) noexcept {
    out_len = 0;
    if (!secret_b64 || !*secret_b64) return false;
    const size_t encoded_len = std::strlen(secret_b64);
    if (encoded_len > 256) return false;
    const size_t decoded = base64url_decode(secret_b64, encoded_len, out, cap);
    if (decoded == 0 || decoded == SIZE_MAX || decoded > cap) return false;
    out_len = decoded;
    return true;
}

// Writes the L2 signature for one request into `out` (base64url, with padding,
// NUL-terminated). `body` may be null/zero-length for requests without a body.
inline bool sign(const uint8_t* secret, size_t secret_len, const char* timestamp,
                 const char* method, const char* path, const char* body,
                 size_t body_len, char* out, size_t out_cap,
                 size_t& out_len) noexcept {
    out_len = 0;
    if (!secret || secret_len == 0 || !timestamp || !method || !path || !out)
        return false;
    if (out_cap < kSignatureB64Chars + 1) return false;
    if (body_len > 0 && !body) return false;

    // The signed message never exceeds 8 KiB in this codebase: the largest body
    // is a signed order (~1.2 KiB) and the heartbeat is a few dozen bytes. The
    // buffer is on the stack so signing never allocates.
    constexpr size_t kMessageMax = 8192;
    char message[kMessageMax];
    const size_t timestamp_len = std::strlen(timestamp);
    const size_t method_len = std::strlen(method);
    const size_t path_len = std::strlen(path);
    const size_t message_len = timestamp_len + method_len + path_len + body_len;
    if (message_len > kMessageMax) return false;
    size_t offset = 0;
    std::memcpy(message + offset, timestamp, timestamp_len);
    offset += timestamp_len;
    std::memcpy(message + offset, method, method_len);
    offset += method_len;
    std::memcpy(message + offset, path, path_len);
    offset += path_len;
    if (body_len > 0) std::memcpy(message + offset, body, body_len);

    uint8_t mac[32];
    HmacSha256 hmac;
    hmac.set_key(secret, secret_len);
    hmac.compute(reinterpret_cast<const uint8_t*>(message), message_len, mac);
    secure_zero(message, message_len);

    const size_t encoded = base64url_encode(mac, sizeof(mac), out);
    secure_zero(mac, sizeof(mac));
    if (encoded == 0 || encoded + 1 > out_cap) return false;
    out[encoded] = '\0';
    out_len = encoded;
    return true;
}

// The five header lines, already formatted. Sizes follow what curl_slist and
// the venue accept; a truncation is reported as failure, never silently sent.
struct Headers {
    char address[96]{};
    char api_key[128]{};
    char passphrase[192]{};
    char signature[96]{};
    char timestamp[48]{};
};

inline bool build_headers(const Credentials& credentials,
                          const uint8_t* secret, size_t secret_len,
                          const char* timestamp, const char* method,
                          const char* path, const char* body, size_t body_len,
                          Headers& headers) noexcept {
    if (!credentials.api_address_hex || !credentials.api_key ||
        !credentials.passphrase)
        return false;
    char signature_b64[kSignatureB64Chars + 1]{};
    size_t signature_len = 0;
    if (!sign(secret, secret_len, timestamp, method, path, body, body_len,
              signature_b64, sizeof(signature_b64), signature_len))
        return false;
    const int address_len = std::snprintf(
        headers.address, sizeof(headers.address), "POLY_ADDRESS: %s",
        credentials.api_address_hex);
    const int key_len = std::snprintf(headers.api_key, sizeof(headers.api_key),
                                      "POLY_API_KEY: %s", credentials.api_key);
    const int pass_len = std::snprintf(
        headers.passphrase, sizeof(headers.passphrase), "POLY_PASSPHRASE: %s",
        credentials.passphrase);
    const int signature_header_len = std::snprintf(
        headers.signature, sizeof(headers.signature), "POLY_SIGNATURE: %s",
        signature_b64);
    const int timestamp_header_len = std::snprintf(
        headers.timestamp, sizeof(headers.timestamp), "POLY_TIMESTAMP: %s",
        timestamp);
    secure_zero(signature_b64, sizeof(signature_b64));
    return address_len > 0 &&
           static_cast<size_t>(address_len) < sizeof(headers.address) &&
           key_len > 0 && static_cast<size_t>(key_len) < sizeof(headers.api_key) &&
           pass_len > 0 &&
           static_cast<size_t>(pass_len) < sizeof(headers.passphrase) &&
           signature_header_len > 0 &&
           static_cast<size_t>(signature_header_len) < sizeof(headers.signature) &&
           timestamp_header_len > 0 &&
           static_cast<size_t>(timestamp_header_len) < sizeof(headers.timestamp);
}

}  // namespace l2auth

#endif  // L2_AUTH_HPP
