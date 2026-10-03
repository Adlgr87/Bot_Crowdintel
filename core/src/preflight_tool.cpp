// crowdintel-preflight — read-only account preflight (Phase 6).
//
// Answers one question before arming: can this account actually trade the
// resolved market? It proves the L2 credentials, the venue clock, the
// closed-only flag, the collateral balance and the spender allowances that the
// signed order will name — everything the CLOB would otherwise reject at order
// time with "not enough balance / allowance" (HTTP 400).
//
// It never signs an order and never touches the private key: `--refresh-
// allowances` is the only call that changes venue state (it forces the CLOB to
// re-read the chain), and it is opt-in.
//
// Exit codes: 0 = READINESS READY, 1 = BLOCKED (reason printed), 2 = usage.
//
// Usage (same environment as the bot; BOT_PRIVATE_KEY_HEX is not needed):
//   BOT_MARKET_SLUG=<slug> BOT_OUTCOME=Yes build/bin/crowdintel-preflight
//   … build/bin/crowdintel-preflight --json
//   … build/bin/crowdintel-preflight --refresh-allowances

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "market_config.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK)
#include "market_metadata.hpp"
#include "market_resolver.hpp"
#include "preflight.hpp"
#include "reconciliation.hpp"
#endif

namespace {

#if defined(CROWDINTEL_HAVE_NETWORK)
struct RunResult {
    preflight::Verdict verdict = preflight::Verdict::kBlocked;
    preflight::Reason reason = preflight::Reason::kReady;
    std::string detail;
    preflight::Report report{};
    char exchange[account::kAddressChars]{};
};

void print_json(const RunResult& run, const MarketConfig& cfg) {
    std::printf(
        "{\"readiness\":\"%s\""
        ",\"reason\":\"%s\""
        ",\"detail\":\"%s\""
        ",\"condition_id\":\"%s\""
        ",\"token_id\":\"%s\""
        ",\"neg_risk\":%s"
        ",\"exchange\":\"%s\""
        ",\"signature_type\":%d"
        ",\"clock_offset_ms\":%lld"
        ",\"closed_only\":%s"
        ",\"collateral_required\":%.6f"
        ",\"collateral_balance\":%.6f"
        ",\"collateral_allowance\":%.6f"
        ",\"collateral_allowance_unlimited\":%s"
        ",\"conditional_required\":%.6f"
        ",\"conditional_balance\":%.6f"
        ",\"conditional_allowance\":%.6f"
        ",\"conditional_allowance_unlimited\":%s"
        ",\"conditional_approval_present\":%s"
        ",\"requests\":%llu"
        ",\"refreshes\":%llu"
        ",\"auth_failures\":%llu"
        ",\"malformed_payloads\":%llu"
        ",\"unlimited_sentinels\":%llu"
        ",\"declared_spenders\":%llu}\n",
        preflight::verdict_name(run.verdict),
        preflight::reason_name(run.reason), run.detail.c_str(), cfg.runtime.condition_id, cfg.runtime.token_id_dec,
        cfg.runtime.neg_risk ? "true" : "false", run.exchange,
        static_cast<int>(cfg.signature_type),
        static_cast<long long>(run.report.clock_offset_ms),
        run.report.closed_only ? "true" : "false",
        static_cast<double>(run.report.collateral_required_f6) * 1e-6,
        static_cast<double>(run.report.collateral_balance_f6) * 1e-6,
        static_cast<double>(run.report.collateral_allowance_f6) * 1e-6,
        run.report.collateral_allowance_unlimited ? "true" : "false",
        static_cast<double>(run.report.conditional_required_f6) * 1e-6,
        static_cast<double>(run.report.conditional_balance_f6) * 1e-6,
        static_cast<double>(run.report.conditional_allowance_f6) * 1e-6,
        run.report.conditional_allowance_unlimited ? "true" : "false",
        run.report.conditional_approval_present ? "true" : "false",
        static_cast<unsigned long long>(run.report.requests),
        static_cast<unsigned long long>(run.report.refreshes),
        static_cast<unsigned long long>(run.report.auth_failures),
        static_cast<unsigned long long>(run.report.malformed_payloads),
        static_cast<unsigned long long>(run.report.unlimited_sentinels),
        static_cast<unsigned long long>(run.report.declared_spenders));
}

void print_text(const RunResult& run, const MarketConfig& cfg) {
    std::printf("verdict            %s\n", preflight::verdict_name(run.verdict));
    std::printf("detail             %s\n",
                run.detail.empty() ? "(none)" : run.detail.c_str());
    std::printf("condition_id       %s\n", cfg.runtime.condition_id);
    std::printf("token_id           %s\n", cfg.runtime.token_id_dec);
    std::printf("exchange (spender) %s%s\n", run.exchange,
                cfg.runtime.neg_risk ? " (negative risk)" : "");
    std::printf("signature_type     %u\n",
                static_cast<unsigned>(cfg.signature_type));
    std::printf("clock_offset_ms    %lld\n",
                static_cast<long long>(run.report.clock_offset_ms));
    std::printf("closed_only        %s\n",
                run.report.closed_only ? "true" : "false");
    std::printf("collateral         balance %.6f required %.6f\n",
                static_cast<double>(run.report.collateral_balance_f6) * 1e-6,
                static_cast<double>(run.report.collateral_required_f6) * 1e-6);
    std::printf("collateral spend   allowance %s (required %.6f)\n",
                run.report.collateral_allowance_unlimited
                    ? "unlimited"
                    : std::to_string(static_cast<double>(
                                         run.report.collateral_allowance_f6) *
                                     1e-6)
                          .c_str(),
                static_cast<double>(run.report.collateral_required_f6) * 1e-6);
    std::printf("conditional        balance %.6f declared %.6f\n",
                static_cast<double>(run.report.conditional_balance_f6) * 1e-6,
                static_cast<double>(run.report.conditional_required_f6) * 1e-6);
    std::printf("conditional spend  approval %s\n",
                run.report.conditional_approval_present
                    ? (run.report.conditional_allowance_unlimited
                           ? "unlimited"
                           : "present")
                    : "ABSENT");
    std::printf("requests           %llu (refreshes %llu, auth failures %llu)\n",
                static_cast<unsigned long long>(run.report.requests),
                static_cast<unsigned long long>(run.report.refreshes),
                static_cast<unsigned long long>(run.report.auth_failures));
}

int run_preflight(MarketConfig& cfg, bool refresh, bool json) {
    if (cfg.runtime.token_id_dec[0] == '\0') {
        std::fprintf(stderr,
                     "READINESS = BLOCKED reason=missing_token "
                     "(the resolved market has no outcome token)\n");
        return 1;
    }
    RunResult run;
    crowdintel::format_address_hex(cfg.runtime.exchange, run.exchange);

    const l2auth::Credentials credentials{cfg.owner_api_key, cfg.api_address_hex,
                                          cfg.api_secret_b64, cfg.api_passphrase};
    recon::RestTransport transport(cfg.clob_host, credentials, cfg.tls_pin);
    if (!transport.usable()) {
        std::fprintf(stderr,
                     "READINESS = BLOCKED reason=preflight_configuration "
                     "detail=%s\n",
                     transport.last_error());
        return 1;
    }

    preflight::Requirements requirements;
    std::snprintf(requirements.exchange_hex, sizeof(requirements.exchange_hex),
                  "%s", run.exchange);
    // One maximum-size order must be affordable: BOT_MAX_ORDER_USD is the
    // engine's own cap, so it is the honest requirement to check.
    requirements.collateral_required_f6 = static_cast<uint64_t>(
        std::llround(cfg.max_order_usd * 1000000.0));
    // BOT_INITIAL_POSITION_SHARES is already 6-decimal fixed point.
    requirements.conditional_required_f6 = cfg.initial_position_shares;
    requirements.signature_type = static_cast<int>(cfg.signature_type);
    requirements.refresh_allowances = refresh;

    preflight::Preflight<recon::RestTransport> preflight(transport, cfg,
                                                         requirements);
    char error[256];
    run.verdict = preflight.run(error, sizeof(error));
    run.reason = preflight.reason();
    run.report = preflight.report();
    run.detail = error;

    if (json) print_json(run, cfg);
    else print_text(run, cfg);
    if (run.verdict == preflight::Verdict::kReady) return 0;
    std::fprintf(stderr, "READINESS = BLOCKED reason=%s detail=%s\n",
                 preflight::reason_name(run.reason), error);
    return 1;
}
#endif  // CROWDINTEL_HAVE_NETWORK

}  // namespace

int main(int argc, char** argv) {
    bool json = false;
    bool refresh = false;
    bool check_config_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) json = true;
        else if (std::strcmp(argv[i], "--refresh-allowances") == 0) refresh = true;
        else if (std::strcmp(argv[i], "--check-config") == 0)
            check_config_only = true;
        else {
            std::fprintf(stderr,
                         "usage: %s [--json] [--refresh-allowances] "
                         "[--check-config]\n", argv[0]);
            return 2;
        }
    }

    MarketConfig cfg;
    // Tool role: L2 credentials yes, signing key and journal no. The tool
    // refuses to start if key material is present at all, and
    // BOT_ENABLE_LIVE_TRADING is still required because it is the same "I
    // intend to arm" flag the bot reads — the tool verifies it instead of
    // trusting it.
    MarketConfig::LoadOptions options;
    options.role = MarketConfig::Role::kTool;
    if (const char* error = cfg.load(options)) {
        std::fprintf(stderr, "READINESS = BLOCKED reason=invalid_config: %s\n",
                     error);
        return 1;
    }
    if (check_config_only) {
        std::printf("CONFIG OK mode=%s market=%s credentials=%s\n",
                    MarketConfig::mode_name(cfg.mode),
                    cfg.market_slug[0] ? cfg.market_slug : cfg.condition_id,
                    cfg.owner_api_key[0] ? "present" : "absent");
        return 0;
    }
    if (cfg.market_slug[0] == '\0' && cfg.condition_id[0] == '\0') {
        std::fprintf(stderr,
                     "READINESS = BLOCKED reason=missing_selector "
                     "(set BOT_MARKET_SLUG or BOT_CONDITION_ID)\n");
        return 1;
    }

#if defined(CROWDINTEL_HAVE_NETWORK)
    MarketMetadataResolver resolver(cfg.clob_host, cfg.gamma_host, cfg.tls_pin);
    char error[192];
    if (!resolver.resolve(cfg, error, sizeof(error))) {
        std::fprintf(stderr, "READINESS = BLOCKED reason=%s\n", error);
        return 1;
    }
    // The clock tolerance is enforced inside resolve() (single policy in the
    // build: MarketConfig::max_clock_offset_ms).
    return run_preflight(cfg, refresh, json);
#else
    (void)refresh;
    (void)json;
    std::fprintf(stderr,
                 "READINESS = BLOCKED reason=offline_build "
                 "(rebuild with -DCROWDINTEL_NETWORK=ON)\n");
    return 1;
#endif
}
