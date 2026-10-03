#ifndef FIXTURE_TRANSPORT_HPP
#define FIXTURE_TRANSPORT_HPP

// Test/paper-only HTTP transport: returns canned responses and records the
// requests it received.  It is intentionally *not* reachable from the live
// binary — it lives under tests/ and is only included by test and paper-mode
// translation units (see docs/CANARY_CHECKLIST.md, "mocks never in live").

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../core/src/clob_rest_client.hpp"

namespace testing {

struct RecordedRequest {
    char method[8]{};
    char url[512]{};
    char headers[8][256]{};
    size_t header_count = 0;
    char body[2048]{};
    size_t body_len = 0;
};

struct FixtureResponse {
    long code = 200;
    char body[4096]{};
    bool transport_ok = true;
    bool ambiguous = false;
    bool truncated = false;
    char error[64]{};
};

class FixtureTransport : public clob::HttpTransport {
public:
    static constexpr size_t K_MAX_RULES = 32;
    static constexpr size_t K_MAX_REQUESTS = 64;

    // Registers a response for any request whose URL contains `url_fragment`
    // and whose method matches.  Rules are evaluated in insertion order.
    bool on(const char* method, const char* url_fragment, long code,
            const char* body) {
        if (rule_count_ >= K_MAX_RULES) return false;
        Rule& rule = rules_[rule_count_++];
        std::snprintf(rule.method, sizeof(rule.method), "%s", method);
        std::snprintf(rule.url_fragment, sizeof(rule.url_fragment), "%s", url_fragment);
        rule.response.code = code;
        std::snprintf(rule.response.body, sizeof(rule.response.body), "%s",
                      body ? body : "");
        rule.response.transport_ok = true;
        rule.response.ambiguous = false;
        rule.response.truncated = false;
        rule.response.error[0] = '\0';
        return true;
    }

    // Registers a transport-level outcome (timeout, connection failure, …).
    bool on_transport(const char* method, const char* url_fragment, bool ambiguous,
                      const char* error) {
        if (rule_count_ >= K_MAX_RULES) return false;
        Rule& rule = rules_[rule_count_++];
        std::snprintf(rule.method, sizeof(rule.method), "%s", method);
        std::snprintf(rule.url_fragment, sizeof(rule.url_fragment), "%s", url_fragment);
        rule.response.code = 0;
        rule.response.body[0] = '\0';
        rule.response.transport_ok = false;
        rule.response.ambiguous = ambiguous;
        std::snprintf(rule.response.error, sizeof(rule.response.error), "%s",
                      error ? error : "transport failure");
        return true;
    }

    // Consumes the next queued response for `url_fragment` (fault injection:
    // first call times out, second succeeds).
    bool queue_once(const char* url_fragment, const FixtureResponse& response) {
        if (queue_count_ >= K_MAX_RULES) return false;
        Rule& rule = queue_[queue_count_++];
        rule.method[0] = '\0';  // any method
        std::snprintf(rule.url_fragment, sizeof(rule.url_fragment), "%s", url_fragment);
        rule.response = response;
        rule.consumed = false;
        return true;
    }

    void request(const char* method, const char* url, const char* body,
                 size_t body_len, const char* const* headers, size_t header_count,
                 const clob::RequestOptions& options, char* body_buffer,
                 size_t body_capacity, clob::HttpResponse& out) override {
        (void)options;
        out = clob::HttpResponse{};
        out.body = body_buffer;
        record(method, url, body, body_len, headers, header_count);

        // One-shot queue first (fault injection).
        for (size_t i = 0; i < queue_count_; ++i) {
            Rule& rule = queue_[i];
            if (rule.consumed) continue;
            if (!matches(rule, method, url)) continue;
            rule.consumed = true;
            finish(rule.response, body_buffer, body_capacity, out);
            return;
        }
        for (size_t i = 0; i < rule_count_; ++i) {
            const Rule& rule = rules_[i];
            if (!matches(rule, method, url)) continue;
            finish(rule.response, body_buffer, body_capacity, out);
            return;
        }
        out.transport_ok = false;
        out.ambiguous = false;
        std::snprintf(out.error, sizeof(out.error), "no fixture for request");
    }

    size_t request_count() const noexcept { return request_count_; }
    const RecordedRequest& request_at(size_t index) const noexcept {
        return requests_[index % K_MAX_REQUESTS];
    }
    const RecordedRequest* last() const noexcept {
        return request_count_ ? &requests_[(request_count_ - 1) % K_MAX_REQUESTS]
                              : nullptr;
    }
    void reset() noexcept {
        request_count_ = 0;
        for (auto& request : requests_) request = RecordedRequest{};
    }

    // Drops every registered rule and queued one-shot (fixtures are matched in
    // registration order, so a test that changes a response must clear first).
    void clear_rules() noexcept {
        rule_count_ = 0;
        queue_count_ = 0;
        for (auto& rule : rules_) rule = Rule{};
        for (auto& rule : queue_) rule = Rule{};
    }

private:
    struct Rule {
        char method[8]{};
        char url_fragment[256]{};
        FixtureResponse response{};
        bool consumed = false;
    };

    static bool matches(const Rule& rule, const char* method, const char* url) {
        if (rule.method[0] && std::strcmp(rule.method, method) != 0) return false;
        return std::strstr(url, rule.url_fragment) != nullptr;
    }

    void record(const char* method, const char* url, const char* body,
                size_t body_len, const char* const* headers, size_t header_count) {
        RecordedRequest& request = requests_[request_count_ % K_MAX_REQUESTS];
        request = RecordedRequest{};
        std::snprintf(request.method, sizeof(request.method), "%s", method);
        std::snprintf(request.url, sizeof(request.url), "%s", url);
        const size_t count = header_count < 8 ? header_count : 8;
        for (size_t i = 0; i < count; ++i) {
            std::snprintf(request.headers[i], sizeof(request.headers[i]), "%s",
                          headers[i] ? headers[i] : "");
        }
        request.header_count = count;
        const size_t copy = body_len < sizeof(request.body) - 1
                                ? body_len : sizeof(request.body) - 1;
        if (body && copy) std::memcpy(request.body, body, copy);
        request.body[copy] = '\0';
        request.body_len = copy;
        ++request_count_;
    }

    static void finish(const FixtureResponse& fixture, char* body_buffer,
                       size_t body_capacity, clob::HttpResponse& out) {
        out.code = fixture.code;
        out.transport_ok = fixture.transport_ok;
        out.ambiguous = fixture.ambiguous;
        out.truncated = fixture.truncated;
        std::snprintf(out.error, sizeof(out.error), "%s", fixture.error);
        const size_t length = std::strlen(fixture.body);
        if (length >= body_capacity) {
            out.truncated = true;
            out.body_len = body_capacity ? body_capacity - 1 : 0;
        } else {
            out.body_len = length;
        }
        if (body_buffer && out.body_len) {
            std::memcpy(body_buffer, fixture.body, out.body_len);
            body_buffer[out.body_len] = '\0';
        }
    }

    Rule rules_[K_MAX_RULES]{};
    size_t rule_count_ = 0;
    Rule queue_[K_MAX_RULES]{};
    size_t queue_count_ = 0;
    RecordedRequest requests_[K_MAX_REQUESTS]{};
    size_t request_count_ = 0;
};

}  // namespace testing

#endif  // FIXTURE_TRANSPORT_HPP
