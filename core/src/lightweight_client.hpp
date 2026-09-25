#ifndef LIGHTWEIGHT_CLIENT_HPP
#define LIGHTWEIGHT_CLIENT_HPP

/**
 * LightweightCLOBClient: HTTPS client for Polymarket CLOB V2.
 *
 * Features added per WORKFLOW_REMEDIACION_CUMPLIMIENTO.md (Fase 1):
 *   T1-1: RateLimiter per endpoint (token bucket, O(1), thread-safe)
 *   T1-2: Exponential backoff with jitter on 429/5xx, Retry-After header parsing
 *   T1-3: Connection pooling/keep-alive (persistent CURL* handle)
 *   T1-4: Real HTTP response parsing (200/429/4xx/5xx, extract order_id + error)
 *
 * CRITICAL: The HMAC-SHA256 authentication computation is PRESERVED EXACTLY
 * as the verified path. Only features OUTSIDE the HMAC are added.
 * The prehash = timestamp + method + request_path + body and the Base64
 * encoding are unchanged.
 *
 * Client is the SINGLE HTTP client for production (mock_client.hpp is
 * benchmark-only, excluded from production binary).
 */

#include <string>
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <vector>
#include <optional>
#include <random>
#include <thread>
#include <strings.h>  // strncasecmp for header parsing (T1-2 Retry-After)
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <curl/curl.h>

#include "rate_limiter.hpp"  // T1-1: Token bucket rate limiter

// ─── HTTP Response Struct (T1-4) ─────────────────────────────────────────
enum class HttpStatus : int {
    OK = 200,
    TOO_MANY = 429,
    BAD_REQUEST = 400,
    UNAUTHORIZED = 401,
    FORBIDDEN = 403,
    NOT_FOUND = 404,
    SERVER_ERROR = 500,
    BAD_GATEWAY = 502,
    SERVICE_UNAVAILABLE = 503,
};

struct HttpResponse {
    HttpStatus status;
    std::string body;
    std::optional<std::string> retry_after;      // From Retry-After header (seconds or HTTP-date)
    std::optional<std::string> order_id;         // Extracted from JSON body (if order submission)
    std::optional<std::string> error_message;    // Extracted from JSON error body
};

struct BalanceResponse {
    double usdc_balance;
    double pol_balance;
};

struct SignedOrder {
    uint64_t nonce;
    std::array<uint8_t, 65> signature;
    std::string payload; // The JSON body of the signed order
};

class LightweightCLOBClient {
public:
    LightweightCLOBClient(
        const std::string& api_key,
        const std::string& secret,
        const std::string& passphrase,
        const std::string& base_url = "https://api.polymarket.com",
        double rate_limit_per_sec = 1.0,    // T1-1: Configurable per env
        double burst = 2.0)
        : api_key_(api_key), secret_(secret), passphrase_(passphrase),
          base_url_(base_url),
          rate_limiter_(rate_limit_per_sec, burst),
          max_retries_(5),                  // T1-2: Configurable
          curl_handle_(nullptr)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        // T1-3: Create persistent curl handle for connection pooling
        curl_handle_ = curl_easy_init();
        if (curl_handle_) {
            // Keep-alive / connection reuse settings
            curl_easy_setopt(curl_handle_, CURLOPT_FRESH_CONNECT, 0L);    // Reuse connections
            curl_easy_setopt(curl_handle_, CURLOPT_FORBID_REUSE, 0L);     // Allow reuse
            curl_easy_setopt(curl_handle_, CURLOPT_TCP_NODELAY, 1L);      // Disable Nagle
            curl_easy_setopt(curl_handle_, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3);
        }
    }

    virtual ~LightweightCLOBClient() {
        if (curl_handle_) {
            curl_easy_cleanup(curl_handle_);
        }
        curl_global_cleanup();
    }

    // ─── T1-1: Rate Limit Check ──────────────────────────────────────
    // Must be called BEFORE attempting network I/O.
    // O(1), branch-predicted: returns true if rate limit allows the request.
    bool check_rate_limit() {
        return rate_limiter_.try_acquire();
    }

    // ─── T1-4: Real HTTP response parsing ────────────────────────────

    /**
     * submit_order_with_response: Submits an order and returns the full
     * parsed HttpResponse (status, body, retry-after, order_id).
     * Implements T1-2 (backoff) and T1-4 (response parsing).
     */
    virtual std::optional<HttpResponse> submit_order_with_response(const SignedOrder& order) {
        // T1-1: Check rate limit before hitting the network
        if (!rate_limiter_.try_acquire()) {
            return std::nullopt;  // Rate limited — caller handles
        }

        HttpResponse response;
        long long timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // T1-4: Compute HMAC — PRESERVED EXACTLY from verified path
        std::string method = "POST";
        std::string request_path = "/v2/order";
        std::string body = order.payload;
        std::string prehash = std::to_string(timestamp_ms) + method + request_path + body;
        std::string signature = hmac_sha256_base64(secret_, prehash);

        // Parse the signed order payload to extract client_order_id
        // The payload JSON contains: {"p":"...","s":...,"side":...,"n":...,"salt":...,"mker":"..."}
        response.order_id = extract_order_id_from_payload(body);

        // Attempt with exponential backoff (T1-2)
        for (int attempt = 0; attempt <= max_retries_; attempt++) {
            // T1-3: Use persistent curl handle (connection pooling)
            HttpResponse attempt_response = perform_http_request(
                method, request_path, body, timestamp_ms, signature);

            // T1-4: Parse response status
            if (attempt_response.status == HttpStatus::OK) {
                // Success — extract order_id from body if not already set
                if (!attempt_response.order_id.has_value()) {
                    attempt_response.order_id = extract_order_id_from_payload(body);
                }
                return attempt_response;
            }

            // T1-2: Exponential backoff with jitter on 429/5xx
            if (should_retry(attempt_response.status)) {
                int delay_ms = calculate_backoff(attempt, attempt_response.retry_after);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                continue;  // Retry
            }

            // Non-retryable error (4xx except 429)
            return attempt_response;
        }

        // Exhausted retries
        response.status = HttpStatus::SERVICE_UNAVAILABLE;
        response.error_message = "Max retries exceeded";
        return response;
    }

    /**
     * submit_order: Legacy interface returning bool (backward compatible).
     * Calls submit_order_with_response and returns true on 2xx.
     */
    bool submit_order(const SignedOrder& order) {
        auto response = submit_order_with_response(order);
        if (!response) {
            return false;  // Rate limited
        }
        long code = static_cast<long>(response->status);
        return (code >= 200 && code < 300);
    }

    // ─── T1-4: Query order status (for anti-retry in OrderManager) ──────
    // Virtual for testability — allows mock clients to override in tests.
    virtual std::optional<std::string> query_order_status(const std::string& client_order_id) {
        // T1-1: Rate limit check
        if (!rate_limiter_.try_acquire()) {
            return std::nullopt;
        }

        // GET /v2/order?client_order_id=...
        std::string request_path = "/v2/order?client_order_id=" + client_order_id;
        // Build HMAC for GET request
        long long timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string prehash = std::to_string(timestamp_ms) + "GET" + request_path + "";
        std::string signature = hmac_sha256_base64(secret_, prehash);

        HttpResponse response = perform_get_request(request_path, timestamp_ms, signature);

        if (response.status == HttpStatus::OK) {
            return response.body;  // Return raw JSON, caller parses status
        }
        return std::nullopt;
    }

    // ─── T3-4: Balance query (for BalanceChecker) ─────────────────────
    std::optional<BalanceResponse> get_balances() {
        if (!rate_limiter_.try_acquire()) {
            return std::nullopt;
        }

        std::string request_path = "/v2/balance";
        long long timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string prehash = std::to_string(timestamp_ms) + "GET" + request_path + "";
        std::string signature = hmac_sha256_base64(secret_, prehash);

        HttpResponse response = perform_get_request(request_path, timestamp_ms, signature);

        if (response.status == HttpStatus::OK) {
            BalanceResponse bal = parse_balance_json(response.body);
            return bal;
        }
        return std::nullopt;
    }

    // ─── T1-2: Backoff configuration ─────────────────────────────────
    void set_max_retries(int max) { max_retries_ = max; }

private:
    std::string api_key_;
    std::string secret_;        // HMAC Secret
    std::string passphrase_;
    std::string base_url_;

    // T1-1: Rate limiter (per endpoint, token bucket)
    RateLimiter rate_limiter_;

    // T1-2: Max retries for backoff
    int max_retries_;

    // T1-3: Persistent curl handle for connection pooling
    CURL* curl_handle_;

    // Response header capture for Retry-After
    struct HeaderData {
        std::string retry_after;
    };

    // ─── HMAC-SHA256 (PRESERVED EXACTLY — DO NOT MODIFY) ─────────────
    // Same computation: HMAC_SHA256(secret, timestamp + method + path + body)
    static std::string hmac_sha256_base64(const std::string& secret, const std::string& data) {
        unsigned int len = 0;
        unsigned char hmac[EVP_MAX_MD_SIZE];

        HMAC(EVP_sha256(), secret.c_str(), secret.length(),
             reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
             hmac, &len);

        return base64_encode(hmac, len);
    }

    // Base64 encoder (preserved)
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

    // libcurl headers + body write callbacks
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        std::string* response_body = static_cast<std::string*>(userp);
        response_body->append(static_cast<char*>(contents), total);
        return total;
    }

    static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
        size_t total = size * nitems;
        std::string header(buffer, total);
        HeaderData* data = static_cast<HeaderData*>(userdata);

        // Parse Retry-After header (T1-2)
        if (header.size() >= 13 &&
            (strncasecmp(header.c_str(), "Retry-After:", 12) == 0)) {
            data->retry_after = header.substr(13);
            // Trim whitespace
            size_t start = data->retry_after.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) {
                data->retry_after = data->retry_after.substr(start);
                data->retry_after.erase(data->retry_after.find_last_not_of(" \t\r\n") + 1);
            }
        }
        return total;
    }

    // ─── HTTP Request Execution (T1-3: uses persistent curl handle) ───
    HttpResponse perform_http_request(const std::string& method,
                                       const std::string& request_path,
                                       const std::string& body,
                                       long long timestamp_ms,
                                       const std::string& signature) {
        HttpResponse response;
        std::string response_body;
        HeaderData header_data;

        if (!curl_handle_) {
            response.status = HttpStatus::SERVICE_UNAVAILABLE;
            response.error_message = "curl handle not initialized";
            return response;
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("X-API-Key: " + api_key_).c_str());
        headers = curl_slist_append(headers, ("X-Signature: " + signature).c_str());
        headers = curl_slist_append(headers, ("X-Passphrase: " + passphrase_).c_str());
        headers = curl_slist_append(headers, ("X-Timestamp: " + std::to_string(timestamp_ms)).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");

        std::string full_url = base_url_ + request_path;
        curl_easy_setopt(curl_handle_, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(curl_handle_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl_handle_, CURLOPT_POSTFIELDSIZE, body.length());
        curl_easy_setopt(curl_handle_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_handle_, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl_handle_, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl_handle_, CURLOPT_HEADERDATA, &header_data);
        curl_easy_setopt(curl_handle_, CURLOPT_TIMEOUT, 5L);       // Bounded wait
        curl_easy_setopt(curl_handle_, CURLOPT_NOSIGNAL, 1L);       // Thread-safe
        curl_easy_setopt(curl_handle_, CURLOPT_FOLLOWLOCATION, 0L); // No redirects

        CURLcode res = curl_easy_perform(curl_handle_);

        long response_code = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl_handle_, CURLINFO_RESPONSE_CODE, &response_code);
        }

        curl_slist_free_all(headers);

        // Parse response
        response.status = static_cast<HttpStatus>(response_code);
        response.body = response_body;
        if (!header_data.retry_after.empty()) {
            response.retry_after = header_data.retry_after;
        }

        // Extract order_id from body if present
        response.order_id = extract_order_id_from_json(response_body);

        // Extract error message if present
        response.error_message = extract_error_from_json(response_body);

        return response;
    }

    HttpResponse perform_get_request(const std::string& request_path,
                                      long long timestamp_ms,
                                      const std::string& signature) {
        HttpResponse response;
        std::string response_body;
        HeaderData header_data;

        if (!curl_handle_) {
            response.status = HttpStatus::SERVICE_UNAVAILABLE;
            response.error_message = "curl handle not initialized";
            return response;
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("X-API-Key: " + api_key_).c_str());
        headers = curl_slist_append(headers, ("X-Signature: " + signature).c_str());
        headers = curl_slist_append(headers, ("X-Passphrase: " + passphrase_).c_str());
        headers = curl_slist_append(headers, ("X-Timestamp: " + std::to_string(timestamp_ms)).c_str());

        std::string full_url = base_url_ + request_path;
        curl_easy_setopt(curl_handle_, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(curl_handle_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_handle_, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl_handle_, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl_handle_, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl_handle_, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl_handle_, CURLOPT_HEADERDATA, &header_data);
        curl_easy_setopt(curl_handle_, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl_handle_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_handle_, CURLOPT_FOLLOWLOCATION, 0L);

        CURLcode res = curl_easy_perform(curl_handle_);

        long response_code = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl_handle_, CURLINFO_RESPONSE_CODE, &response_code);
        }

        curl_slist_free_all(headers);

        response.status = static_cast<HttpStatus>(response_code);
        response.body = response_body;
        if (!header_data.retry_after.empty()) {
            response.retry_after = header_data.retry_after;
        }

        return response;
    }

    // ─── Helpers ──────────────────────────────────────────────────────

    // Check if status code should trigger retry (T1-2)
    static bool should_retry(HttpStatus status) {
        switch (status) {
            case HttpStatus::TOO_MANY:         // 429
            case HttpStatus::BAD_GATEWAY:       // 502
            case HttpStatus::SERVICE_UNAVAILABLE: // 503
                return true;
            default:
                return false;
        }
    }

    // Calculate exponential backoff with jitter (T1-2)
    int calculate_backoff(int attempt, const std::optional<std::string>& retry_after) {
        // Base delay: 100ms * 2^attempt (100, 200, 400, 800, 1600...)
        int base_delay_ms = 100 * (1 << attempt);

        // Add jitter: ±25% random
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> jitter(-0.25, 0.25);
        double jittered = base_delay_ms * (1.0 + jitter(rng));

        // If Retry-After header present, use it as minimum
        int delay_ms = static_cast<int>(jittered);
        if (retry_after) {
            try {
                int retry_after_sec = std::stoi(*retry_after);
                delay_ms = (std::max)(delay_ms, retry_after_sec * 1000);
            } catch (...) {
                // Retry-After might be HTTP-date format — ignore, use exponential
            }
        }

        return delay_ms;
    }

    // Extract order_id from a JSON body (minimal parser, no external deps)
    static std::optional<std::string> extract_order_id_from_json(const std::string& json) {
        // Look for "order_id":"..." or "orderId":"..."
        size_t pos = json.find("\"order_id\":\"");
        if (pos == std::string::npos) {
            pos = json.find("\"orderId\":\"");
            if (pos == std::string::npos) return std::nullopt;
            pos += 13;
        } else {
            pos += 13;
        }
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return std::nullopt;
        return json.substr(pos, end - pos);
    }

    // Extract order_id from the payload (SignedOrder)
    static std::optional<std::string> extract_order_id_from_payload(const std::string& payload) {
        // The payload contains the nonce and salt — use as client_order_id
        // Look for "n":<nonce>
        size_t pos = payload.find("\"n\":");
        if (pos == std::string::npos) return std::nullopt;
        pos += 4;
        size_t end = payload.find_first_of(",}", pos);
        if (end == std::string::npos) return std::nullopt;
        std::string nonce_str = payload.substr(pos, end - pos);
        return "client_" + nonce_str;
    }

    // Extract error message from JSON error response
    static std::optional<std::string> extract_error_from_json(const std::string& json) {
        size_t pos = json.find("\"error\":\"");
        if (pos == std::string::npos) {
            pos = json.find("\"message\":\"");
            if (pos == std::string::npos) return std::nullopt;
            pos += 12;
        } else {
            pos += 11;
        }
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return std::nullopt;
        return json.substr(pos, end - pos);
    }

    // Parse balance JSON: {"USDC": 1234.56, "POL": 78.90}
    static BalanceResponse parse_balance_json(const std::string& json) {
        BalanceResponse bal{0.0, 0.0};
        size_t usdc_pos = json.find("\"USDC\":");
        if (usdc_pos != std::string::npos) {
            size_t start = usdc_pos + 7;
            size_t end = json.find_first_of(",}", start);
            if (end != std::string::npos) {
                bal.usdc_balance = std::stod(json.substr(start, end - start));
            }
        }
        size_t pol_pos = json.find("\"POL\":");
        if (pol_pos != std::string::npos) {
            size_t start = pol_pos + 7;
            size_t end = json.find_first_of(",}", start);
            if (end != std::string::npos) {
                bal.pol_balance = std::stod(json.substr(start, end - start));
            }
        }
        return bal;
    }
};

#endif // LIGHTWEIGHT_CLIENT_HPP
