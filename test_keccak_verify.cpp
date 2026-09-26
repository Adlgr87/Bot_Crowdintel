#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include "core/crypto/eip712_signer.hpp"

void print_hash(const uint8_t* hash) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 32; i++) {
        ss << std::setw(2) << (int)hash[i];
    }
    std::cout << ss.str() << std::endl;
}

int main() {
    uint8_t hash[32];
    
    // Test 1: keccak256("") should = c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
    const uint8_t empty[] = "";
    keccak256_hash(empty, 0, hash);
    std::cout << "keccak256(\"\") = ";
    print_hash(hash);
    std::cout << "Expected:         c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470" << std::endl;
    
    if (memcmp(hash, "\xc5\xd2\x46\x01\x86\xf7\x23\x3c\x92\x7e\x7d\xb2\xdc\xc7\x03\xc0\xe5\x00\xb6\x53\xca\x82\x27\x3b\x7b\xfa\xd8\x04\x5d\x85\xa4\x70", 32) == 0) {
        std::cout << "✅ Keccak-256 VERIFIED CORRECT" << std::endl;
    } else {
        std::cout << "❌ Keccak-256 INCORRECT" << std::endl;
    }
    
    return 0;
}
