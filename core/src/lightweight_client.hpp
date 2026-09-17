#ifndef LIGHTWEIGHT_CLIENT_HPP
#define LIGHTWEIGHT_CLIENT_HPP

#include <string>
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <vector>
#include <openssl/hmac.h>
#include <iostream>
#include <openssl/evp.h>
#include <curl/curl.h>

/**
 * LightweightCLOBClient: HTTPS client for Polymarket CLOB V2.
 * Uses libcurl for TLS and implements Polymarket's required HMAC authentication.
 */
struct SignedOrder {
    uint64_t nonce;
    std::array<uint8_t, 65> signature;
    std::string payload; // The URL-encoded JSON body of the signed order
};

class LightweightCLOBClient {
public:
    LightweightCLOBClient(
        const std::string& api_key,
        const std::string& secret,
        const std::string& passphrase,
        const std::string& base_url = "https://api.polymarket.com")
        : api_key_(api_key), secret_(secret), passphrase_(passphrase), base_url_(base_url) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~LightweightCLOBClient() {
        curl_global_cleanup();
    }

    // Submits an order to Polymarket CLOB V2 with HMAC authentication.
    // Returns true on HTTP 200, false otherwise.
    bool submit_order(const SignedOrder& order) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "❌ Failed to initialize CURL." << std::endl;
            return false;
        }

        // 1. Get the current timestamp (in milliseconds) for the X-Timestamp header
        long long timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // 2. Calculate HMAC signature: HMAC_SHA256(secret, timestamp + method + request_path + body)
        std::string method = "POST";
        std::string request_path = "/v2/order";
        std::string body = order.payload;
        std::string prehash = std::to_string(timestamp_ms) + method + request_path + body;
        std::string signature = hmac_sha256_base64(secret_, prehash);

        // 3. Set up the libcurl request with the required headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("X-API-Key: " + api_key_).c_str());
        headers = curl_slist_append(headers, ("X-Signature: " + signature).c_str());
        headers = curl_slist_append(headers, ("X-Passphrase: " + passphrase_).c_str());
        headers = curl_slist_append(headers, ("X-Timestamp: " + std::to_string(timestamp_ms)).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");

        std::string full_url = base_url_ + request_path;
        curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());

        // Disable Nagle for the underlying TCP connection (if possible)
        // Note: libcurl handles this well by default for HTTPS.
        
        // Capture the response code
        long response_code = 0;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // We don't need the body for a quick check

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        }

        std::cout << "🌐 HTTPS Order Submitted | Code: " << response_code 
                  << " | Nonce: " << order.nonce << std::endl;

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        // Consider any 2xx status code a success
        return (response_code >= 200 && response_code < 300);
    }

private:
    std::string api_key_;
    std::string secret_;        // HMAC Secret
    std::string passphrase_;
    std::string base_url_;

    // HMAC-SHA256 and returns the signature as a Base64 string (as Polymarket expects).
    static std::string hmac_sha256_base64(const std::string& secret, const std::string& data) {
        unsigned int len = 0;
        unsigned char hmac[EVP_MAX_MD_SIZE];
        
        HMAC(EVP_sha256(), secret.c_str(), secret.length(),
             reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
             hmac, &len);

        // Base64 encode the binary HMAC
        return base64_encode(hmac, len);
    }

    // Simple Base64 encoder
    static std::string base64_encode(const unsigned char* bytes_to_encode, size_t len) {
        static constexpr char base64_chars[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string ret;
        int i = 0;
        int j = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];

        while (len--) {
            char_array_3[i++] = *(bytes_to_encode++);
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;
                for(i = 0; i < 4; i++) ret += base64_chars[char_array_4[i]];
                i = 0;
            }
        }

        if (i) {
            for (j = i; j < 3; j++) char_array_3[j] = 0;
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for (j = 0; j < i + 1; j++) ret += base64_chars[char_array_4[j]];
            while(i++ < 3) ret += '=';
        }
        return ret;
    }

    // libcurl callback to discard body
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        return size * nmemb; // Just ignore the response body
    }
};

#endif // LIGHTWEIGHT_CLIENT_HPP
