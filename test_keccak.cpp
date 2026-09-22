#include <iostream>
#include <iomanip>
#include <sstream>
#include "core/crypto/eip712_signer.hpp"

int main() {
    uint8_t hash[32];
    const uint8_t empty[] = "";
    keccak256_hash(empty, 0, hash);
    
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 32; i++) {
        ss << std::setw(2) << (int)hash[i];
    }
    std::string result = ss.str();
    
    std::cout << "keccak256(\"\") = " << result << std::endl;
    std::cout << "Expected:       c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470" << std::endl;
    
    if (result == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470") {
        std::cout << "✅ Keccak-256 VERIFIED CORRECT" << std::endl;
    } else {
        std::cout << "❌ Keccak-256 INCORRECT" << std::endl;
    }
    return 0;
}
