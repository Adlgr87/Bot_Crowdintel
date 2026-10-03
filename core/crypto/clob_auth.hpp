#ifndef CLOB_AUTH_HPP
#define CLOB_AUTH_HPP

// ─────────────────────────────────────────────────────────────────────────────
// CLOB **L1** authentication: the EIP-712 "ClobAuth" message that proves
// control of the signing key and is used to create or derive CLOB API
// credentials (POST /auth/api-key, GET /auth/derive-api-key).
//
// Verified 2026-10-03 against two independent official sources:
//   * https://docs.polymarket.com/getting-started/api ("Authentication", step 1)
//       domain  = {name:"ClobAuthDomain", version:"1", chainId:137}
//       types   = ClobAuth(address address,string timestamp,uint256 nonce,
//                          string message)
//       message = "This message attests that I control the given wallet"
//       headers = POLY_ADDRESS, POLY_SIGNATURE, POLY_TIMESTAMP, POLY_NONCE
//   * Polymarket/py-sdk src/polymarket/_internal/l1_auth.py:19-52 (identical
//       domain, type list, primary type and message text)
// Note the domain has **no** verifyingContract member, unlike the order domain.
//
// Preflight uses this to prove that the configured private key is the key that
// owns the configured API credentials, by deriving them and comparing the API
// key.  The derived secret is never logged and is wiped after comparison.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "eip712_signer.hpp"
#include "keccak256.hpp"
#include "secure_zero.hpp"

namespace clob_auth {

inline constexpr const char* K_DOMAIN_TYPE_STR =
    "EIP712Domain(string name,string version,uint256 chainId)";
inline constexpr const char* K_STRUCT_TYPE_STR =
    "ClobAuth(address address,string timestamp,uint256 nonce,string message)";
inline constexpr const char* K_DOMAIN_NAME = "ClobAuthDomain";
inline constexpr const char* K_DOMAIN_VERSION = "1";
inline constexpr const char* K_MESSAGE =
    "This message attests that I control the given wallet";
inline constexpr uint64_t K_DEFAULT_NONCE = 0;

// domainSeparator = keccak(keccak(domainType) ‖ keccak(name) ‖ keccak(version) ‖
//                          chainId)
inline void domain_separator(uint64_t chain_id, uint8_t out[32]) noexcept {
    uint8_t buffer[32 * 4];
    keccak256_hash(reinterpret_cast<const uint8_t*>(K_DOMAIN_TYPE_STR),
                   std::strlen(K_DOMAIN_TYPE_STR), buffer);
    keccak256_hash(reinterpret_cast<const uint8_t*>(K_DOMAIN_NAME),
                   std::strlen(K_DOMAIN_NAME), buffer + 32);
    keccak256_hash(reinterpret_cast<const uint8_t*>(K_DOMAIN_VERSION),
                   std::strlen(K_DOMAIN_VERSION), buffer + 64);
    std::memset(buffer + 96, 0, 32);
    for (int i = 0; i < 8; ++i)
        buffer[96 + 24 + i] =
            static_cast<uint8_t>((chain_id >> (8 * (7 - i))) & 0xFFULL);
    keccak256_hash(buffer, sizeof(buffer), out);
}

inline bool address_from_hex(const char* text, uint8_t out[20]) noexcept {
    if (!text) return false;
    const size_t len = std::strlen(text);
    if (len != 42 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return false;
    auto nibble = [](char c, int& out_value) {
        if (c >= '0' && c <= '9') { out_value = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { out_value = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { out_value = c - 'A' + 10; return true; }
        return false;
    };
    for (int i = 0; i < 20; ++i) {
        int high = 0;
        int low = 0;
        if (!nibble(text[2 + i * 2], high) || !nibble(text[3 + i * 2], low))
            return false;
        out[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

// Builds the EIP-712 digest for the ClobAuth message.
inline bool build_digest(const char* address_hex, uint64_t timestamp_s, uint64_t nonce,
                         uint64_t chain_id, uint8_t out_digest[32]) noexcept {
    uint8_t address[20];
    if (!address_from_hex(address_hex, address)) return false;
    char timestamp_text[24];
    const int written = std::snprintf(timestamp_text, sizeof(timestamp_text), "%llu",
                                      static_cast<unsigned long long>(timestamp_s));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(timestamp_text))
        return false;

    uint8_t struct_buffer[32 * 5];
    keccak256_hash(reinterpret_cast<const uint8_t*>(K_STRUCT_TYPE_STR),
                   std::strlen(K_STRUCT_TYPE_STR), struct_buffer);
    std::memset(struct_buffer + 32, 0, 12);
    std::memcpy(struct_buffer + 44, address, 20);
    keccak256_hash(reinterpret_cast<const uint8_t*>(timestamp_text),
                   std::strlen(timestamp_text), struct_buffer + 64);
    std::memset(struct_buffer + 96, 0, 32);
    for (int i = 0; i < 8; ++i)
        struct_buffer[96 + 24 + i] =
            static_cast<uint8_t>((nonce >> (8 * (7 - i))) & 0xFFULL);
    keccak256_hash(reinterpret_cast<const uint8_t*>(K_MESSAGE),
                   std::strlen(K_MESSAGE), struct_buffer + 128);

    uint8_t struct_hash[32];
    keccak256_hash(struct_buffer, sizeof(struct_buffer), struct_hash);
    uint8_t separator[32];
    domain_separator(chain_id, separator);

    uint8_t final_buffer[2 + 32 + 32];
    final_buffer[0] = 0x19;
    final_buffer[1] = 0x01;
    std::memcpy(final_buffer + 2, separator, 32);
    std::memcpy(final_buffer + 34, struct_hash, 32);
    keccak256_hash(final_buffer, sizeof(final_buffer), out_digest);
    secure_zero(struct_buffer, sizeof(struct_buffer));
    secure_zero(struct_hash, sizeof(struct_hash));
    return true;
}

// Signs the ClobAuth message and returns "0x" + 130 lowercase hex characters.
inline bool sign(const EIP712Signer& signer, const char* address_hex,
                 uint64_t timestamp_s, uint64_t nonce, uint64_t chain_id,
                 char out_signature_hex[133]) noexcept {
    uint8_t digest[32];
    if (!build_digest(address_hex, timestamp_s, nonce, chain_id, digest)) return false;
    uint8_t signature[65];
    const bool signed_ok = signer.sign_digest(digest, signature);
    secure_zero(digest, sizeof(digest));
    if (!signed_ok) {
        secure_zero(signature, sizeof(signature));
        return false;
    }
    static const char* digits = "0123456789abcdef";
    out_signature_hex[0] = '0';
    out_signature_hex[1] = 'x';
    for (int i = 0; i < 65; ++i) {
        out_signature_hex[2 + i * 2] = digits[signature[i] >> 4];
        out_signature_hex[3 + i * 2] = digits[signature[i] & 0x0FU];
    }
    out_signature_hex[132] = '\0';
    secure_zero(signature, sizeof(signature));
    return true;
}

// Parses {"apiKey":…,"secret":…,"passphrase":…} without copying the secret into
// anything larger than a bounded, wipeable buffer.
struct DerivedCredentials {
    char api_key[96]{};
    char secret[128]{};
    char passphrase[160]{};
    void wipe() noexcept { secure_zero(this, sizeof(*this)); }
};

}  // namespace clob_auth

#endif  // CLOB_AUTH_HPP
