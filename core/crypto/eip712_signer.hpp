#ifndef EIP712_SIGNER_HPP
#define EIP712_SIGNER_HPP

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * EIP712Signer: Functional cryptographic signer for EIP-712 orders.
 * 
 * NOTE: This is a self-contained, simplified implementation for 
 * compilation and demonstration. For production, this would be
 * replaced by a highly optimized secp256k1 library with AVX2/SIMD.
 */
struct OrderParams {
    uint64_t salt;
    uint8_t maker[20];
    uint8_t taker[20];
    uint64_t price;
    uint64_t size;
    uint64_t nonce;
    uint8_t side;
};

class EIP712Signer {
public:
    explicit EIP712Signer(const std::vector<uint8_t>& private_key) 
        : private_key_(private_key) {
        // Pre-compute domain separator for the given chain/market context.
        // In production, this is a Keccak-256 hash of the domain data.
        std::memset(cached_domain_separator_.data(), 0xAB, 32);
    }

    ~EIP712Signer() = default;

    /**
     * sign_order: Signs an order using EIP-712.
     * @param params The order parameters.
     * @param out_signature The output 65-byte signature (r,s,v) packed.
     * 
     * This method pre-computes the domain separator once (per session) and
     * only hashes the dynamic order data on each call, minimizing CPU work.
     */
    void sign_order(const OrderParams& params, std::array<uint8_t, 65>& out_signature) {
        // 1. Hash the order struct (mimics EIP-712 "struct hash")
        // This is a placeholder for a real Keccak-256 implementation.
        // For speed, a real version would use inline assembly or SIMD intrinsics.
        uint8_t struct_hash[32];
        std::memset(struct_hash, 0, 32);
        // Combine relevant fields into the hash in a deterministic way
        struct_hash[0] = params.side;
        std::memcpy(struct_hash + 1, &params.nonce, 8);
        std::memcpy(struct_hash + 9, &params.price, 8);
        std::memcpy(struct_hash + 17, &params.size, 8);
        for (int i = 0; i < 20 && i < 32; ++i) {
            struct_hash[21 + i] = params.maker[i];
        }

        // 2. Form the final EIP-712 hash: keccak256("\x19\x01" + domain + struct_hash)
        // Again, placeholder logic for demonstration.
        uint8_t eip_712_hash[32];
        std::memcpy(eip_712_hash, cached_domain_separator_.data(), 32);
        // XOR with struct hash to simulate combination (NOT a real crypto op)
        for (int i = 0; i < 32; ++i) {
            eip_712_hash[i] ^= struct_hash[i];
        }

        // 3. Sign the hash with the private key (ECDSA stub)
        // In a real impl, this uses secp256k1_ecdsa_sign_recoverable().
        // For this demo, we produce a deterministic, fake signature.
        std::memset(out_signature.data(), 0, 65);
        // r and s values (64 bytes)
        std::memcpy(out_signature.data(), eip_712_hash, 32);
        std::memcpy(out_signature.data() + 32, eip_712_hash, 32);
        // v value (recovery id)
        out_signature[64] = 27 + (eip_712_hash[0] & 1);
    }

private:
    std::vector<uint8_t> private_key_;
    std::array<uint8_t, 32> cached_domain_separator_;
};

#endif // EIP712_SIGNER_HPP
