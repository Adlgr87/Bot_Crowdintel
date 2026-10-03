#ifndef ORDER_DIGEST_HPP
#define ORDER_DIGEST_HPP

// The EIP-712 digest of a V2 order, computed without signing.
//
// Two uses, both safety-critical:
//   1. a *local* order identity that exists before the order is sent, so a
//      crash between "handed to the transport" and "venue answered" still
//      leaves a durable record of exactly which order was in flight;
//   2. an offline cross-check that the digest the signer signed is the digest
//      the wire body describes.
//
// [NO VERIFICADO] Whether the venue's `orderID` equals this digest is *not*
// documented for CLOB V2 (the legacy TypeScript SDK exposes getOrderHash() via
// viem's hashTypedData, and V1 order ids were the EIP-712 hash, but no current
// official source states it for V2).  The recorder therefore never assumes it:
// local tickets are keyed "L:<digest>" and are linked to the venue id returned
// by POST /order.  When the two happen to be equal the recorder logs it once,
// which is how the canary run settles the question.  Verify manually by
// comparing a POST /order `orderID` with this digest for the same payload.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "eip712_signer.hpp"
#include "keccak256.hpp"

namespace crowdintel {

// digest = keccak256(0x1901 ‖ domainSeparator(neg_risk) ‖ structHash(order))
inline bool compute_order_digest(const OrderV2& order, bool neg_risk,
                                 uint8_t out_digest[32]) noexcept {
    uint8_t separator[32];
    compute_domain_separator(neg_risk, separator);
    uint8_t encoded[32 * 11];
    abi_encode_order(order, encoded);
    uint8_t type_hash[32];
    keccak256_hash(reinterpret_cast<const uint8_t*>(K_ORDER_TYPE_STR),
                   std::strlen(K_ORDER_TYPE_STR), type_hash);
    uint8_t struct_input[32 + sizeof(encoded)];
    std::memcpy(struct_input, type_hash, 32);
    std::memcpy(struct_input + 32, encoded, sizeof(encoded));
    uint8_t struct_hash[32];
    keccak256_hash(struct_input, sizeof(struct_input), struct_hash);
    uint8_t final_input[2 + 32 + 32];
    final_input[0] = 0x19;
    final_input[1] = 0x01;
    std::memcpy(final_input + 2, separator, 32);
    std::memcpy(final_input + 34, struct_hash, 32);
    keccak256_hash(final_input, sizeof(final_input), out_digest);
    return true;
}

inline void digest_to_hex(const uint8_t digest[32], char out[67]) noexcept {
    static const char* digits = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 32; ++i) {
        out[2 + i * 2] = digits[digest[i] >> 4];
        out[3 + i * 2] = digits[digest[i] & 0x0FU];
    }
    out[66] = '\0';
}

}  // namespace crowdintel

#endif  // ORDER_DIGEST_HPP
