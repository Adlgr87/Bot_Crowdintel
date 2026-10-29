// ─────────────────────────────────────────────────────────────────────────────
// test_cancel_id: order-id validation for DELETE /order
//
// The order id is interpolated into a JSON body, so it is an injection surface.
// These cases pin the boundary: only a bounded "0x" + hex string may reach the
// request, because the L2 HMAC signs exactly those bytes.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <cstring>

#include "../../core/src/lightweight_client.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
                ++g_failures; }                                                \
    } while (0)

using LightweightCLOBClient = ::LightweightCLOBClient;

static void test_valid_ids() {
    std::printf("valid_ids\n");
    CHECK(LightweightCLOBClient::valid_order_id(
              "0x0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
          "a canonical 32-byte order id is accepted");
    CHECK(LightweightCLOBClient::valid_order_id("0xab"), "a short hex id is accepted");
    CHECK(LightweightCLOBClient::valid_order_id("0xABCDEF"), "uppercase hex is accepted");
}

static void test_rejections() {
    std::printf("rejections\n");
    CHECK(!LightweightCLOBClient::valid_order_id(nullptr), "null is rejected");
    CHECK(!LightweightCLOBClient::valid_order_id(""), "empty is rejected");
    CHECK(!LightweightCLOBClient::valid_order_id("0x"), "bare prefix is rejected");
    CHECK(!LightweightCLOBClient::valid_order_id("abcdef"),
          "a missing 0x prefix is rejected");
    CHECK(!LightweightCLOBClient::valid_order_id("0xzz"), "non-hex is rejected");
    CHECK(!LightweightCLOBClient::valid_order_id("0x12 34"), "a space is rejected");
    CHECK(!LightweightCLOBClient::valid_order_id("0x12\n34"),
          "a newline is rejected");
    CHECK(!LightweightCLOBClient::valid_order_id("0x12;34"), "semicolon is rejected");

    // The case that matters: closing the JSON string and appending a field.
    CHECK(!LightweightCLOBClient::valid_order_id("0x12\",\"evil\":\"1"),
          "JSON injection is rejected");
    CHECK(!LightweightCLOBClient::valid_order_id("0x12}"),
          "closing brace is rejected");

    // Length bound: "0x" + 65 hex chars exceeds the cap.
    char too_long[80];
    too_long[0] = '0';
    too_long[1] = 'x';
    for (int i = 2; i < 79; ++i) too_long[i] = 'a';
    too_long[79] = '\0';
    CHECK(!LightweightCLOBClient::valid_order_id(too_long),
          "an over-long id is rejected");
}

int main() {
    std::printf("=== test_cancel_id (DELETE /order) ===\n");
    test_valid_ids();
    test_rejections();
    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
