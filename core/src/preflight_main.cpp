// ═══════════════════════════════════════════════════════════════════════════
// crowdintel-preflight — the live gate.
//
// Runs every check in preflight::Runner, prints one line per check, writes the
// pass token when (and only when) READY is true, and exits 1 on any FAIL.
//
// Usage:
//   crowdintel-preflight [--json] [--no-token] [--only <name-prefix>]
//
// Exit codes: 0 = READY, 1 = BLOCKED (any FAIL), 2 = configuration error.
//
// This binary never places, cancels or approves anything.  The only mutating
// call it can make is the opt-in heartbeat probe (BOT_PREFLIGHT_CHECK_HEARTBEAT=1),
// which is skipped whenever open orders exist because starting that contract
// would cancel them when the process exits.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "../crypto/eip712_signer.hpp"
#include "../include/event_ledger.hpp"
#include "clob_rest_client.hpp"
#include "market_config.hpp"
#include "preflight.hpp"
#include "rpc_client.hpp"

#ifdef CROWDINTEL_HAVE_NETWORK
#include "curl_transport.hpp"
#else
namespace {
// Offline builds have no transport.  Every network-dependent check then reports
// a hard failure in live mode (the configuration already refuses live without a
// network build) and a skip otherwise.
class NullTransport : public clob::HttpTransport {
public:
    void request(const char*, const char*, const char*, size_t, const char* const*,
                 size_t, const clob::RequestOptions&, char*, size_t,
                 clob::HttpResponse& out) override {
        out = clob::HttpResponse{};
        out.transport_ok = false;
        out.ambiguous = false;
        std::snprintf(out.error, sizeof(out.error),
                      "network transport is not compiled into this build");
    }
};
}  // namespace
#endif

namespace {

void print_json(const preflight::Report& report) {
    std::printf("{\"ready\":%s,\"failures\":%zu,\"passes\":%zu,\"skips\":%zu,"
                "\"config_fingerprint\":\"%s\",\"chain_id\":%llu,"
                "\"condition_id\":\"%s\",\"token_id\":\"%s\","
                "\"server_time_s\":%llu,\"clock_offset_s\":%lld,"
                "\"collateral_balance\":%llu,\"collateral_allowance\":%llu,"
                "\"target_allowance\":%llu,\"open_orders\":%zu,\"checks\":[",
                report.ready ? "true" : "false", report.failures(), report.passes(),
                report.skips(), report.config_fingerprint,
                static_cast<unsigned long long>(report.chain_id),
                report.condition_id, report.token_id,
                static_cast<unsigned long long>(report.server_time_s),
                static_cast<long long>(report.clock_offset_s),
                static_cast<unsigned long long>(report.collateral_balance),
                static_cast<unsigned long long>(report.collateral_allowance),
                static_cast<unsigned long long>(report.target_allowance),
                report.open_orders);
    for (size_t i = 0; i < report.count; ++i) {
        std::printf("%s{\"name\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}",
                    i ? "," : "", report.checks[i].name,
                    preflight::status_name(report.checks[i].status),
                    report.checks[i].detail);
    }
    std::printf("]}\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool json_output = false;
    bool write_token = true;
    const char* only_prefix = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) {
            json_output = true;
        } else if (std::strcmp(argv[i], "--no-token") == 0) {
            write_token = false;
        } else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only_prefix = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: crowdintel-preflight [--json] [--no-token] "
                         "[--only <name-prefix>]\n");
            return 2;
        }
    }

    MarketConfig config;
    // Trading credentials are required in live mode; other modes may preflight
    // the public surface without them.
    const char* mode_env = std::getenv("BOT_MODE");
    const bool live_requested = mode_env && std::strcmp(mode_env, "live") == 0;
    if (const char* error = config.load(live_requested, false)) {
        std::fprintf(stderr, "CONFIG_ERROR: %s\n", error);
        return 2;
    }
    if (only_prefix) {
        std::fprintf(stderr,
                     "NOTE: --only filters reporting, not execution; every check "
                     "still runs.\n");
    }

#ifdef CROWDINTEL_HAVE_NETWORK
    clob::CurlTransport transport;
    transport.set_tls_pin(config.tls_pin);
    if (!transport.usable()) {
        std::fprintf(stderr, "FATAL: curl initialisation failed\n");
        return 2;
    }
    transport.warmup(config.clob_host);
#else
    NullTransport transport;
#endif

    EIP712Signer signer;
    uint8_t private_key[32]{};
    bool signer_ready = false;
    if (parse_hex_bytes(config.private_key_hex, std::strlen(config.private_key_hex),
                        private_key, sizeof(private_key))) {
        signer_ready = signer.init(private_key, config.neg_risk);
    }
    secure_zero(private_key, sizeof(private_key));

    // Bind the wallet identity into the configuration before anything uses it,
    // exactly as the bot does before it signs an order (main_hot_path.cpp).
    // Without this call the identity checks compare against a zeroed signer:
    // `signer_identity` can never pass (so no token is ever written and live
    // trading can never be authorised), `maker_funder` and `api_owner` pass
    // vacuously on zeros, and every balance/allowance/position check queries the
    // zero address instead of the wallet that will own the orders.
    if (signer_ready) {
        if (const char* error = config.finalize_identity(signer.signer_address())) {
            std::fprintf(stderr, "CONFIG_ERROR: %s\n", error);
            return 2;
        }
        // Public addresses, not secrets: the operator must be able to confirm that
        // preflight validated the same wallet the bot will sign with.
        std::printf("wallet=%s\n", config.signer_hex);
        std::printf("maker=%s\n", config.maker_hex);
        std::printf("api_address=%s\n", config.api_address_hex);
    } else if (live_requested) {
        std::fprintf(stderr,
                     "CONFIG_ERROR: a usable BOT_PRIVATE_KEY_HEX(_FILE) is required "
                     "in live mode\n");
        return 2;
    }

    clob::Credentials credentials{};
    // Same fallback as the bot: L2 credentials belong to the signer EOA unless
    // BOT_API_ADDRESS says otherwise.
    std::snprintf(credentials.address, sizeof(credentials.address), "%s",
                  config.api_address_hex[0] ? config.api_address_hex
                                            : config.signer_hex);
    std::snprintf(credentials.api_key, sizeof(credentials.api_key), "%s",
                  config.owner_api_key);
    std::snprintf(credentials.api_secret_b64, sizeof(credentials.api_secret_b64),
                  "%s", config.api_secret_b64);
    std::snprintf(credentials.api_passphrase, sizeof(credentials.api_passphrase),
                  "%s", config.api_passphrase);
    credentials.signature_type = config.signature_type;
    clob::ClobApiClient api(transport, credentials, config.clob_host,
                            config.gamma_host);
    rpc::JsonRpcClient rpc(transport, config.polygon_rpc_url,
                           config.polygon_rpc_backup_url);


    // The ledger carries bounded maps totalling more than a megabyte; it lives
    // on the heap, never on the stack.
    auto ledger_storage = std::make_unique<ledger::EventLedger>();
    ledger::EventLedger& ledger = *ledger_storage;
    ledger::EventLedger::Options ledger_options{};
    std::snprintf(ledger_options.directory, sizeof(ledger_options.directory), "%s",
                  config.ledger_dir);
    ledger_options.fsync_each_append = config.ledger_fsync;
    ledger_options.checkpoint_every = config.ledger_checkpoint_every;
    char ledger_error[192]{};
    const bool ledger_ok = ledger.open(ledger_options, ledger_error,
                                       sizeof(ledger_error));
    if (!ledger_ok)
        std::fprintf(stderr, "LEDGER: %s\n", ledger_error);

    preflight::Inputs inputs{};
    inputs.config = &config;
    inputs.api = &api;
    inputs.rpc = &rpc;
    inputs.signer = signer_ready ? &signer : nullptr;
    inputs.ledger = ledger_ok ? &ledger : nullptr;
    inputs.check_user_ws = config.preflight_check_user_ws;
    inputs.check_heartbeat = config.preflight_check_heartbeat;

    preflight::Runner runner(inputs);
    preflight::Report report;
    const bool ready = runner.run(report);

    if (json_output) {
        print_json(report);
    } else {
        preflight::Runner::print(report);
        if (only_prefix) {
            std::printf("filtered_by=%s\n", only_prefix);
        }
    }

    if (ready && write_token) {
        if (!runner.write_token(report)) {
            std::fprintf(stderr,
                         "FATAL: checks passed but %s could not be written; live "
                         "trading stays disabled\n",
                         config.preflight_token_file);
            return 1;
        }
        if (!json_output)
            std::printf("token=%s\n", config.preflight_token_file);
    }
    if (!ready) {
        std::fprintf(stderr, "READY=false: %zu check(s) failed\n", report.failures());
        return 1;
    }
    return 0;
}
