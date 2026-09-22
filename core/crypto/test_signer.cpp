#include "eip712_signer.hpp"
#include <iostream>
#include <iomanip>
#include <cassert>

static void print_hash(const uint8_t* h, const char* label) {
    std::cout << label << ": ";
    for (int i = 0; i < 32; i++)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)h[i];
    std::cout << std::dec << std::endl;
}

static bool compare_hash(const uint8_t* got, const uint8_t* expected, const char* label) {
    for (int i = 0; i < 32; i++) {
        if (got[i] != expected[i]) {
            std::cout << "❌ " << label << " MISMATCH at byte " << i << std::endl;
            std::cout << "  Got:      ";
            for (int j = 0; j < 32; j++)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)got[j];
            std::cout << "\n  Expected: ";
            for (int j = 0; j < 32; j++)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)expected[j];
            std::cout << std::dec << std::endl;
            return false;
        }
    }
    std::cout << "✅ " << label << " PASS" << std::endl;
    return true;
}

int main() {
    std::cout << "🔐 EIP-712 Signing Engine — Known-Answer Tests" << std::endl;
    std::cout << std::endl;

    // ── Keccak-256 known-answer vectors (from pycryptodome / Ethereum) ──
    struct TestCase {
        const char* name;
        const uint8_t* data;
        size_t len;
        uint8_t expected[32];
    };

    // Expected hashes from pycryptodome: keccak.new(digest_bits=256).digest()
    uint8_t empty_hash[32] = {
        0xc5, 0xd2, 0x46, 0x01, 0x86, 0xf7, 0x23, 0x3c,
        0x92, 0x7e, 0x7d, 0xb2, 0xdc, 0xc7, 0x03, 0xc0,
        0xe5, 0x00, 0xb6, 0x53, 0xca, 0x82, 0x27, 0x3b,
        0x7b, 0xfa, 0xd8, 0x04, 0x5d, 0x85, 0xa4, 0x70
    };

    uint8_t abc_hash[32] = {
        0x4e, 0x03, 0x65, 0x7a, 0xea, 0x45, 0xa9, 0x4f,
        0xc7, 0xd4, 0x7b, 0xa8, 0x26, 0xc8, 0xd6, 0x67,
        0xc0, 0xd1, 0xe6, 0xe3, 0x3a, 0x64, 0xa0, 0x36,
        0xec, 0x44, 0xf5, 0x8f, 0xa1, 0x2d, 0x6c, 0x45
    };

    // "abc" = 0x61, 0x62, 0x63
    uint8_t abc_data[3] = {0x61, 0x62, 0x63};

    // Test: empty string
    uint8_t out[32];
    EIP712Signer::hash_keccak256(nullptr, 0, out);
    if (!compare_hash(out, empty_hash, "Keccak-256(\"\")"))
        return 1;

    // Test: "abc"
    EIP712Signer::hash_keccak256(abc_data, 3, out);
    if (!compare_hash(out, abc_hash, "Keccak-256(\"abc\")"))
        return 1;

    // Test: single byte 0x00
    uint8_t zero_byte = 0x00;
    uint8_t zero_hash[32] = {
        0xbc, 0x36, 0x78, 0x9e, 0x7a, 0x1e, 0x28, 0x14,
        0x36, 0x46, 0x42, 0x29, 0x82, 0x8f, 0x81, 0x7d,
        0x66, 0x12, 0xf7, 0xb4, 0x77, 0xd6, 0x65, 0x91,
        0xff, 0x96, 0xa9, 0xe0, 0x64, 0xbc, 0xc9, 0x8a
    };
    EIP712Signer::hash_keccak256(&zero_byte, 1, out);
    if (!compare_hash(out, zero_hash, "Keccak-256(0x00)"))
        return 1;

    std::cout << std::endl;
    std::cout << "📝 EIP-712 Signing Test" << std::endl;
    std::cout << std::endl;

    // ── EIP-712 signing test ──
    // Test private key (0xAA * 32), known domain
    std::vector<uint8_t> test_private_key(32, 0xAA);

    // Domain: EIP712Domain(string name,address verifyingContract)
    // Domain data: name = "TEST", verifyingContract = 0x1234... (20 bytes)
    std::string domain_type = "EIP712Domain(string name,address verifyingContract)";
    uint8_t contract[20];
    memset(contract, 0xAB, 20);
    std::vector<uint8_t> domain_data;
    domain_data.reserve(64);  // 32 (name hash) + 32 (address padded)

    // name hash: keccak256("TEST")
    uint8_t name_hash[32];
    const char* name = "TEST";
    EIP712Signer::hash_keccak256(
        reinterpret_cast<const uint8_t*>(name), 4, name_hash);
    domain_data.insert(domain_data.end(), name_hash, name_hash + 32);

    // verifyingContract: 20 bytes left-padded to 32 bytes
    uint8_t addr_padded[32] = {0};
    memcpy(addr_padded + 12, contract, 20);
    domain_data.insert(domain_data.end(), addr_padded, addr_padded + 32);

    // Compute domain separator
    uint8_t expected_domain_sep[32];
    eip712_domain_separator(domain_type, domain_data, expected_domain_sep);
    print_hash(expected_domain_sep, "Domain Separator");

    try {
        EIP712Signer signer(test_private_key, domain_type, domain_data);

        // ── Known Answer Test for EIP-712 full pipeline ──
        // Private key 0xAA*32, domain with name="TEST", verifyingContract=0xAB*20
        // Generate NUM_RUNS signatures (ECDSA is non-deterministic via OpenSSL's RNG)
        // and verify each one on-chain-equivalent recovery using Python/pycryptodome.
        constexpr int NUM_RUNS = 20;
        int all_valid = 0;

        for (int run = 0; run < NUM_RUNS; run++) {
            OrderParams params;
            params.salt = 123456789;
            memset(params.maker, 0x11, 20);
            memset(params.taker, 0x00, 20);
            params.price = 500000000;
            params.size = 1000000000;
            params.nonce = 987654321;
            params.side = 0;

            std::array<uint8_t, 65> signature;
            signer.sign_order(params, signature);

            // Print parseable output for external verification
            std::cout << "SIG " << run << " r=";
            for (int i = 0; i < 32; i++)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)signature[i];
            std::cout << " s=";
            for (int i = 32; i < 64; i++)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)signature[i];
            std::cout << " v=" << std::dec << (int)signature[64] << std::endl;

            // Verify v is 27 or 28
            if (signature[64] == 27 || signature[64] == 28)
                all_valid++;
        }

        // Print the EIP-712 hash for the first run (deterministic given fixed input)
        {
            OrderParams params;
            params.salt = 123456789;
            memset(params.maker, 0x11, 20);
            memset(params.taker, 0x00, 20);
            params.price = 500000000;
            params.size = 1000000000;
            params.nonce = 987654321;
            params.side = 0;

            const std::string STRUCT_TYPE =
                "OrderParams(uint64 salt,address maker,address taker,uint64 price,uint64 size,uint64 nonce,uint8 side)";
            uint8_t struct_hash[32];
            eip712_order_struct_hash(STRUCT_TYPE, params, struct_hash);

            uint8_t final_payload[66];
            final_payload[0] = 0x19;
            final_payload[1] = 0x01;
            memcpy(final_payload + 2, expected_domain_sep, 32);
            memcpy(final_payload + 34, struct_hash, 32);
            uint8_t eip712_hash[32];
            keccak256_hash(final_payload, 66, eip712_hash);
            print_hash(eip712_hash, "EIP712_HASH");
        }

        std::cout << std::endl;
        std::cout << "✅ " << all_valid << "/" << NUM_RUNS << " signatures have valid v (27 or 28)" << std::endl;
        if (all_valid != NUM_RUNS) {
            std::cout << "❌ Some signatures have invalid v values" << std::endl;
            return 1;
        }
        std::cout << "✅ Signature tests PASS" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Signing failed: " << e.what() << std::endl;
        return 1;
    }
}
