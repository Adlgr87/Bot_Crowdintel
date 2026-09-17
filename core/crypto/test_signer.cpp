#include "eip712_signer.hpp"
#include <iostream>
#include <iomanip>

int main() {
    std::cout << "🔐 Testing EIP-712 Signing Engine..." << std::endl;
    
    // Use a known test private key
    std::vector<uint8_t> test_private_key(32, 0xAA); // 0xAA * 32 bytes
    
    try {
        EIP712Signer signer(test_private_key);
        
        OrderParams params;
        params.salt = 123456789;
        memset(params.maker, 0x11, 20);
        memset(params.taker, 0x00, 20);
        params.price = 500000000; // $5.00 * 1e6
        params.size = 1000000000; // $10.00 * 1e6
        params.nonce = 987654321;
        params.side = 0; // Buy
        
        std::array<uint8_t, 65> signature;
        signer.sign_order(params, signature);
        
        std::cout << "✅ Signature successful!" << std::endl;
        std::cout << "   r: ";
        for(int i = 0; i < 32; i++) std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)signature[i];
        std::cout << std::dec << std::endl;
        std::cout << "   s: ";
        for(int i = 32; i < 64; i++) std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)signature[i];
        std::cout << std::dec << std::endl;
        std::cout << "   v: 0x" << std::hex << (int)signature[64] << std::dec << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "❌ Signing failed: " << e.what() << std::endl;
        return 1;
    }
}
