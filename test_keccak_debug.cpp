#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include "core/crypto/eip712_signer.hpp"

// Implementación independiente de referencia de Keccak-256
// Basado en el algoritmo de Keccak Team (http://keccak.noekeon.org/)
inline void keccak256_ref(const uint8_t* data, size_t len, uint8_t out[32]) {
    // Estado 5x5
    uint64_t state[5][5] = {0};
    
    // Padding pad10*1
    uint8_t padded[136] = {0};
    memcpy(padded, data, len);
    padded[len] ^= 0x01;
    padded[135] ^= 0x80;
    
    // Absorver - lane-major: lane[x,y] = state[x][y]
    for (int x = 0; x < 5; x++) {
        for (int y = 0; y < 5; y++) {
            int byte_idx = (x + 5*y) * 8;  // lane major
            if (byte_idx < 136) {
                uint64_t lane = 0;
                for (int k = 0; k < 8; k++)
                    lane |= (uint64_t)padded[byte_idx + k] << (8 * k);
                // state[x][y] ^= lane;
                // Actually absorb in the canonical order: byte stream mapped to lanes
            }
        }
    }
    
    // Absorber correcto: simplemente XOR los bytes en orden lane-major
    // En Keccak, los datos se mapean a lanes en orden: lane[0] = bytes[0:8], lane[1] = bytes[8:16], etc.
    // lane[x, y] = state[x + 5*y (linear index)]
    // lane 0 -> state[0][0], lane 1 -> state[1][0], ..., lane 5 -> state[0][1], ...
    for (int lane = 0; lane < 17; lane++) {
        uint64_t val = 0;
        for (int k = 0; k < 8; k++)
            val |= (uint64_t)padded[lane * 8 + k] << (8 * k);
        // lane 0 -> x=0,y=0; lane 1 -> x=1,y=0; ... lane 5 -> x=0,y=1
        int x = lane % 5;
        int y = lane / 5;
        state[x][y] ^= val;
    }
    
    // Round constants
    static const uint64_t RC[24] = {
        0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
        0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
        0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
        0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
        0x000000008000808BULL, 0x000000008000808AULL, 0x0000000080008003ULL,
        0x0000000080000002ULL, 0x0000000080000000ULL, 0x0000000000008009ULL,
        0x0000000000000003ULL, 0x000000000000000AULL, 0x800000000000008AULL,
        0x8000000000000088ULL, 0x8000000080008009ULL, 0x8000000080000000ULL
    };
    
    // Rotación offsets r[x][y]
    static const int r[5][5] = {
        { 0, 36,  3, 41, 18 },
        { 1, 32,  4, 43, 19 },
        { 62, 6, 44, 23, 13 },
        { 28, 55, 25, 21, 56 },
        { 27, 20, 39, 56,  0 }
    };
    
    for (int round = 0; round < 24; round++) {
        // Theta
        uint64_t C[5], D[5];
        for (int x = 0; x < 5; x++)
            C[x] = state[x][0] ^ state[x][1] ^ state[x][2] ^ state[x][3] ^ state[x][4];
        for (int x = 0; x < 5; x++)
            D[x] = C[(x+4)%5] ^ ((C[(x+1)%5] << 1) | (C[(x+1)%5] >> 63));
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                state[x][y] ^= D[x];
        
        // Rho and Pi
        uint64_t B[5][5];
        for (int x = 0; x < 5; x++) {
            for (int y = 0; y < 5; y++) {
                uint64_t val = state[x][y];
                int shift = r[x][y];
                int nx = (2 * x + 3 * y) % 5;
                if (shift == 0) B[y][nx] = val;
                else B[y][nx] = (val << shift) | (val >> (64 - shift));
            }
        }
        
        // Chi
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                state[x][y] = B[x][y] ^ ((~B[(x+1)%5][y]) & B[(x+2)%5][y]);
        
        // Iota
        state[0][0] ^= RC[round];
    }
    
    // Squeeze
    for (int lane = 0; lane < 4; lane++) {
        int x = lane % 5;
        int y = lane / 5;
        uint64_t val = state[x][y];
        for (int k = 0; k < 8; k++)
            out[lane * 8 + k] = (val >> (8 * k)) & 0xFF;
    }
}

void print_hash(const uint8_t* hash) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i = 0; i < 32; i++) {
        ss << std::setw(2) << (int)hash[i];
    }
    std::cout << ss.str();
}

int main() {
    uint8_t hash[32];
    
    const uint8_t empty[] = "";
    
    keccak256_hash(empty, 0, hash);
    std::cout << "keccak256(\"\") actual = ";
    print_hash(hash);
    std::cout << std::endl;
    
    keccak256_ref(empty, 0, hash);
    std::cout << "keccak256(\"\") ref    = ";
    print_hash(hash);
    std::cout << std::endl;
    
    std::cout << "Expected              = c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470" << std::endl;
    
    return 0;
}
