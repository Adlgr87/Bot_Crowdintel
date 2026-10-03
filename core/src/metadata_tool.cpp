// crowdintel-metadata — read-only market metadata resolution (Phase 1).
//
// Resolves and cross-validates every venue-controlled parameter for the market
// selected by BOT_MARKET_SLUG or BOT_CONDITION_ID (plus optional BOT_OUTCOME)
// using only public REST endpoints. Loads no key material and no API
// credentials; safe to run on any host before provisioning secrets.
//
// Exit codes: 0 = READINESS READY, 1 = BLOCKED (reason printed).
//
// Usage:
//   BOT_MARKET_SLUG=<slug> BOT_OUTCOME=Yes build/bin/crowdintel-metadata
//   BOT_CONDITION_ID=0x… BOT_OUTCOME=No build/bin/crowdintel-metadata --json

#include <cstdio>
#include <cstring>
#include <string>

#include "market_config.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK)
#include "market_resolver.hpp"
#endif

namespace {

void print_json(const MarketConfig& cfg) {
    char exchange[43];
    crowdintel::format_address_hex(cfg.runtime.exchange, exchange);
    std::printf(
        "{\"readiness\":\"READY\""
        ",\"condition_id\":\"%s\""
        ",\"slug\":\"%s\""
        ",\"token_id\":\"%s\""
        ",\"outcome\":\"%s\""
        ",\"tick_size\":\"%.6f\""
        ",\"min_order_size\":\"%.6f\""
        ",\"fee_rate\":%.10f"
        ",\"fee_exponent\":%.4f"
        ",\"fee_taker_only\":%s"
        ",\"fee_source\":\"%s\""
        ",\"neg_risk\":%s"
        ",\"exchange\":\"%s\""
        ",\"accepting_orders\":%s"
        ",\"active\":%s"
        ",\"closed\":%s"
        ",\"enable_order_book\":%s"
        ",\"archived\":%s"
        ",\"restricted\":%s"
        ",\"clock_offset_ms\":%lld"
        ",\"tick_source\":\"%s\""
        ",\"min_size_source\":\"%s\"}\n",
        cfg.runtime.condition_id, cfg.runtime.market_slug,
        cfg.runtime.token_id_dec, cfg.runtime.outcome_label,
        static_cast<double>(cfg.runtime.tick_size) * 1e-6,
        static_cast<double>(cfg.runtime.min_order_size) * 1e-6,
        cfg.runtime.fee_rate, cfg.runtime.fee_exponent,
        cfg.runtime.fee_taker_only ? "true" : "false", cfg.runtime.fee_source,
        cfg.runtime.neg_risk ? "true" : "false", exchange,
        cfg.runtime.accepting_orders ? "true" : "false",
        cfg.runtime.active ? "true" : "false",
        cfg.runtime.closed ? "true" : "false",
        cfg.runtime.enable_order_book ? "true" : "false",
        cfg.runtime.archived ? "true" : "false",
        cfg.runtime.restricted ? "true" : "false",
        static_cast<long long>(cfg.runtime.clock_offset_ms),
        cfg.runtime.tick_source, cfg.runtime.min_size_source);
}

void print_text(const MarketConfig& cfg) {
    char exchange[43];
    crowdintel::format_address_hex(cfg.runtime.exchange, exchange);
    std::printf("condition_id      %s\n", cfg.runtime.condition_id);
    std::printf("slug              %s\n", cfg.runtime.market_slug);
    std::printf("outcome           %s\n", cfg.runtime.outcome_label);
    std::printf("token_id          %s\n", cfg.runtime.token_id_dec);
    std::printf("tick_size         %.6f (source: %s)\n",
                static_cast<double>(cfg.runtime.tick_size) * 1e-6,
                cfg.runtime.tick_source);
    std::printf("min_order_size    %.6f shares (source: %s)\n",
                static_cast<double>(cfg.runtime.min_order_size) * 1e-6,
                cfg.runtime.min_size_source);
    std::printf("taker_fee         %.10f x (p*(1-p))^%.4f%s (source: %s)\n",
                cfg.runtime.fee_rate, cfg.runtime.fee_exponent,
                cfg.runtime.fee_taker_only ? ", takers only" : "",
                cfg.runtime.fee_source);
    std::printf("neg_risk          %s\n", cfg.runtime.neg_risk ? "true" : "false");
    std::printf("exchange          %s\n", exchange);
    std::printf("status            active=%d closed=%d accepting_orders=%d "
                "enable_order_book=%d archived=%d restricted=%d\n",
                cfg.runtime.active ? 1 : 0, cfg.runtime.closed ? 1 : 0,
                cfg.runtime.accepting_orders ? 1 : 0,
                cfg.runtime.enable_order_book ? 1 : 0,
                cfg.runtime.archived ? 1 : 0,
                cfg.runtime.restricted ? 1 : 0);
    std::printf("clock_offset_ms   %lld\n",
                static_cast<long long>(cfg.runtime.clock_offset_ms));
}

}  // namespace

int main(int argc, char** argv) {
    bool json = false;
    bool check_config_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) json = true;
        else if (std::strcmp(argv[i], "--check-config") == 0)
            check_config_only = true;
        else {
            std::fprintf(stderr, "usage: %s [--json] [--check-config]\n", argv[0]);
            return 2;
        }
    }

    MarketConfig cfg;
    // Inspector role: no signer, no credentials, no journal. This tool must
    // stay usable before provisioning and never reads a secret.
    MarketConfig::LoadOptions options;
    options.role = MarketConfig::Role::kInspector;
    if (const char* error = cfg.load(options)) {
        std::fprintf(stderr, "READINESS = BLOCKED reason=invalid_config: %s\n",
                     error);
        return 1;
    }
    if (check_config_only) {
        std::printf("CONFIG OK mode=%s market=%s role=inspector\n",
                    MarketConfig::mode_name(cfg.mode),
                    cfg.market_slug[0] ? cfg.market_slug : cfg.condition_id);
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
#else
    std::fprintf(stderr,
                 "READINESS = BLOCKED reason=offline_build "
                 "(rebuild with -DCROWDINTEL_NETWORK=ON)\n");
    return 1;
#endif

    if (json) print_json(cfg);
    else print_text(cfg);
    return 0;
}
