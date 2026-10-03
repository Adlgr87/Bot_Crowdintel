#ifndef PREFLIGHT_HPP
#define PREFLIGHT_HPP

// Phase 6 — account preflight: credentials, balance and allowances.
//
// Why this exists: Phase 5 proves *order* state; this proves the *account* can
// actually trade. The CLOB answers "not enough balance / allowance" with HTTP
// 400 at order time (docs.polymarket.com/resources/error-codes), and by then
// the intent is already journaled. The preflight asks the venue before arming.
//
// Endpoints used, all with the L2 headers that Phase 5 already signs
// (`ts + "GET" + path`, query excluded — verified):
//   GET /time                                  public clock sample
//   GET /auth/ban-status/closed-only           closed-only accounts cannot open
//   GET /balance-allowance?asset_type=…        balance + spender→amount map
//   GET /balance-allowance/update?asset_type=… optional cache refresh
//
// Rules (fail closed):
//   * no collateral balance proof, or a balance below the configured maximum
//     order size, blocks arming;
//   * the spender the order will name — the exchange selected by the resolved
//     negative-risk flag — must appear in the allowance map with enough (or
//     unlimited) amount, otherwise arming is blocked;
//   * the conditional-token (ERC-1155 style, all-or-nothing operator) approval
//     is required as well, because a fill credits inventory and the engine may
//     then submit SELL orders; its balance is reported and only enforced when
//     the operator declared an initial position;
//   * a malformed or ambiguous payload (`allowances` missing, duplicated
//     spender, unparsable amount, more spenders than the bounded table) is a
//     failure, never a default;
//   * credentials are proven by an authenticated 200: a 401/403 is reported as
//     such. Ownership/provenance of the API key is Phase 5's job (`owner`
//     field on every order), not duplicated here;
//   * the venue clock is sampled and compared against the same configured
//     tolerance the metadata resolver uses (MarketConfig::max_clock_offset_ms),
//     so there is exactly one clock policy in the build.
//
// The refresh call is opt-in and never implicit: it writes to the venue's
// cache and exists so an operator who just approved contracts can sync without
// waiting. The trading bot never calls it on its own.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "account_state.hpp"
#include "clob_order_info.hpp"
#include "market_config.hpp"
#include "market_metadata.hpp"

namespace preflight {

enum class Verdict : uint8_t {
    kReady = 0,
    kBlocked = 1,
    kTransportError = 2,
};

inline const char* verdict_name(Verdict verdict) noexcept {
    switch (verdict) {
        case Verdict::kReady: return "READY";
        case Verdict::kBlocked: return "BLOCKED";
        case Verdict::kTransportError: return "TRANSPORT_ERROR";
    }
    return "INVALID";
}

enum class Reason : uint8_t {
    kReady = 0,
    kConfigIncomplete,
    kClockUnsynced,
    kCredentialsRejected,
    kClosedOnly,
    kCollateralBalance,
    kCollateralAllowance,
    kConditionalBalance,
    kConditionalAllowance,
    kMalformedResponse,
    kRefreshFailed,
    kTransport,
};

inline const char* reason_name(Reason reason) noexcept {
    switch (reason) {
        case Reason::kReady: return "ready";
        case Reason::kConfigIncomplete: return "preflight_configuration";
        case Reason::kClockUnsynced: return "clock_offset";
        case Reason::kCredentialsRejected: return "credentials_rejected";
        case Reason::kClosedOnly: return "closed_only_account";
        case Reason::kCollateralBalance: return "collateral_balance";
        case Reason::kCollateralAllowance: return "collateral_allowance";
        case Reason::kConditionalBalance: return "conditional_balance";
        case Reason::kConditionalAllowance: return "conditional_allowance";
        case Reason::kMalformedResponse: return "malformed_account_response";
        case Reason::kRefreshFailed: return "balance_refresh";
        case Reason::kTransport: return "transport";
    }
    return "invalid";
}

struct Report {
    uint64_t requests = 0;
    uint64_t clock_checks = 0;
    uint64_t closed_only_checks = 0;
    uint64_t balance_queries = 0;
    uint64_t refreshes = 0;
    uint64_t malformed_payloads = 0;
    uint64_t auth_failures = 0;
    uint64_t unlimited_sentinels = 0;
    uint64_t declared_spenders = 0;
    int64_t clock_offset_ms = 0;
    bool closed_only = false;
    bool conditional_checked = false;
    bool conditional_approval_present = false;
    uint64_t collateral_required_f6 = 0;
    uint64_t collateral_balance_f6 = 0;
    uint64_t collateral_allowance_f6 = 0;
    bool collateral_allowance_unlimited = false;
    uint64_t conditional_required_f6 = 0;
    uint64_t conditional_balance_f6 = 0;
    uint64_t conditional_allowance_f6 = 0;
    bool conditional_allowance_unlimited = false;
};

struct Requirements {
    // Spender the signed order will name: the exchange chosen by the resolved
    // negative-risk flag. Empty means "no proof of who may spend": blocked.
    char exchange_hex[account::kAddressChars]{};
    uint64_t collateral_required_f6 = 0;
    // Minimum outcome-token balance. 0 means "no declared inventory": the
    // balance is reported but not enforced, while the operator approval still
    // is, because a fill can credit inventory at any time.
    uint64_t conditional_required_f6 = 0;
    int signature_type = 0;
    bool refresh_allowances = false;
};

// Pure policy: turns one parsed account state into a reason, given the
// requirement. Kept separate from I/O so it is unit-testable without network.
inline Reason check_collateral(const account::State& state,
                               const Requirements& requirements,
                               uint64_t& allowance_out, bool& unlimited_out,
                               bool& found_out) noexcept {
    allowance_out = 0;
    unlimited_out = false;
    found_out = false;
    if (state.balance_f6 < requirements.collateral_required_f6)
        return Reason::kCollateralBalance;
    uint64_t amount = 0;
    bool unlimited = false;
    if (!account::find_allowance(state, requirements.exchange_hex, amount,
                                 unlimited))
        return Reason::kCollateralAllowance;
    found_out = true;
    allowance_out = amount;
    unlimited_out = unlimited;
    if (!unlimited && amount < requirements.collateral_required_f6)
        return Reason::kCollateralAllowance;
    return Reason::kReady;
}

inline Reason check_conditional(const account::State& state,
                                const Requirements& requirements,
                                uint64_t& allowance_out, bool& unlimited_out,
                                bool& found_out) noexcept {
    allowance_out = 0;
    unlimited_out = false;
    found_out = false;
    if (state.balance_f6 < requirements.conditional_required_f6)
        return Reason::kConditionalBalance;
    uint64_t amount = 0;
    bool unlimited = false;
    if (!account::find_allowance(state, requirements.exchange_hex, amount,
                                 unlimited))
        return Reason::kConditionalAllowance;
    found_out = true;
    allowance_out = amount;
    unlimited_out = unlimited;
    // ERC-1155 approvals are all-or-nothing, so any non-zero value is the full
    // authority; a zero value is not an approval.
    if (!unlimited && amount == 0) return Reason::kConditionalAllowance;
    return Reason::kReady;
}

// The transport contract is the same one Phase 5 uses (recon::RestTransport in
// network builds, a scripted transport in tests):
//   bool usable(); const char* last_error();
//   bool get(path, query, long& http_code, char* out, size_t out_cap,
//            size_t& out_len);
template <typename Transport>
class Preflight {
public:
    Preflight(Transport& transport, const MarketConfig& cfg,
              const Requirements& requirements) noexcept
        : transport_(transport), cfg_(cfg), requirements_(requirements) {}

    bool usable() const noexcept {
        return transport_.usable() &&
               cfg_.owner_api_key[0] != '\0' &&
               cfg_.runtime.token_id_dec[0] != '\0' &&
               requirements_.exchange_hex[0] != '\0';
    }

    // Contract: `error` always holds either "" or the blocking reason.
    Verdict run(char* error, size_t error_cap) noexcept {
        report_ = Report{};
        last_reason_ = Reason::kReady;
        transport_failed_ = false;
        report_.collateral_required_f6 = requirements_.collateral_required_f6;
        report_.conditional_required_f6 = requirements_.conditional_required_f6;
        if (error && error_cap) error[0] = '\0';
        if (!usable()) {
            last_reason_ = Reason::kConfigIncomplete;
            set_error(error, error_cap,
                      "preflight is not configured (transport, credentials, "
                      "resolved token id and exchange are all required)");
            return Verdict::kBlocked;
        }

        const Reason clock = check_clock(error, error_cap);
        if (clock != Reason::kReady) return finish(clock, error, error_cap);

        if (requirements_.refresh_allowances) {
            const Reason refreshed = refresh(error, error_cap);
            if (refreshed != Reason::kReady)
                return finish(refreshed, error, error_cap);
        }

        const Reason ban = check_closed_only(error, error_cap);
        if (ban != Reason::kReady) return finish(ban, error, error_cap);

        const Reason collateral = check_asset("COLLATERAL", nullptr, error,
                                              error_cap);
        if (collateral != Reason::kReady)
            return finish(collateral, error, error_cap);

        const Reason conditional =
            check_asset("CONDITIONAL", cfg_.runtime.token_id_dec, error,
                        error_cap);
        if (conditional != Reason::kReady)
            return finish(conditional, error, error_cap);

        return finish(Reason::kReady, error, error_cap);
    }

    const Report& report() const noexcept { return report_; }
    Reason reason() const noexcept { return last_reason_; }

private:
    Verdict finish(Reason reason, char* error, size_t error_cap) noexcept {
        last_reason_ = reason;
        if (reason == Reason::kReady) return Verdict::kReady;
        if (error && error[0] == '\0')
            set_error(error, error_cap, reason_name(reason));
        // "Could not ask the venue" is a different failure from "the venue
        // answered no": the first is unproven, the second is proven bad.
        return transport_failed_ ? Verdict::kTransportError : Verdict::kBlocked;
    }

    static void set_error(char* error, size_t cap, const char* text) noexcept {
        if (error && cap) std::snprintf(error, cap, "%s", text);
    }

    // `signature_type` participates in address derivation, so it is always on
    // the query; the *signed* path stays the bare path (Phase 5 evidence).
    void append_signature_type(char* query, size_t cap) noexcept {
        char value[8];
        std::snprintf(value, sizeof(value), "%d", requirements_.signature_type);
        append_q(query, cap, "signature_type", value);
    }

    static void append_q(char* out, size_t cap, const char* key,
                         const char* value) noexcept {
        // Percent-encoding is unnecessary here: keys and values are fixed
        // alphanumerics built above, never venue-controlled text.
        const size_t used = std::strlen(out);
        if (used + 1 >= cap) return;
        std::snprintf(out + used, cap - used, "%s%s=%s", used ? "&" : "", key,
                      value);
    }

    Reason check_clock(char* error, size_t error_cap) noexcept {
        char body[256];
        long http_code = 0;
        size_t length = 0;
        if (!transport_.get("/time", nullptr, http_code, body, sizeof(body),
                            length))
            return transport_failure(error, error_cap);
        ++report_.requests;
        if (http_code != 200) {
            char message[128];
            std::snprintf(message, sizeof(message),
                          "GET /time returned HTTP %ld", http_code);
            set_error(error, error_cap, message);
            return Reason::kClockUnsynced;
        }
        int64_t server_seconds = 0;
        if (!crowdintel::parse_server_time(body, length, server_seconds)) {
            ++report_.malformed_payloads;
            set_error(error, error_cap, "/time payload is malformed");
            return Reason::kMalformedResponse;
        }
        ++report_.clock_checks;
        report_.clock_offset_ms =
            (server_seconds - static_cast<int64_t>(::time(nullptr))) * 1000;
        const int64_t tolerance = cfg_.max_clock_offset_ms;
        if (tolerance <= 0 || report_.clock_offset_ms > tolerance ||
            report_.clock_offset_ms < -tolerance) {
            set_error(error, error_cap,
                      "venue clock is outside the configured tolerance");
            return Reason::kClockUnsynced;
        }
        return Reason::kReady;
    }

    Reason refresh(char* error, size_t error_cap) noexcept {
        char query[128] = "asset_type=COLLATERAL";
        append_signature_type(query, sizeof(query));
        char body[256];
        long http_code = 0;
        size_t length = 0;
        if (!transport_.get("/balance-allowance/update", query, http_code, body,
                            sizeof(body), length))
            return transport_failure(error, error_cap);
        ++report_.requests;
        ++report_.refreshes;
        if (http_code != 200) {
            char message[128];
            std::snprintf(message, sizeof(message),
                          "GET /balance-allowance/update returned HTTP %ld",
                          http_code);
            set_error(error, error_cap, message);
            return http_code == 401 || http_code == 403
                       ? rejected(error, error_cap)
                       : Reason::kRefreshFailed;
        }
        return Reason::kReady;
    }

    Reason check_closed_only(char* error, size_t error_cap) noexcept {
        char body[256];
        long http_code = 0;
        size_t length = 0;
        if (!transport_.get("/auth/ban-status/closed-only", nullptr, http_code,
                            body, sizeof(body), length))
            return transport_failure(error, error_cap);
        ++report_.requests;
        ++report_.closed_only_checks;
        if (http_code == 401 || http_code == 403)
            return rejected(error, error_cap);
        if (http_code != 200) {
            char message[128];
            std::snprintf(message, sizeof(message),
                          "GET /auth/ban-status/closed-only returned HTTP %ld",
                          http_code);
            set_error(error, error_cap, message);
            return Reason::kCredentialsRejected;
        }
        bool closed_only = false;
        if (!clob_info::parse_closed_only(body, length, closed_only)) {
            ++report_.malformed_payloads;
            set_error(error, error_cap, "closed-only payload is malformed");
            return Reason::kMalformedResponse;
        }
        report_.closed_only = closed_only;
        if (closed_only) {
            set_error(error, error_cap,
                      "account is closed-only: opening orders is not allowed");
            return Reason::kClosedOnly;
        }
        return Reason::kReady;
    }

    Reason check_asset(const char* asset_type, const char* token_id,
                       char* error, size_t error_cap) noexcept {
        char query[256];
        std::snprintf(query, sizeof(query), "asset_type=%s", asset_type);
        if (token_id && *token_id) {
            const size_t used = std::strlen(query);
            std::snprintf(query + used, sizeof(query) - used, "&token_id=%s",
                          token_id);
        }
        append_signature_type(query, sizeof(query));

        char body[2048];
        long http_code = 0;
        size_t length = 0;
        if (!transport_.get("/balance-allowance", query, http_code, body,
                            sizeof(body), length))
            return transport_failure(error, error_cap);
        ++report_.requests;
        ++report_.balance_queries;
        if (http_code == 401 || http_code == 403)
            return rejected(error, error_cap);
        if (http_code != 200) {
            char message[160];
            std::snprintf(message, sizeof(message),
                          "GET /balance-allowance (%s) returned HTTP %ld",
                          asset_type, http_code);
            set_error(error, error_cap, message);
            return Reason::kCredentialsRejected;
        }

        account::State state;
        if (!account::parse_balance_allowance(body, length, state)) {
            ++report_.malformed_payloads;
            char message[160];
            std::snprintf(message, sizeof(message),
                          "GET /balance-allowance (%s) payload is malformed",
                          asset_type);
            set_error(error, error_cap, message);
            return Reason::kMalformedResponse;
        }
        report_.declared_spenders += state.declared_spenders;
        if (state.unlimited_sentinel) ++report_.unlimited_sentinels;

        const bool collateral = std::strcmp(asset_type, "COLLATERAL") == 0;
        uint64_t allowance = 0;
        bool unlimited = false;
        bool found = false;
        const Reason reason =
            collateral ? check_collateral(state, requirements_, allowance,
                                          unlimited, found)
                       : check_conditional(state, requirements_, allowance,
                                           unlimited, found);
        if (collateral) {
            report_.collateral_balance_f6 = state.balance_f6;
            report_.collateral_allowance_f6 = allowance;
            report_.collateral_allowance_unlimited = unlimited;
        } else {
            report_.conditional_checked = true;
            report_.conditional_balance_f6 = state.balance_f6;
            report_.conditional_allowance_f6 = allowance;
            report_.conditional_allowance_unlimited = unlimited;
            report_.conditional_approval_present = found;
        }
        if (reason == Reason::kCollateralBalance) {
            char message[192];
            std::snprintf(message, sizeof(message),
                          "collateral balance %.6f is below the required %.6f",
                          static_cast<double>(state.balance_f6) * 1e-6,
                          static_cast<double>(requirements_.collateral_required_f6) *
                              1e-6);
            set_error(error, error_cap, message);
            return reason;
        }
        if (reason == Reason::kCollateralAllowance) {
            char message[220];
            std::snprintf(message, sizeof(message),
                          "collateral allowance for %s is %s (required %.6f)",
                          requirements_.exchange_hex,
                          found ? "insufficient" : "absent",
                          static_cast<double>(requirements_.collateral_required_f6) *
                              1e-6);
            set_error(error, error_cap, message);
            return reason;
        }
        if (reason == Reason::kConditionalBalance) {
            char message[192];
            std::snprintf(message, sizeof(message),
                          "outcome-token balance %.6f is below the declared "
                          "inventory %.6f",
                          static_cast<double>(state.balance_f6) * 1e-6,
                          static_cast<double>(requirements_.conditional_required_f6) *
                              1e-6);
            set_error(error, error_cap, message);
            return reason;
        }
        if (reason == Reason::kConditionalAllowance) {
            char message[192];
            std::snprintf(message, sizeof(message),
                          "outcome-token operator approval for %s is %s",
                          requirements_.exchange_hex,
                          found ? "zero" : "absent");
            set_error(error, error_cap, message);
            return reason;
        }
        return Reason::kReady;
    }

    Reason rejected(char* error, size_t error_cap) noexcept {
        ++report_.auth_failures;
        set_error(error, error_cap,
                  "the venue rejected the API credentials (HTTP 401/403)");
        return Reason::kCredentialsRejected;
    }

    Reason transport_failure(char* error, size_t error_cap) noexcept {
        transport_failed_ = true;
        char message[192];
        std::snprintf(message, sizeof(message), "transport: %s",
                      transport_.last_error());
        set_error(error, error_cap, message);
        return Reason::kTransport;
    }

    Transport& transport_;
    const MarketConfig& cfg_;
    Requirements requirements_{};
    Report report_{};
    Reason last_reason_ = Reason::kReady;
    bool transport_failed_ = false;
};

}  // namespace preflight

#endif  // PREFLIGHT_HPP
