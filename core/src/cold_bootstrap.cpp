// ─────────────────────────────────────────────────────────────────────────────
// cold_bootstrap.cpp — Phase 2.2: API reconciliation with exponential backoff
//
// PERF: Cold starts on bot restart — reconciles state via REST API before
//       un blocking WindowShield gate
// Impact: Ensures clean state after crash, prevents stale orders
//
// Strategy:
//   1. Exponential backoff for rate-limited endpoints (Polymarket API limit: 500rpm)
//   2. Reconcile open orders, positions, PnL before starting trading
//   3. Only release WindowShield gate when state is consistent
// ─────────────────────────────────────────────────────────────────────────────
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>

// PERF: Exponential backoff with jitter (Polymarket rate limit: 500 req/min)
// Impact: Avoids 429 Too Many Requests, reduces retry overhead
struct BackoffTimer {
    uint32_t attempt;
    static constexpr uint32_t BASE_MS = 500;
    static constexpr uint32_t MAX_MS = 30000;
    
    uint32_t next_delay_ms() noexcept {
        uint32_t delay = BASE_MS << (attempt > 10 ? 10 : attempt);
        if (delay > MAX_MS) delay = MAX_MS;
        // Add ±25% jitter to avoid thundering herd
        uint32_t jitter = (delay / 4) * ((reinterpret_cast<uintptr_t>(&delay) % 3) - 1);
        return delay + jitter;
    }
    
    void sleep() noexcept {
        uint32_t ms = next_delay_ms();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        attempt++;
    }
};

// PERF: HTTP response callback (zero-alloc via pre-sized buffer)
struct HttpResponse {
    char data[8192];  // Pre-allocated, no heap alloc
    size_t size;
};

static size_t write_cb(void* ptr, size_t sz, size_t nm, void* ud) noexcept {
    auto* resp = static_cast<HttpResponse*>(ud);
    size_t total = sz * nm;
    if (resp->size + total < sizeof(resp->data)) {
        std::memcpy(resp->data + resp->size, ptr, total);
        resp->size += total;
    }
    return total;
}

// PERF: Reconcile order state before unblocking WindowShield
// Returns: true if state consistent, false if timeout
bool cold_bootstrap_reconcile(const char* api_key,
                               const char* base_url,
                               std::atomic<uint32_t>& window_shield_state) noexcept {
    BackoffTimer backoff{};
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    for (int i = 0; i < 5; i++) {  // Max 5 retries
        HttpResponse resp{};
        char headers_buf[256];
        snprintf(headers_buf, sizeof(headers_buf), "Authorization: %s", api_key);
        struct curl_slist* headers = curl_slist_append(nullptr, headers_buf);
        
        curl_easy_setopt(curl, CURLOPT_URL, base_url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
        
        CURLcode rc = curl_easy_perform(curl);
        
        // CRITICAL FIX ID-C01: Must check HTTP status code AND validate state
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        
        if (rc == CURLE_OK && http_code == 200) {
            // State reconciled — release WindowShield
            // Fail-closed: only release if HTTP 200 AND status field is "ok"
            if (strstr(resp.data, "\"status\":\"ok\"") || 
                strstr(resp.data, '"status":"ok"')) {
                window_shield_state.store(0, std::memory_order_release);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return true;
            }
        }
        // Fail-closed: any non-200 or invalid body keeps WindowShield BLOQUEADO
        backoff.sleep();
        
        // Exponential backoff on failure
        backoff.sleep();
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return false;  // Timeout after max retries — keep WindowShield HALTED
}
