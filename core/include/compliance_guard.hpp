// ─────────────────────────────────────────────────────────────────────────────
// compliance_guard.hpp — Jurisdiction & restricted-market gate (fail-closed)
//
// Implements the ComplianceGuard step documented in README.md and
// ARCHITECTURE_MAP.md as "step 1 of ExecutionEngine::run_tick()".  The header
// was missing from the source tree despite being referenced in docs; this file
// provides the actual implementation.
//
// Fail-closed semantics: every check() returns true ONLY when ALL checks
// pass.  Any configuration error, missing data, or denied jurisdiction
// results in an order rejection.  Cost: ~20-40ns (hashset lookup, inlined).
//
// VER-08 | fix/ver-08-compliance-guard
// ─────────────────────────────────────────────────────────────────────────────
#ifndef COMPLIANCE_GUARD_HPP
#define COMPLIANCE_GUARD_HPP

#include <cstdint>
#include <unordered_set>
#include <string>
#include <string_view>
#include <cstring>

// ── Configuration structs (populated from MarketConfig at init) ────────────

struct JurisdictionConfig {
    bool enabled = false;                         // env: BOT_JURISDICTION_ENABLED
    std::unordered_set<std::string> allowed;      // e.g. {"US", "CA", "GB"}
};

struct RestrictedMarketsConfig {
    std::unordered_set<std::string> condition_ids;    // Polymarket market condition IDs (0x + 64 hex chars)
    std::unordered_set<uint64_t> market_hashes;       // alpha_hash_bytes(market_slug) values
};

// ── ComplianceGuard ──────────────────────────────────────────────────────────

class ComplianceGuard {
public:
    struct Result {
        bool allowed;        // true = may proceed, false = reject (fail-closed)
        const char* reason;  // human-readable reason when !allowed
    };

    explicit ComplianceGuard(const JurisdictionConfig& jur,
                             const RestrictedMarketsConfig& rm) noexcept
        : jur_(jur), restricted_(rm) {}

    // Default constructor — fail-closed: jurisdiction check enabled with an
    // EMPTY allowlist, so every order is rejected unless caller provides an
    // explicit JurisdictionConfig with a valid allowlist.
    // (NEMESIS-CRIT: the original = default was fail-open because jur_.enabled
    //  defaulted to false, silently skipping the jurisdiction check.)
    ComplianceGuard() noexcept
        : jur_({true, {}}), restricted_({}) {}

    // Hot-path check: ~20-40ns when inlined.
    //
    // Checks (all must pass):
    //   1. Jurisdiction must be allowed (if jurisdiction check is enabled)
    //   2. Market condition_id must not be in restricted list
    //   3. Market hash must not be in restricted list
    //
    // Parameters:
    //   market_condition_id — 0x-prefixed hex string (may be empty)
    //   market_hash         — uint64_t hash of market slug
    //   operator_jurisdiction — ISO 3166-1 alpha-2 (may be null)
    Result check(const char* market_condition_id,
                 uint64_t market_hash,
                 const char* operator_jurisdiction = nullptr) const noexcept
    {
        // ── 1. Jurisdiction check ─────────────────────────────────────────────
        if (jur_.enabled) {
            if (!operator_jurisdiction || !*operator_jurisdiction) {
                return {false, "compliance: jurisdiction not provided"};
            }
            if (jur_.allowed.find(operator_jurisdiction) == jur_.allowed.end()) {
                return {false, "compliance: jurisdiction restricted"};
            }
        }

        // ── 2. Restricted condition_id check ──────────────────────────────────
        if (market_condition_id && *market_condition_id) {
            if (restricted_.condition_ids.find(market_condition_id) !=
                restricted_.condition_ids.end()) {
                return {false, "compliance: restricted market (condition_id)"};
            }
        }

        // ── 3. Restricted market_hash check ────────────────────────────────────
        if (restricted_.market_hashes.find(market_hash) !=
            restricted_.market_hashes.end()) {
            return {false, "compliance: restricted market (hash)"};
        }

        return {true, nullptr};
    }

    // Cold-path helper: load restricted markets from a comma-separated list.
    static RestrictedMarketsConfig parse_restricted(const char* csv) noexcept {
        RestrictedMarketsConfig rm;
        if (!csv || !*csv) return rm;
        std::string_view s(csv);
        size_t start = 0;
        while (start < s.size()) {
            size_t comma = s.find(',', start);
            std::string_view token = (comma == std::string_view::npos)
                ? s.substr(start) : s.substr(start, comma - start);
            if (token.size() > 0) {
                // Heuristic: tokens starting with "0x" are condition IDs
                if (token.size() >= 2 && token[0] == '0' &&
                    (token[1] == 'x' || token[1] == 'X')) {
                    // Validate: 0x + exactly 64 lowercase hex chars
                    bool valid = (token.size() == 66);
                    for (size_t i = 2; i < token.size() && valid; ++i) {
                        char c = token[i];
                        valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                    }
                    if (valid) {
                        rm.condition_ids.emplace(std::string(token));
                    }
                } else {
                    // Parse as base-10 numeric hash (NOT base-0: "010" → 10, not 8)
                    try {
                        size_t pos;
                        uint64_t h = std::stoull(std::string(token), &pos, 10);
                        if (pos == token.size()) {
                            rm.market_hashes.insert(h);
                        }
                    } catch (...) {
                        // Not a valid hash — skip
                    }
                }
            }
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        return rm;
    }

private:
    JurisdictionConfig jur_;
    RestrictedMarketsConfig restricted_;
};

#endif // COMPLIANCE_GUARD_HPP
