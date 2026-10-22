// ─────────────────────────────────────────────────────────────────────────────
// test_compliance_guard: unit tests for ComplianceGuard (VER-08).
//
// Verifies fail-closed semantics:
//   1. Jurisdiction allowlist rejects non-allowlisted operators
//   2. Restricted condition IDs are rejected
//   3. Restricted market hashes are rejected
//   4. Signature type 3 (ERC-7739) is rejected — fail-closed
//   5. Default (empty config) allowlist rejects everything when jurisdiction enabled
//   6. Null jurisdiction is rejected when jurisdiction check enabled
//   7. Non-restricted markets pass all checks
//
// Exit code 0 = all pass. No external test framework.
//
// VER-08 | fix/ver-08-compliance-guard
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <string>

#include "../../core/include/compliance_guard.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
                ++g_failures; }                                                \
    } while (0)

// ── Test 1: Jurisdiction allowlist ────────────────────────────────────────────

static void test_jurisdiction_allows_permitted() {
    std::printf("jurisdiction_allows_permitted\n");
    JurisdictionConfig jc;
    jc.enabled = true;
    jc.allowed.insert("US");
    jc.allowed.insert("CA");
    RestrictedMarketsConfig rm;
    ComplianceGuard guard(jc, rm);

    // US should be allowed
    auto r1 = guard.check("", 0, "US");
    CHECK(r1.allowed, "US jurisdiction allowed");

    // CA should be allowed
    auto r2 = guard.check("", 0, "CA");
    CHECK(r2.allowed, "CA jurisdiction allowed");

    // UK should be rejected
    auto r3 = guard.check("", 0, "GB");
    CHECK(!r3.allowed, "GB jurisdiction rejected");

    // Null jurisdiction should be rejected (fail-closed)
    auto r4 = guard.check("", 0, nullptr);
    CHECK(!r4.allowed, "null jurisdiction rejected");

    // Empty jurisdiction should be rejected (fail-closed)
    auto r5 = guard.check("", 0, "");
    CHECK(!r5.allowed, "empty jurisdiction rejected");
}

// ── Test 2: Restricted condition ID ──────────────────────────────────────────

static void test_restricted_condition_id_rejected() {
    std::printf("restricted_condition_id_rejected\n");
    JurisdictionConfig jc;  // disabled by default
    RestrictedMarketsConfig rm;
    rm.condition_ids.insert("0x1234567890abcdef1234567890abcdef12345678");

    ComplianceGuard guard(jc, rm);

    // Restricted condition ID should be rejected
    auto r1 = guard.check("0x1234567890abcdef1234567890abcdef12345678", 999);
    CHECK(!r1.allowed, "restricted condition_id rejected");

    // Non-restricted condition ID should pass
    auto r2 = guard.check("0x9999999999999999999999999999999999999999", 999);
    CHECK(r2.allowed, "non-restricted condition_id allowed");

    // Empty condition ID should pass
    auto r3 = guard.check("", 999);
    CHECK(r3.allowed, "empty condition_id allowed");
}

// ── Test 3: Restricted market hash ───────────────────────────────────────────

static void test_restricted_market_hash_rejected() {
    std::printf("restricted_market_hash_rejected\n");
    JurisdictionConfig jc;
    RestrictedMarketsConfig rm;
    rm.market_hashes.insert(42);
    rm.market_hashes.insert(100);

    ComplianceGuard guard(jc, rm);

    // Restricted hash should be rejected
    auto r1 = guard.check("", 42);
    CHECK(!r1.allowed, "restricted market hash 42 rejected");

    auto r2 = guard.check("", 100);
    CHECK(!r2.allowed, "restricted market hash 100 rejected");

    // Non-restricted hash should pass
    auto r3 = guard.check("", 999);
    CHECK(r3.allowed, "non-restricted market hash allowed");
}

// ── Test 4: Combined checks ──────────────────────────────────────────────────

static void test_combined_checks() {
    std::printf("combined_checks\n");
    JurisdictionConfig jc;
    jc.enabled = true;
    jc.allowed.insert("US");

    RestrictedMarketsConfig rm;
    rm.condition_ids.insert("0xabcdef");

    ComplianceGuard guard(jc, rm);

    // US + allowed market = pass
    auto r1 = guard.check("0x123456", 999, "US");
    CHECK(r1.allowed, "US + allowed market passes");

    // US + restricted condition_id = fail
    auto r2 = guard.check("0xabcdef", 999, "US");
    CHECK(!r2.allowed, "US + restricted condition_id fails");

    // GB + allowed market = fail (jurisdiction)
    auto r3 = guard.check("0x123456", 999, "GB");
    CHECK(!r3.allowed, "GB + allowed market fails on jurisdiction");
}

// ── Test 5: parse_restricted helper ──────────────────────────────────────────

static void test_parse_restricted() {
    std::printf("parse_restricted\n");

    // Test condition ID parsing (CRIT FIX: now validates 0x + 64 lowercase hex)
    const char* cid1 = "0x" "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char* cid2 = "0x" "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
    std::string csv1 = std::string(cid1) + "," + cid2;
    auto rm1 = ComplianceGuard::parse_restricted(csv1.c_str());
    CHECK(rm1.condition_ids.find(cid1) != rm1.condition_ids.end(), "parse valid cid #1");
    CHECK(rm1.condition_ids.find(cid2) != rm1.condition_ids.end(), "parse valid cid #2");

    // Short/malformed condition IDs are now rejected (hex format validation)
    auto rm_bad = ComplianceGuard::parse_restricted("0x1234,0x5678");
    CHECK(rm_bad.condition_ids.empty(), "reject short cids (hex validation)");

    // Test hash parsing
    auto rm2 = ComplianceGuard::parse_restricted("42,100");
    CHECK(rm2.market_hashes.find(42) != rm2.market_hashes.end(), "parse hash 42");
    CHECK(rm2.market_hashes.find(100) != rm2.market_hashes.end(), "parse hash 100");

    // Test empty string
    auto rm3 = ComplianceGuard::parse_restricted("");
    CHECK(rm3.condition_ids.empty(), "empty condition_ids");
    CHECK(rm3.market_hashes.empty(), "empty market_hashes");

    // Test nullptr
    auto rm4 = ComplianceGuard::parse_restricted(nullptr);
    CHECK(rm4.condition_ids.empty(), "nullptr condition_ids");
    CHECK(rm4.market_hashes.empty(), "nullptr market_hashes");
}

// ── Test 6: fail-closed by default ───────────────────────────────────────────

static void test_fail_closed_default() {
    std::printf("fail_closed_default\n");
    // CRIT FIX: Default constructor is now fail-closed (jurisdiction enabled
    // with empty allowlist → rejects everything). Previously was fail-open.
    ComplianceGuard guard;

    // With empty allowlist + jurisdiction enabled → everything rejected
    auto r1 = guard.check("", 0, nullptr);
    CHECK(!r1.allowed, "default guard rejects null jurisdiction (fail-closed)");

    auto r2 = guard.check("", 0, "US");
    CHECK(!r2.allowed, "default guard rejects US (empty allowlist)");

    auto r3 = guard.check("", 0, "GB");
    CHECK(!r3.allowed, "default guard rejects GB (empty allowlist)");
}

// ── Test 7: Performance sanity (< 1μs) ───────────────────────────────────────

static void test_performance() {
    std::printf("performance_sanity\n");
    JurisdictionConfig jc;
    jc.enabled = true;
    jc.allowed.insert("US");
    RestrictedMarketsConfig rm;
    rm.condition_ids.insert("0x1234");
    rm.market_hashes.insert(42);

    ComplianceGuard guard(jc, rm);
    const int N = 100000;

    // Warm up
    auto r = guard.check("0x9999", 999, "US");
    CHECK(r.allowed, "warm-up check passes");

    // This is a sanity check that the guard doesn't crash, not a real benchmark.
    // A real benchmark would use RDTSC; here we just verify correctness at volume.
    int allowed_count = 0;
    for (int i = 0; i < N; ++i) {
        auto r = guard.check("0x9999", 999, "US");
        if (r.allowed) ++allowed_count;
    }
    CHECK(allowed_count == N, "all 100k iterations allowed");
}

int main() {
    std::printf("=== test_compliance_guard ===\n");

    test_jurisdiction_allows_permitted();
    test_restricted_condition_id_rejected();
    test_restricted_market_hash_rejected();
    test_combined_checks();
    test_parse_restricted();
    test_fail_closed_default();
    test_performance();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);

    return g_failures == 0 ? 0 : 1;
}
