#ifndef CURL_TRANSPORT_HPP
#define CURL_TRANSPORT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// libcurl implementation of clob::HttpTransport (network builds only).
//
// One persistent easy handle per transport, DNS/TLS/connection reuse across
// calls, bounded response capture with explicit truncation detection, no
// redirects, TLS peer+hostname verification always on.
//
// Ambiguity is classified conservatively: only failures that provably happen
// before any application data leaves the host (DNS, TCP connect, TLS handshake,
// local interface) are treated as "the venue never saw this".  Everything else
// — timeouts, send/receive errors, partial responses — is reported as
// `ambiguous`, which forces the caller to move the affected order to UNKNOWN
// and reconcile instead of retrying.  Retrying an ambiguous POST /order is how
// duplicate exposure happens.
// ─────────────────────────────────────────────────────────────────────────────

#include <curl/curl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "clob_rest_client.hpp"

namespace clob {

class CurlTransport : public HttpTransport {
public:
    CurlTransport() {
        static std::once_flag global_init;
        std::call_once(global_init,
                       [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
        handle_ = curl_easy_init();
    }

    ~CurlTransport() override {
        if (handle_) curl_easy_cleanup(handle_);
    }

    CurlTransport(const CurlTransport&) = delete;
    CurlTransport& operator=(const CurlTransport&) = delete;

    bool usable() const noexcept { return handle_ != nullptr; }

    // Warms DNS/TCP/TLS before the first latency-sensitive call.
    void warmup(const char* url) {
        if (!handle_ || !url || !*url) return;
        std::lock_guard<std::mutex> lock(mutex_);
        curl_easy_reset(handle_);
        curl_easy_setopt(handle_, CURLOPT_URL, url);
        curl_easy_setopt(handle_, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(handle_, CURLOPT_TIMEOUT_MS, 2000L);
        curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT_MS, 1500L);
        apply_tls_options();
        (void)curl_easy_perform(handle_);
    }

    void request(const char* method, const char* url, const char* body,
                 size_t body_len, const char* const* headers, size_t header_count,
                 const RequestOptions& options, char* body_buffer,
                 size_t body_capacity, HttpResponse& out) override {
        out = HttpResponse{};
        out.body = body_buffer;
        if (!handle_ || !method || !url) {
            std::snprintf(out.error, sizeof(out.error), "transport unavailable");
            out.ambiguous = true;
            return;
        }
        const bool plain_http = std::strncmp(url, "http://", 7) == 0;
        const bool loopback = std::strstr(url, "127.0.0.1") != nullptr ||
                              std::strstr(url, "localhost") != nullptr ||
                              std::strstr(url, "[::1]") != nullptr;
        if (plain_http && !(allow_loopback_plain_http_ && loopback)) {
            std::snprintf(out.error, sizeof(out.error),
                          "plaintext http is refused for non-loopback hosts");
            out.ambiguous = false;  // nothing was sent
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        Capture capture{body_buffer, body_capacity};
        curl_easy_reset(handle_);
        curl_easy_setopt(handle_, CURLOPT_URL, url);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(handle_, CURLOPT_PROTOCOLS_STR,
                         plain_http ? "http" : "https");
        curl_easy_setopt(handle_, CURLOPT_REDIR_PROTOCOLS_STR,
                         plain_http ? "http" : "https");
#else
        curl_easy_setopt(handle_, CURLOPT_PROTOCOLS,
                         plain_http ? static_cast<long>(CURLPROTO_HTTP)
                                    : static_cast<long>(CURLPROTO_HTTPS));
        curl_easy_setopt(handle_, CURLOPT_REDIR_PROTOCOLS,
                         plain_http ? static_cast<long>(CURLPROTO_HTTP)
                                    : static_cast<long>(CURLPROTO_HTTPS));
#endif
        curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, &write_callback);
        curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &capture);
        curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT_MS,
                         static_cast<long>(options.connect_timeout_ms));
        curl_easy_setopt(handle_, CURLOPT_TIMEOUT_MS,
                         static_cast<long>(options.total_timeout_ms));
        curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION,
                         options.allow_redirects ? 1L : 0L);
        curl_easy_setopt(handle_, CURLOPT_HTTP_VERSION,
                         static_cast<long>(CURL_HTTP_VERSION_1_1));
        apply_tls_options();

        if (std::strcmp(method, "GET") == 0) {
            curl_easy_setopt(handle_, CURLOPT_HTTPGET, 1L);
        } else if (std::strcmp(method, "POST") == 0) {
            curl_easy_setopt(handle_, CURLOPT_POST, 1L);
            curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body ? body : "");
            curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(body_len));
        } else if (std::strcmp(method, "DELETE") == 0) {
            curl_easy_setopt(handle_, CURLOPT_CUSTOMREQUEST, "DELETE");
            if (body && body_len) {
                curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body);
                curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE,
                                 static_cast<long>(body_len));
            }
        } else {
            std::snprintf(out.error, sizeof(out.error), "unsupported method");
            out.ambiguous = true;
            return;
        }

        curl_slist* header_list = nullptr;
        for (size_t i = 0; i < header_count && headers && headers[i]; ++i)
            header_list = curl_slist_append(header_list, headers[i]);
        if (header_count && !header_list) {
            std::snprintf(out.error, sizeof(out.error), "header allocation failed");
            // Nothing was sent: allocation happens before the transfer.
            out.ambiguous = false;
            return;
        }
        if (header_list) curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, header_list);

        const uint64_t started = now_micros();
        const CURLcode code = curl_easy_perform(handle_);
        out.elapsed_us = now_micros() - started;
        if (header_list) curl_slist_free_all(header_list);

        long http_code = 0;
        curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &http_code);
        out.code = http_code;
        out.body_len = capture.length;
        out.truncated = capture.truncated;
        if (code == CURLE_OK) {
            out.transport_ok = true;
            out.ambiguous = false;
            if (capture.truncated) {
                std::snprintf(out.error, sizeof(out.error),
                              "response exceeded the bounded buffer");
                out.transport_ok = false;
                // The request was processed; the response is unusable.
                out.ambiguous = true;
            }
            return;
        }
        out.transport_ok = false;
        out.ambiguous = is_ambiguous(code);
        std::snprintf(out.error, sizeof(out.error), "%s", curl_easy_strerror(code));
        requests_failed_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t failures() const noexcept {
        return requests_failed_.load(std::memory_order_relaxed);
    }

    // Optional public-key pinning in the repo's "sha256//<base64>" spelling.
    void set_tls_pin(const char* pins) { pin_.set(pins); }

    // Plain HTTP is refused everywhere except loopback, and only after an
    // explicit opt-in.  It exists for the in-process test venue
    // (tests/support/local_venue.hpp); production callers never set it, and the
    // order-submission client (LightweightCLOBClient) is HTTPS-only regardless
    // via CURLOPT_PROTOCOLS_STR.
    void allow_loopback_plain_http(bool allow) noexcept {
        allow_loopback_plain_http_ = allow;
    }

    // Failures that provably precede any application data are safe to retry;
    // everything else is ambiguous and must never be replayed.
    static bool is_ambiguous(CURLcode code) noexcept {
        switch (code) {
            case CURLE_OK:
            case CURLE_UNSUPPORTED_PROTOCOL:
            case CURLE_URL_MALFORMAT:
            case CURLE_COULDNT_RESOLVE_PROXY:
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_CONNECT:
            case CURLE_SSL_CONNECT_ERROR:      // handshake: no application data sent
            case CURLE_SSL_CERTPROBLEM:
            case CURLE_SSL_CIPHER:
            case CURLE_PEER_FAILED_VERIFICATION:  // CURLE_SSL_CACERT in older curl
            case CURLE_USE_SSL_FAILED:
            case CURLE_SSL_ENGINE_NOTFOUND:
            case CURLE_SSL_ENGINE_SETFAILED:
            case CURLE_INTERFACE_FAILED:
            case CURLE_TOO_MANY_REDIRECTS:
                return false;
            case CURLE_OPERATION_TIMEDOUT:     // may be connect or post-send
            case CURLE_SEND_ERROR:
            case CURLE_SEND_FAIL_REWIND:
            case CURLE_RECV_ERROR:
            case CURLE_PARTIAL_FILE:
            case CURLE_GOT_NOTHING:
            case CURLE_HTTP2_STREAM:
            case CURLE_HTTP2:
            case CURLE_SSL_SHUTDOWN_FAILED:
            case CURLE_ABORTED_BY_CALLBACK:
                return true;
            default:
                return true;  // unknown → conservative
        }
    }

private:
    struct Capture {
        char* buffer;
        size_t capacity;
        size_t length = 0;
        bool truncated = false;
    };

    static size_t write_callback(char* data, size_t size, size_t count, void* user) {
        auto* capture = static_cast<Capture*>(user);
        const size_t incoming = size * count;
        if (!capture->buffer || capture->capacity == 0) {
            capture->truncated = true;
            return incoming;  // drain without storing
        }
        const size_t room = capture->capacity - 1 - capture->length;
        const size_t copy = incoming < room ? incoming : room;
        if (copy) {
            std::memcpy(capture->buffer + capture->length, data, copy);
            capture->length += copy;
            capture->buffer[capture->length] = '\0';
        }
        if (copy < incoming) capture->truncated = true;
        return incoming;
    }

    void apply_tls_options() const {
        curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle_, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(handle_, CURLOPT_SSL_ENABLE_ALPN, 1L);
        // TLS 1.2 minimum, matching the WebSocket sessions.
        curl_easy_setopt(handle_, CURLOPT_SSLVERSION,
                         static_cast<long>(CURL_SSLVERSION_TLSv1_2));
        if (pin_.engine()) {
            curl_easy_setopt(handle_, CURLOPT_PINNEDPUBLICKEY, pin_.engine());
        }
    }

    static uint64_t now_micros() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
               static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
    }

    // Optional sha256//<base64> pin, translated to curl's PEM/base64 form.
    class Pin {
    public:
        void set(const char* pins) {
            value_[0] = '\0';
            if (!pins || !*pins) return;
            // Accept the repo's "sha256//<base64>" spelling (first pin only).
            const char* base64 = std::strstr(pins, "sha256//");
            if (!base64) return;
            base64 += 8;
            const char* end = std::strchr(base64, ';');
            const size_t length = end ? static_cast<size_t>(end - base64)
                                      : std::strlen(base64);
            if (length == 0 || length + 1 > sizeof(value_)) return;
            std::memcpy(value_, base64, length);
            value_[length] = '\0';
        }
        const char* engine() const noexcept { return value_[0] ? value_ : nullptr; }

    private:
        char value_[128]{};
    };

    CURL* handle_ = nullptr;
    bool allow_loopback_plain_http_ = false;
    std::mutex mutex_;
    Pin pin_;
    std::atomic<uint64_t> requests_failed_{0};
};

}  // namespace clob

#endif  // CURL_TRANSPORT_HPP
