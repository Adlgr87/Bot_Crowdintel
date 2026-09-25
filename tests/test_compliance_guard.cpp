/**
 * test_compliance_guard.cpp — T5-4 Compliance + Market Metadata Tests
 *
 * Verifies:
 *   1. Token in blocklist → rejected (is_token_allowed returns false)
 *   2. Market closed → blocks (verify_market_active / check_all blocks)
 *   3. Tick size applied correctly (apply_tick_size snaps to tick)
 *   4. Jurisdiction not configured → blocked (fail-closed, never hardcode "allow all")
 *
 * Additional coverage:
 *   5. check_all() integration — full pipeline OK path
 *   6. Market resolved status → blocks
 *   7. enable_dynamic_fees flag round-trip
 *   8. resolution_time field present and usable
 *   9. Jurisdiction disabled → operator opt-in (all allowed)
 */

#include "compliance_guard.hpp"
#include "market_metadata.hpp"
#include "tick_result.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <unordered_set>

// ─── Minimal test harness ─────────────────────────────────────────────

static int g_tests_run = 0;
static int g_tests_passed = 0;

static void run_test(const char* name, bool (*fn)()) {
    g_tests_run++;
    std::cout << "▶ " << name << " ... " << std::flush;
    bool ok = fn();
    if (ok) {
        g_tests_passed++;
        std::cout << "✅ PASS" << std::endl;
    } else {
        std::cout << "❌ FAIL" << std::endl;
    }
}

// ─── Test fixtures ────────────────────────────────────────────────────

static ComplianceConfig make_default_config() {
    ComplianceConfig cfg;
    cfg.jurisdiction_check_enabled = false;  // disabled by default for token-focused tests
    cfg.allowed_jurisdictions.clear();
    cfg.restricted_tokens.clear();
    cfg.resolution_warning_hours = 24;
    return cfg;
}

static MarketMetadata make_metadata(const std::string& token_id,
                                     const std::string& status,
                                     int tick_size,
                                     bool dynamic_fees = false,
                                     bool near_resolution = false) {
    MarketMetadata meta;
    meta.token_id = token_id;
    meta.question = "Test question";
    meta.outcome_token = "Yes";
    meta.tick_size = tick_size;
    meta.status = status;
    meta.enable_dynamic_fees = dynamic_fees;
    if (near_resolution) {
        // Resolution time 1 hour in the future → is_resolving_soon(24) = true
        meta.resolution_time = std::chrono::system_clock::now() +
                               std::chrono::hours(1);
    } else {
        // Far in the future so is_resolving_soon(warning_hours) = false
        meta.resolution_time = std::chrono::system_clock::time_point::max();
    }
    meta.resolution_src = std::nullopt;
    meta.resolution_criteria = std::nullopt;
    return meta;
}

// ─── Test 1: Token in blocklist → rejected ────────────────────────────

static bool test_token_blocklist() {
    ComplianceConfig cfg = make_default_config();
    cfg.restricted_tokens.insert("BLOCKED_TOKEN");
    cfg.restricted_tokens.insert("SCAM_TOKEN");

    ComplianceGuard guard(cfg);

    // Token in blocklist → NOT allowed
    if (guard.is_token_allowed("BLOCKED_TOKEN")) return false;
    if (guard.is_token_allowed("SCAM_TOKEN")) return false;

    // Token NOT in blocklist → allowed
    if (!guard.is_token_allowed("GOOD_TOKEN")) return false;

    // Empty restricted list → all allowed
    ComplianceConfig cfg2 = make_default_config();
    ComplianceGuard guard2(cfg2);
    if (!guard2.is_token_allowed("ANY_TOKEN")) return false;

    // check_all() should return MARKET_NOT_TRADABLE for blocked token
    MarketMetadataCache cache;
    cache.set_metadata(make_metadata("BLOCKED_TOKEN", "active", 100));

    TickResult result = guard.check_all("BLOCKED_TOKEN", "US", cache);
    return (result == TickResult::MARKET_NOT_TRADABLE);
}

// ─── Test 2: Market closed/resolved → blocks ───────────────────────────

static bool test_market_closed() {
    ComplianceConfig cfg = make_default_config();
    ComplianceGuard guard(cfg);

    MarketMetadataCache cache;
    cache.set_metadata(make_metadata("ACTIVE_TOKEN", "active", 100));
    cache.set_metadata(make_metadata("CLOSED_TOKEN", "closed", 100));
    cache.set_metadata(make_metadata("RESOLVED_TOKEN", "resolved", 100));

    // Active market → tradable
    if (!guard.verify_market_active("ACTIVE_TOKEN", cache)) return false;

    // Closed market → NOT tradable
    if (guard.verify_market_active("CLOSED_TOKEN", cache)) return false;

    // Resolved market → NOT tradable
    if (guard.verify_market_active("RESOLVED_TOKEN", cache)) return false;

    // Unknown market (not in cache) → NOT tradable (safe default)
    if (guard.verify_market_active("MISSING_TOKEN", cache)) return false;

    // check_all() blocks closed market even with jurisdiction allow
    ComplianceConfig cfg2;
    cfg2.jurisdiction_check_enabled = true;
    cfg2.allowed_jurisdictions.insert("US");
    cfg2.resolution_warning_hours = 24;
    ComplianceGuard guard2(cfg2);
    TickResult result = guard2.check_all("CLOSED_TOKEN", "US", cache);
    return (result == TickResult::MARKET_NOT_TRADABLE);
}

// ─── Test 3: Tick size applied correctly ─────────────────────────────

static bool test_tick_size() {
    // tick_size=100 → snap to nearest 100 micro-units
    assert(MarketMetadataCache::apply_tick_size(50000, 100) == 50000);
    assert(MarketMetadataCache::apply_tick_size(50099, 100) == 50000);
    assert(MarketMetadataCache::apply_tick_size(50100, 100) == 50100);
    assert(MarketMetadataCache::apply_tick_size(999999, 100) == 999900);

    // tick_size=50
    assert(MarketMetadataCache::apply_tick_size(50025, 50) == 50000);
    assert(MarketMetadataCache::apply_tick_size(50050, 50) == 50050);

    // tick_size=1 (no snapping)
    assert(MarketMetadataCache::apply_tick_size(50001, 1) == 50001);

    // tick_size=0 or negative → no snapping (safety)
    assert(MarketMetadataCache::apply_tick_size(50001, 0) == 50001);
    assert(MarketMetadataCache::apply_tick_size(50001, -1) == 50001);

    // Dynamic per-market tick_size retrieval
    MarketMetadataCache cache;
    cache.set_metadata(make_metadata("T100", "active", 100));
    cache.set_metadata(make_metadata("T50", "active", 50));
    cache.set_metadata(make_metadata("T10", "active", 10));

    assert(cache.get_tick_size("T100") == 100);
    assert(cache.get_tick_size("T50") == 50);
    assert(cache.get_tick_size("T10") == 10);
    assert(cache.get_tick_size("UNKNOWN") == 1);  // default: no snapping

    return true;
}

// ─── Test 4: Jurisdiction not configured → blocked (fail-closed) ────────

static bool test_jurisdiction_not_configured() {
    // CRITICAL: When jurisdiction_check_enabled=true and NO jurisdictions
    // are configured, ALL jurisdictions must be blocked (fail-closed).
    // The code NEVER hardcodes "allow all" in this path.
    ComplianceConfig cfg;
    cfg.jurisdiction_check_enabled = true;
    cfg.allowed_jurisdictions.clear();  // empty — no jurisdictions configured
    cfg.resolution_warning_hours = 24;

    ComplianceGuard guard(cfg);

    // Even "US" (the default) must be blocked when not in the allowed set
    if (guard.verify_jurisdiction("US")) return false;
    if (guard.verify_jurisdiction("GB")) return false;
    if (guard.verify_jurisdiction("DE")) return false;
    if (guard.verify_jurisdiction("")) return false;

    // check_all() must also block
    MarketMetadataCache cache;
    cache.set_metadata(make_metadata("GOOD_TOKEN", "active", 100));
    TickResult result = guard.check_all("GOOD_TOKEN", "US", cache);
    return (result == TickResult::MARKET_NOT_TRADABLE);
}

// ─── Test 5: Jurisdiction allow-list working correctly ─────────────────

static bool test_jurisdiction_allowlist() {
    ComplianceConfig cfg;
    cfg.jurisdiction_check_enabled = true;
    cfg.allowed_jurisdictions.insert("US");
    cfg.allowed_jurisdictions.insert("CA");
    cfg.allowed_jurisdictions.insert("GB");
    cfg.resolution_warning_hours = 24;

    ComplianceGuard guard(cfg);

    // Allowed jurisdictions → true
    if (!guard.verify_jurisdiction("US")) return false;
    if (!guard.verify_jurisdiction("CA")) return false;
    if (!guard.verify_jurisdiction("GB")) return false;

    // Not allowed → false
    if (guard.verify_jurisdiction("CN")) return false;
    if (guard.verify_jurisdiction("RU")) return false;
    if (guard.verify_jurisdiction("DE")) return false;

    return true;
}

// ─── Test 6: check_all() full integration ──────────────────────────────

static bool test_check_all_integration() {
    ComplianceConfig cfg;
    cfg.jurisdiction_check_enabled = true;
    cfg.allowed_jurisdictions.insert("US");
    cfg.restricted_tokens.insert("BLOCKED");
    cfg.resolution_warning_hours = 24;

    ComplianceGuard guard(cfg);
    MarketMetadataCache cache;
    cache.set_metadata(make_metadata("GOOD_TOKEN", "active", 100));
    cache.set_metadata(make_metadata("BLOCKED", "active", 100));
    cache.set_metadata(make_metadata("CLOSED_TOKEN", "closed", 100));

    // All checks pass → OK
    if (guard.check_all("GOOD_TOKEN", "US", cache) != TickResult::OK) return false;

    // Blocked token → MARKET_NOT_TRADABLE
    if (guard.check_all("BLOCKED", "US", cache) != TickResult::MARKET_NOT_TRADABLE)
        return false;

    // Bad jurisdiction → MARKET_NOT_TRADABLE
    if (guard.check_all("GOOD_TOKEN", "CN", cache) != TickResult::MARKET_NOT_TRADABLE)
        return false;

    // Closed market → MARKET_NOT_TRADABLE
    if (guard.check_all("CLOSED_TOKEN", "US", cache) != TickResult::MARKET_NOT_TRADABLE)
        return false;

    return true;
}

// ─── Test 7: enable_dynamic_fees + resolution_time fields ──────────────

static bool test_metadata_fields() {
    MarketMetadata meta;
    meta.token_id = "TEST_TOKEN";
    meta.question = "Will X happen?";
    meta.outcome_token = "Yes";
    meta.tick_size = 50;
    meta.status = "active";
    meta.enable_dynamic_fees = true;
    meta.resolution_time = std::chrono::system_clock::time_point::max();

    MarketMetadataCache cache;
    cache.set_metadata(meta);

    const MarketMetadata* retrieved = cache.get("TEST_TOKEN");
    if (retrieved == nullptr) return false;
    if (retrieved->enable_dynamic_fees != true) return false;
    if (retrieved->tick_size != 50) return false;
    if (retrieved->status != "active") return false;

    // dynamic_fees flag retrieval (hot path O(1))
    if (cache.get_dynamic_fees("TEST_TOKEN") != true) return false;

    // Test a market with dynamic_fees disabled
    MarketMetadata meta2;
    meta2.token_id = "TEST_TOKEN_2";
    meta2.tick_size = 100;
    meta2.status = "active";
    meta2.enable_dynamic_fees = false;
    meta2.resolution_time = std::chrono::system_clock::time_point::max();
    cache.set_metadata(meta2);

    return (cache.get_dynamic_fees("TEST_TOKEN_2") == false);
}

// ─── Test 8: Resolving-soon market is blocked ──────────────────────────

static bool test_market_resolving_soon() {
    ComplianceConfig cfg = make_default_config();
    ComplianceGuard guard(cfg);

    MarketMetadataCache cache;
    // Market resolving in 1 hour, with 24h warning window → is_resolving_soon = true
    cache.set_metadata(make_metadata("RESOLVING_TOKEN", "active", 100, false, true));

    // Market resolving soon should NOT be tradable
    if (guard.verify_market_active("RESOLVING_TOKEN", cache)) return false;

    // After reducing the warning window to 0 hours, it should be tradable
    cfg.resolution_warning_hours = 0;
    ComplianceGuard guard0(cfg);
    if (!guard0.verify_market_active("RESOLVING_TOKEN", cache)) return false;

    return true;
}

// ─── Test 9: Jurisdiction disabled → operator opt-in ────────────────────

static bool test_jurisdiction_disabled() {
    // When operator explicitly disables jurisdiction check, all are allowed.
    // Default is ENABLED (fail-closed). Only operator can disable.
    ComplianceConfig cfg;
    cfg.jurisdiction_check_enabled = false;
    cfg.allowed_jurisdictions.clear();
    cfg.resolution_warning_hours = 24;

    ComplianceGuard guard(cfg);

    if (!guard.verify_jurisdiction("US")) return false;
    if (!guard.verify_jurisdiction("GB")) return false;
    if (!guard.verify_jurisdiction("CN")) return false;

    return true;
}

// ─── Main ──────────────────────────────────────────────────────────────

int main() {
    std::cout << "🛡️  ComplianceGuard & MarketMetadataCache — Test Suite" << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << std::endl;

    run_test("T1: token_blocklist_rejected",       test_token_blocklist);
    run_test("T2: market_closed_blocks_trading",   test_market_closed);
    run_test("T3: tick_size_snap",                 test_tick_size);
    run_test("T4: jurisdiction_not_configured",    test_jurisdiction_not_configured);
    run_test("T5: jurisdiction_allowlist",         test_jurisdiction_allowlist);
    run_test("T6: check_all_integration",          test_check_all_integration);
    run_test("T7: metadata_dynamic_fees_fields",   test_metadata_fields);
    run_test("T8: market_resolving_soon",          test_market_resolving_soon);
    run_test("T9: jurisdiction_disabled",          test_jurisdiction_disabled);

    std::cout << std::endl;
    std::cout << "══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Results: " << g_tests_passed << "/" << g_tests_run << " passed";
    if (g_tests_passed == g_tests_run) {
        std::cout << " ✅" << std::endl;
    } else {
        std::cout << " ❌" << std::endl;
    }
    std::cout << std::endl;

    if (g_tests_passed != g_tests_run) {
        std::cout << "❌ " << (g_tests_run - g_tests_passed) << " test(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "✅ All compliance guard tests PASS" << std::endl;
    return 0;
}
