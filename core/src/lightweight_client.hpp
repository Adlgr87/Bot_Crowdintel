#ifndef LIGHTWEIGHT_CLIENT_HPP
#define LIGHTWEIGHT_CLIENT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// LightweightCLOBClient: HTTPS order submission for Polymarket CLOB V2.
//
// Wire protocol (docs.polymarket.com — L2 auth headers are unchanged in V2):
//   POST {host}/order
//   POLY_ADDRESS / POLY_SIGNATURE / POLY_TIMESTAMP / POLY_API_KEY / POLY_PASSPHRASE
//   POLY_SIGNATURE = base64url( HMAC-SHA256( base64url_decode(secret),
//                                            ts_seconds + "POST" + "/order" + body ) )
//
// Hot-path design:
//   - ONE persistent curl easy handle (connection + TLS session reuse; the
//     TCP/TLS handshake happens once, not per order).
//   - HMAC key midstates precomputed once → per-order HMAC ≈ 2 SHA-256 blocks.
//   - Secret decoded into a fixed buffer and wiped on destruction (volatile
//     stores the optimizer cannot elide).
//   - Optional TLS public-key pinning (CURLOPT_PINNEDPUBLICKEY, "sha256//...").
//   - Response captured into a fixed buffer; the CLOB orderID is extracted so
//     the caller can track/cancel the order.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <curl/curl.h>

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"
#include "market_config.hpp"
#include "polymarket_order.hpp"

class LightweightCLOBClient {
public:
    LightweightCLOBClient(const MarketConfig& cfg) : cfg_(cfg) {
        static std::once_flag curl_once;
        std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

        // Decode the base64url secret ONCE — the HMAC key is the raw bytes.
        const size_t slen = std::strlen(cfg.api_secret_b64);
        secret_len_ = base64url_decode(cfg.api_secret_b64, slen, secret_raw_);
        if (secret_len_ == 0 || secret_len_ == (size_t)-1 ||
            secret_len_ > sizeof(secret_raw_)) {
            std::fprintf(stderr, "FATAL: CLOB_SECRET is not valid base64url\n");
            std::exit(1);
        }
        hmac_.set_key(secret_raw_, secret_len_);

        // Persistent handle: connection cache + TLS session survive across orders.
        curl_ = curl_easy_init();
        if (!curl_) { std::fprintf(stderr, "FATAL: curl_easy_init failed\n"); std::exit(1); }
        std::snprintf(url_, sizeof(url_), "%s/order", cfg.clob_host);
        curl_easy_setopt(curl_, CURLOPT_URL, url_);
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, nullptr);  // identity — no decompress latency
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, "crowdintel-bot/2.0");
        if (cfg.tls_pin[0]) {
            curl_easy_setopt(curl_, CURLOPT_PINNEDPUBLICKEY, cfg.tls_pin);
        }
    }

    ~LightweightCLOBClient() {
        if (curl_) {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
        secure_zero(secret_raw_, sizeof(secret_raw_));
        secure_zero(&hmac_, sizeof(hmac_));
    }

    LightweightCLOBClient(const LightweightCLOBClient&) = delete;
    LightweightCLOBClient& operator=(const LightweightCLOBClient&) = delete;

    // HOT PATH (network egress). `body` is the fully built, signed wire JSON.
    SubmitResult submit(const WireBody& body) {
        SubmitResult res{false, 0, {0}};
        resp_len_ = 0;

        // 1. Timestamp (seconds) for both the header and the HMAC message.
        char ts[24];
        const uint64_t now_sec = now_unix_seconds();
        const size_t ts_len = u64_to_dec(now_sec, ts);

        // 2. HMAC over  ts + "POST" + "/order" + body  (stack-joined message;
        //    ≈700-1200 bytes → one streaming pass over precomputed midstates).
        char sig_b64[48];
        uint8_t digest[32];
        {
            char msg[1600 + 64];
            size_t off = 0;
            std::memcpy(msg + off, ts, ts_len); off += ts_len;
            std::memcpy(msg + off, "POST", 4); off += 4;
            std::memcpy(msg + off, "/order", 6); off += 6;
            std::memcpy(msg + off, body.buf, body.len); off += body.len;
            hmac_.compute((const uint8_t*)msg, off, digest);
            secure_zero(msg, off);
        }
        const size_t sig_len = base64url_encode(digest, 32, sig_b64);
        sig_b64[sig_len] = '\0';

        // 3. Headers (two of them vary per request → rebuilt per call).
        char h_addr[80], h_sig[96], h_ts[48], h_key[96], h_pp[160], h_ct[40];
        std::snprintf(h_addr, sizeof(h_addr), "POLY_ADDRESS: %s", cfg_.maker_hex);
        std::snprintf(h_sig,  sizeof(h_sig),  "POLY_SIGNATURE: %s", sig_b64);
        std::snprintf(h_ts,   sizeof(h_ts),   "POLY_TIMESTAMP: %.*s", (int)ts_len, ts);
        std::snprintf(h_key,  sizeof(h_key),  "POLY_API_KEY: %s", cfg_.owner_api_key);
        std::snprintf(h_pp,   sizeof(h_pp),   "POLY_PASSPHRASE: %s", cfg_.api_passphrase);
        std::snprintf(h_ct,   sizeof(h_ct),   "Content-Type: application/json");

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, h_addr);
        headers = curl_slist_append(headers, h_sig);
        headers = curl_slist_append(headers, h_ts);
        headers = curl_slist_append(headers, h_key);
        headers = curl_slist_append(headers, h_pp);
        headers = curl_slist_append(headers, h_ct);

        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.buf);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)body.len);

        // 4. Send. libcurl reuses the keep-alive connection.
        const CURLcode rc = curl_easy_perform(curl_);
        if (rc == CURLE_OK) {
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &res.http_code);
            res.ok = (res.http_code >= 200 && res.http_code < 300);
        }
        if (res.ok && resp_len_) extract_order_id(res.order_id, sizeof(res.order_id));

        curl_slist_free_all(headers);
        return res;
    }

private:
    static uint64_t now_unix_seconds() {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec;
    }

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<LightweightCLOBClient*>(userdata);
        const size_t total = size * nmemb;
        const size_t space = sizeof(self->resp_) - self->resp_len_ - 1;
        const size_t take = total < space ? total : space;
        if (take) {
            std::memcpy(self->resp_ + self->resp_len_, ptr, take);
            self->resp_len_ += take;
            self->resp_[self->resp_len_] = '\0';
        }
        return total;  // always consume everything
    }

    void extract_order_id(char* out, size_t cap) const {
        static constexpr char K[] = "\"orderID\":\"";
        const char* hit = std::strstr(resp_, K);
        if (!hit) return;
        hit += sizeof(K) - 1;
        size_t i = 0;
        while (hit[i] && hit[i] != '"' && i < cap - 1) { out[i] = hit[i]; ++i; }
        out[i] = '\0';
    }

    const MarketConfig& cfg_;
    CURL* curl_ = nullptr;
    char  url_[192];
    HmacSha256 hmac_;
    uint8_t secret_raw_[64];
    size_t  secret_len_ = 0;
    char    resp_[1024];
    size_t  resp_len_ = 0;
};

#endif // LIGHTWEIGHT_CLIENT_HPP
