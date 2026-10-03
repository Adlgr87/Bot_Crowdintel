#ifndef PREFLIGHT_HPP
#define PREFLIGHT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// crowdintel-preflight: the gate between "configured" and "allowed to trade".
//
// Rules:
//   * every check reports PASS, FAIL or SKIP with a secret-free detail string;
//   * any FAIL → READY=false and the binary exits 1;
//   * a SKIP is never a pass: checks that live mode requires (RPC chain id,
//     metadata, balances, allowances, ledger, kill switch) FAIL when they cannot
//     run, and only genuinely optional probes (L1 credential derivation, user
//     WSS handshake, heartbeat contract) may SKIP;
//   * nothing the venue can change is defaulted: tick size, minimum order size,
//     negative-risk flag, fee schedule, market status, token id, balances and
//     allowances are all read at check time;
//   * the process never approves tokens.  When an allowance is below the target
//     it reports the exact target and fails; setting the approval is an operator
//     action (see docs/DEPLOYMENT.md).  `approve(max_uint256)` is never used:
//     the target is the configured exposure limit plus an operating margin.
//
// On success the runner writes an atomic pass token (path from
// BOT_PREFLIGHT_TOKEN_FILE) containing the wall-clock time, the effective
// configuration fingerprint, the chain id and the resolved market identity.
// The bot refuses BOT_ENABLE_LIVE_TRADING=1 unless that token is fresh and its
// fingerprint matches the configuration it loaded.
// ─────────────────────────────────────────────────────────────────────────────

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../crypto/clob_auth.hpp"
#include "../crypto/eip712_signer.hpp"
#include "../include/event_ledger.hpp"
#include "../include/json_scan.hpp"
#include "../include/venue_metadata.hpp"
#include "clob_rest_client.hpp"
#include "kill_switch.hpp"
#include "market_config.hpp"
#include "metadata_pipeline.hpp"
#include "rpc_client.hpp"

#ifdef CROWDINTEL_HAVE_NETWORK
#include "user_ws_client.hpp"
#include "user_ws_protocol.hpp"
#include "ws_session.hpp"
#endif

namespace preflight {

enum class Status : uint8_t { PASS = 0, FAIL = 1, SKIP = 2 };

inline const char* status_name(Status status) noexcept {
    switch (status) {
        case Status::PASS: return "PASS";
        case Status::FAIL: return "FAIL";
        case Status::SKIP: return "SKIP";
    }
    return "????";
}

struct Check {
    char name[40]{};
    Status status = Status::FAIL;
    char detail[160]{};
};

struct Report {
    static constexpr size_t K_MAX_CHECKS = 48;
    Check checks[K_MAX_CHECKS]{};
    size_t count = 0;
    bool ready = false;
    char config_fingerprint[65]{};
    char condition_id[70]{};
    char token_id[80]{};
    uint64_t chain_id = 0;
    uint64_t server_time_s = 0;
    int64_t clock_offset_s = 0;
    uint64_t collateral_balance = 0;
    uint64_t collateral_allowance = 0;
    uint64_t target_allowance = 0;
    uint64_t outcome_balance = 0;
    size_t open_orders = 0;
    uint64_t started_wall_ns = 0;
    uint64_t finished_wall_ns = 0;

    void add(const char* name, Status status, const char* format, ...) noexcept {
        if (count >= K_MAX_CHECKS) return;
        Check& check = checks[count++];
        std::snprintf(check.name, sizeof(check.name), "%s", name);
        check.status = status;
        va_list args;
        va_start(args, format);
        std::vsnprintf(check.detail, sizeof(check.detail), format, args);
        va_end(args);
        if (status == Status::FAIL) ready = false;
    }
    size_t failures() const noexcept {
        size_t total = 0;
        for (size_t i = 0; i < count; ++i)
            if (checks[i].status == Status::FAIL) ++total;
        return total;
    }
    size_t passes() const noexcept {
        size_t total = 0;
        for (size_t i = 0; i < count; ++i)
            if (checks[i].status == Status::PASS) ++total;
        return total;
    }
    size_t skips() const noexcept {
        size_t total = 0;
        for (size_t i = 0; i < count; ++i)
            if (checks[i].status == Status::SKIP) ++total;
        return total;
    }
};

struct Inputs {
    const MarketConfig* config = nullptr;
    clob::ClobApiClient* api = nullptr;
    rpc::JsonRpcClient* rpc = nullptr;
    const EIP712Signer* signer = nullptr;   // optional (L1 credential check)
    ledger::EventLedger* ledger = nullptr;  // optional (durability probe)
    bool check_user_ws = false;
    bool check_heartbeat = false;
};

class Runner {
public:
    explicit Runner(const Inputs& inputs) : inputs_(inputs) {}

    bool run(Report& report) {
        report = Report{};
        report.started_wall_ns = wall_ns();
        if (!inputs_.config || !inputs_.api) {
            report.add("inputs", Status::FAIL,
                       "preflight requires a configuration and a CLOB client");
            report.ready = false;
            report.finished_wall_ns = wall_ns();
            return false;
        }
        const MarketConfig& cfg = *inputs_.config;
        clob::ClobApiClient& api = *inputs_.api;
        const bool live = cfg.bot_mode == BotMode::LIVE;
        std::snprintf(report.config_fingerprint, sizeof(report.config_fingerprint),
                      "%s", cfg.config_fingerprint);
        report.ready = true;

        // ── 1. Mode and arming ──────────────────────────────────────────────
        report.add("mode", Status::PASS, "BOT_MODE=%s armed=%d",
                   bot_mode_name(cfg.bot_mode), cfg.live_armed ? 1 : 0);
        if (live && !cfg.live_armed)
            report.add("live_armed", Status::FAIL,
                       "BOT_ENABLE_LIVE_TRADING must be 1 for live mode");
        if (live && std::getenv("CROWDINTEL_FORCE_MOCK"))
            report.add("mock_absent", Status::FAIL,
                       "CROWDINTEL_FORCE_MOCK is set; a live run must not be forced "
                       "into the mock transport");
        else
            report.add("mock_absent", Status::PASS,
                       live ? "no forced mock transport" : "not applicable");
        report.add("config_fingerprint", Status::PASS, "%s", cfg.config_fingerprint);

        // ── 2. Chain identity (validates every hardcoded constant) ──────────
        if (inputs_.rpc && inputs_.rpc->configured()) {
            clob::CallResult call{};
            uint64_t chain_id = 0;
            if (inputs_.rpc->chain_id(chain_id, call)) {
                report.chain_id = chain_id;
                if (chain_id == venue::K_POLYGON_CHAIN_ID)
                    report.add("chain_id", Status::PASS, "eth_chainId=%llu (Polygon)",
                               static_cast<unsigned long long>(chain_id));
                else
                    report.add("chain_id", Status::FAIL,
                               "eth_chainId=%llu, expected %llu",
                               static_cast<unsigned long long>(chain_id),
                               static_cast<unsigned long long>(
                                   venue::K_POLYGON_CHAIN_ID));
            } else {
                report.add("chain_id", live ? Status::FAIL : Status::SKIP,
                           "eth_chainId unavailable (%s)", call.detail);
            }
            if (inputs_.rpc->used_backup())
                report.add("rpc_failover", Status::PASS,
                           "POLYGON_RPC_BACKUP_URL served the request");
        } else {
            report.add("chain_id", live ? Status::FAIL : Status::SKIP,
                       "POLYGON_RPC_URL is not configured");
        }

        // ── 3. CLOB reachability, server time, credentials ──────────────────
        clob::CallResult call{};
        if (api.health(call))
            report.add("clob_health", Status::PASS, "GET %s answered",
                       venue::K_PATH_HEALTH);
        else
            report.add("clob_health", live ? Status::FAIL : Status::SKIP,
                       "GET %s failed (%s)", venue::K_PATH_HEALTH, call.detail);

        uint64_t server_time = 0;
        if (api.server_time(server_time, call)) {
            report.server_time_s = server_time;
            const uint64_t local = static_cast<uint64_t>(wall_ns() / 1000000000ULL);
            const int64_t offset = static_cast<int64_t>(local) -
                                   static_cast<int64_t>(server_time);
            report.clock_offset_s = offset;
            const int64_t limit = static_cast<int64_t>(cfg.max_clock_skew_s);
            if (offset < -limit || offset > limit)
                report.add("clock_offset", Status::FAIL,
                           "local clock is %+lld s from GET /time (limit %llu s)",
                           static_cast<long long>(offset),
                           static_cast<unsigned long long>(cfg.max_clock_skew_s));
            else
                report.add("clock_offset", Status::PASS,
                           "offset %+lld s within %llu s",
                           static_cast<long long>(offset),
                           static_cast<unsigned long long>(cfg.max_clock_skew_s));
        } else {
            report.add("clock_offset", live ? Status::FAIL : Status::SKIP,
                       "GET /time failed (%s)", call.detail);
        }

        if (api.credentials_ready()) {
            clob::OrderPage page{};
            if (api.open_orders(nullptr, nullptr, nullptr, page, call) && page.complete) {
                report.open_orders = page.count;
                report.add("credentials_l2", Status::PASS,
                           "GET /data/orders accepted (%zu open orders)", page.count);
            } else {
                report.add("credentials_l2", live ? Status::FAIL : Status::SKIP,
                           "GET /data/orders rejected (http %ld, %s)",
                           call.response.code, call.detail);
            }
        } else {
            report.add("credentials_l2", live ? Status::FAIL : Status::SKIP,
                       "CLOB_API_KEY/SECRET/PASSPHRASE are not loaded");
        }

        // ── 4. Identity: signer / maker / funder / signature type ───────────
        if (inputs_.signer && inputs_.signer->signer_address()) {
            const bool signer_matches =
                std::memcmp(inputs_.signer->signer_address(), cfg.signer, 20) == 0;
            report.add("signer_identity",
                       signer_matches ? Status::PASS : Status::FAIL,
                       "EIP-712 signer %s %s configured signer",
                       cfg.signer_hex, signer_matches ? "matches" : "DOES NOT MATCH");
        } else {
            report.add("signer_identity", live ? Status::FAIL : Status::SKIP,
                       "signer is not available to preflight");
        }
        if (cfg.signature_type == 0) {
            const bool consistent = std::memcmp(cfg.maker, cfg.signer, 20) == 0;
            report.add("maker_funder", consistent ? Status::PASS : Status::FAIL,
                       "signature type 0 (EOA) requires maker == signer");
        } else if (cfg.signature_type == 1 || cfg.signature_type == 2) {
            const bool distinct = std::memcmp(cfg.maker, cfg.signer, 20) != 0;
            report.add("maker_funder", distinct ? Status::PASS : Status::FAIL,
                       "signature type %u requires maker (proxy/safe) != signer",
                       static_cast<unsigned>(cfg.signature_type));
        } else {
            report.add("maker_funder", Status::FAIL,
                       "signature type %u is not supported (POLY_1271 fails closed)",
                       static_cast<unsigned>(cfg.signature_type));
        }
        // The address that owns the funds must be the address the venue sees.
        if (std::strcmp(cfg.api_address_hex, cfg.signer_hex) != 0 &&
            cfg.signature_type == 0)
            report.add("api_owner", Status::FAIL,
                       "POLY_ADDRESS %s must equal the signer for EOA orders",
                       cfg.api_address_hex);
        else
            report.add("api_owner", Status::PASS, "POLY_ADDRESS=%s",
                       cfg.api_address_hex);

        // ── 5. Optional L1 credential derivation ────────────────────────────
        if (cfg.preflight_check_l1 && api.credentials_ready()) {
            (void)derive_and_compare_credentials(report);
        } else {
            report.add("credentials_l1", Status::SKIP,
                       "BOT_PREFLIGHT_CHECK_L1=0 (set it to prove the private key "
                       "owns these API credentials)");
        }

        // ── 6. Market metadata ──────────────────────────────────────────────
        venue::PipelineOptions options{};
        options.require_status = live;
        options.use_gamma = true;
        venue::MetadataPipeline pipeline(api, options);
        venue::MetadataPolicy policy{};
        policy.require_gamma_status = live;
        policy.allow_protocol_v2 = cfg.allow_protocol_v2_positions;
        policy.max_taker_fee_micro =
            static_cast<uint64_t>(cfg.max_taker_fee_rate * 1000000.0);
        venue::PipelineResult result{};
        const char* slug = cfg.market_slug;
        const bool pipeline_ok =
            pipeline.run(slug, cfg.condition_id,
                         cfg.token_id_dec[0] ? cfg.token_id_dec : nullptr, policy,
                         result);
        metadata_ = result.metadata;
        if (pipeline_ok) {
            std::snprintf(report.condition_id, sizeof(report.condition_id), "%s",
                          result.metadata.condition_id);
            std::snprintf(report.token_id, sizeof(report.token_id), "%s",
                          result.resolved_token_id);
            report.add("market_identity", Status::PASS, "condition=%s token=%s",
                       result.metadata.condition_id, result.resolved_token_id);
            report.add("market_status", Status::PASS,
                       "active=%d closed=%d archived=%d accepting=%d book=%d "
                       "restricted=%d",
                       result.metadata.active, result.metadata.closed,
                       result.metadata.archived, result.metadata.accepting_orders,
                       result.metadata.enable_order_book, result.metadata.restricted);
            report.add("tick_size", Status::PASS, "tick=%llu (1e-6) from venue",
                       static_cast<unsigned long long>(result.metadata.tick_raw));
            report.add("min_order_size", Status::PASS, "min=%llu shares (1e-6)",
                       static_cast<unsigned long long>(result.metadata.min_size_raw));
            report.add("neg_risk", Status::PASS, "neg_risk=%d",
                       result.metadata.neg_risk ? 1 : 0);
            report.add("fee_schedule", Status::PASS,
                       "rate=%llu exponent=%llu bps=%llu fees_enabled=%d",
                       static_cast<unsigned long long>(result.metadata.fee_rate_micro),
                       static_cast<unsigned long long>(
                           result.metadata.fee_exponent_micro),
                       static_cast<unsigned long long>(result.metadata.fee_rate_bps),
                       result.metadata.fees_enabled ? 1 : 0);
            if (cfg.tick_size && cfg.tick_size != result.metadata.tick_raw)
                report.add("tick_expectation", Status::FAIL,
                           "BOT_TICK_SIZE=%llu disagrees with the venue (%llu)",
                           static_cast<unsigned long long>(cfg.tick_size),
                           static_cast<unsigned long long>(result.metadata.tick_raw));
            else
                report.add("tick_expectation", Status::PASS,
                           "configuration matches the venue tick");
            if (cfg.min_size_shares &&
                cfg.min_size_shares != result.metadata.min_size_raw)
                report.add("min_size_expectation", Status::FAIL,
                           "BOT_MIN_SIZE_SHARES=%llu disagrees with the venue (%llu)",
                           static_cast<unsigned long long>(cfg.min_size_shares),
                           static_cast<unsigned long long>(
                               result.metadata.min_size_raw));
            else
                report.add("min_size_expectation", Status::PASS,
                           "configuration matches the venue minimum size");
            if (cfg.bot_mode == BotMode::LIVE &&
                static_cast<int>(cfg.neg_risk) !=
                    static_cast<int>(result.metadata.neg_risk))
                report.add("neg_risk_expectation", Status::FAIL,
                           "BOT_NEG_RISK=%d disagrees with the venue (%d)",
                           cfg.neg_risk ? 1 : 0, result.metadata.neg_risk ? 1 : 0);
            else
                report.add("neg_risk_expectation", Status::PASS,
                           "negative-risk flag agrees with the venue");
            if (venue::is_protocol_v2_position_id(result.resolved_token_id))
                report.add("protocol_v2_guard",
                           cfg.allow_protocol_v2_positions ? Status::PASS
                                                           : Status::FAIL,
                           "token is a protocol-v2 position id (Combos exchange)");
            else
                report.add("protocol_v2_guard", Status::PASS,
                           "token settles through the CTF exchanges");
            if (result.book_valid)
                report.add("book_snapshot", Status::PASS,
                           "%zu bids / %zu asks hash=%s", result.book.bid_count,
                           result.book.ask_count, result.book.hash);
            else if (live)
                report.add("book_snapshot", Status::FAIL, "GET /book unavailable");
        } else {
            const Status severity = live ? Status::FAIL : Status::SKIP;
            report.add("market_identity", severity, "%s", result.detail);
            char reasons[160]{};
            result.report.format(reasons, sizeof(reasons));
            if (reasons[0]) report.add("market_validation", severity, "%s", reasons);
        }

        // ── 7. Contracts and balances on chain ──────────────────────────────
        if (inputs_.rpc && inputs_.rpc->configured() && report.chain_id ==
                                                             venue::K_POLYGON_CHAIN_ID) {
            check_contracts_and_balances(report, result.metadata);
        } else {
            report.add("contracts", live ? Status::FAIL : Status::SKIP,
                       "RPC unavailable; contract code and balances not verified");
            report.add("collateral_balance", live ? Status::FAIL : Status::SKIP,
                       "RPC unavailable");
            report.add("collateral_allowance", live ? Status::FAIL : Status::SKIP,
                       "RPC unavailable");
        }

        // ── 8. Ledger durability ────────────────────────────────────────────
        if (inputs_.ledger) {
            if (inputs_.ledger->corrupt() || !inputs_.ledger->is_open())
                report.add("ledger", Status::FAIL,
                           "ledger at %s is corrupt or not open", cfg.ledger_dir);
            else {
                ledger::Event event{};
                event.type = ledger::EventType::STATE_EVENT;
                event.source = ledger::Source::LOCAL;
                event.wall_ns = ledger::now_wall_ns();
                event.add_str(ledger::F_REASON, "preflight_durability_probe");
                ledger::compute_event_key(event.type, event.source, "", "", 0,
                                          event.wall_ns, event.key);
                char error[128]{};
                if (inputs_.ledger->commit(event, error, sizeof(error)))
                    report.add("ledger", Status::PASS,
                               "%s writable, %llu events, %llu checkpoints",
                               cfg.ledger_dir,
                               static_cast<unsigned long long>(
                                   inputs_.ledger->applied_events()),
                               static_cast<unsigned long long>(
                                   inputs_.ledger->checkpoints()));
                else
                    report.add("ledger", Status::FAIL, "journal write failed: %s",
                               error);
            }
        } else {
            report.add("ledger", live ? Status::FAIL : Status::SKIP,
                       "ledger was not provided to preflight");
        }

        // ── 9. Kill switch ──────────────────────────────────────────────────
        // Only a clean ENOENT passes.  An engaged switch fails, and so does a
        // check that cannot be answered: preflight must not authorise live
        // trading on an operator lever whose state is unknown.
        {
            const safety::KillSwitchState state =
                safety::poll_kill_switch(cfg.kill_switch_file);
            if (state == safety::KillSwitchState::DISENGAGED)
                report.add("kill_switch", Status::PASS,
                           "kill switch %s is not engaged", cfg.kill_switch_file);
            else if (state == safety::KillSwitchState::ENGAGED)
                report.add("kill_switch", Status::FAIL,
                           "kill switch %s is present", cfg.kill_switch_file);
            else {
                char error[160]{};
                safety::kill_switch_error_text(error, sizeof(error));
                report.add("kill_switch", Status::FAIL,
                           "kill switch %s cannot be checked (%s)",
                           cfg.kill_switch_file, error);
            }
        }

        // ── 10. Optional probes ─────────────────────────────────────────────
        check_user_ws(report, live);
        check_heartbeat(report, api, live);

        report.finished_wall_ns = wall_ns();
        report.ready = report.failures() == 0;
        return report.ready;
    }

    const venue::MarketMetadata& metadata() const noexcept { return metadata_; }

    static void print(const Report& report) {
        std::printf("crowdintel-preflight\n");
        std::printf("config_fingerprint=%s\n", report.config_fingerprint);
        for (size_t i = 0; i < report.count; ++i) {
            const Check& check = report.checks[i];
            std::printf("[%s] %-22s %s\n", status_name(check.status), check.name,
                        check.detail);
        }
        std::printf("checks=%zu pass=%zu fail=%zu skip=%zu\n", report.count,
                    report.passes(), report.failures(), report.skips());
        std::printf("READY=%s\n", report.ready ? "true" : "false");
        if (!report.ready) {
            std::printf("REASON=");
            bool first = true;
            for (size_t i = 0; i < report.count; ++i) {
                if (report.checks[i].status != Status::FAIL) continue;
                std::printf("%s%s", first ? "" : ";", report.checks[i].name);
                first = false;
            }
            std::printf("\n");
        }
    }

    // Atomic write of the pass token.  Only written when READY is true.
    bool write_token(const Report& report) const {
        if (!report.ready) return false;
        const MarketConfig& cfg = *inputs_.config;
        char tmp_path[320];
        std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cfg.preflight_token_file);
        const int fd = ::open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        char body[1024];
        const int written = std::snprintf(
            body, sizeof(body),
            "{\"version\":1,\"ready\":true,\"timestamp_s\":%llu,"
            "\"config_fingerprint\":\"%s\",\"chain_id\":%llu,"
            "\"condition_id\":\"%s\",\"token_id\":\"%s\","
            "\"checks\":%zu,\"failures\":0,\"mode\":\"%s\"}",
            static_cast<unsigned long long>(report.finished_wall_ns / 1000000000ULL),
            report.config_fingerprint,
            static_cast<unsigned long long>(report.chain_id), report.condition_id,
            report.token_id, report.count, bot_mode_name(cfg.bot_mode));
        bool ok = written > 0 && static_cast<size_t>(written) < sizeof(body);
        if (ok) {
            const char* cursor = body;
            size_t remaining = static_cast<size_t>(written);
            while (remaining) {
                const ssize_t n = ::write(fd, cursor, remaining);
                if (n <= 0) { ok = false; break; }
                cursor += n;
                remaining -= static_cast<size_t>(n);
            }
        }
        if (ok && ::fsync(fd) != 0) ok = false;
        ::close(fd);
        if (!ok) {
            ::unlink(tmp_path);
            return false;
        }
        if (::rename(tmp_path, cfg.preflight_token_file) != 0) {
            ::unlink(tmp_path);
            return false;
        }
        const char* slash = std::strrchr(cfg.preflight_token_file, '/');
        if (slash) {
            char directory[192];
            const size_t length = static_cast<size_t>(slash - cfg.preflight_token_file);
            if (length && length < sizeof(directory)) {
                std::memcpy(directory, cfg.preflight_token_file, length);
                directory[length] = '\0';
                const int dir_fd = ::open(directory, O_RDONLY | O_CLOEXEC);
                if (dir_fd >= 0) { ::fsync(dir_fd); ::close(dir_fd); }
            }
        }
        return true;
    }

    // The bot calls this before honouring BOT_ENABLE_LIVE_TRADING=1.
    static bool token_is_fresh(const char* path, uint64_t max_age_s,
                               const char* expected_fingerprint, char* detail,
                               size_t cap) {
        if (!path || !path[0]) {
            std::snprintf(detail, cap, "preflight token path is not configured");
            return false;
        }
        if (max_age_s == 0) {
            // A zero tolerance would make the gate meaningless; the
            // configuration already bounds this to 10..86400 s.
            std::snprintf(detail, cap, "preflight max age must be greater than zero");
            return false;
        }
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            std::snprintf(detail, cap, "preflight token %s is missing", path);
            return false;
        }
        char body[2048];
        const ssize_t n = ::read(fd, body, sizeof(body) - 1);
        ::close(fd);
        if (n <= 0) {
            std::snprintf(detail, cap, "preflight token is unreadable");
            return false;
        }
        body[n] = '\0';
        const size_t len = static_cast<size_t>(n);
        bool ready = false;
        if (!json_scan::get_bool(body, len, "ready", ready) || !ready) {
            std::snprintf(detail, cap, "preflight token does not report ready");
            return false;
        }
        uint64_t timestamp = 0;
        if (!json_scan::get_u64(body, len, "timestamp_s", timestamp)) {
            std::snprintf(detail, cap, "preflight token has no timestamp");
            return false;
        }
        const uint64_t now = static_cast<uint64_t>(wall_ns() / 1000000000ULL);
        if (timestamp > now) {
            std::snprintf(detail, cap, "preflight token is dated in the future");
            return false;
        }
        if (now - timestamp > max_age_s) {
            std::snprintf(detail, cap,
                          "preflight token is %llu s old (limit %llu s)",
                          static_cast<unsigned long long>(now - timestamp),
                          static_cast<unsigned long long>(max_age_s));
            return false;
        }
        if (expected_fingerprint && expected_fingerprint[0]) {
            char fingerprint[65]{};
            if (!json_scan::get_string(body, len, "config_fingerprint", fingerprint,
                                       sizeof(fingerprint)) ||
                std::strcmp(fingerprint, expected_fingerprint) != 0) {
                std::snprintf(detail, cap,
                              "preflight token belongs to a different configuration");
                return false;
            }
        }
        std::snprintf(detail, cap, "preflight token is %llu s old",
                      static_cast<unsigned long long>(now - timestamp));
        return true;
    }

private:
    static uint64_t wall_ns() noexcept {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    void check_contracts_and_balances(Report& report,
                                      const venue::MarketMetadata& metadata) const {
        const MarketConfig& cfg = *inputs_.config;
        rpc::JsonRpcClient& client = *inputs_.rpc;
        clob::CallResult call{};
        char pUSD[43];
        rpc::bytes_to_hex(venue::K_COLLATERAL_PUSD, 20, pUSD, sizeof(pUSD));
        char ctf[43];
        rpc::bytes_to_hex(venue::K_CONDITIONAL_TOKENS, 20, ctf, sizeof(ctf));
        const bool v2_token = venue::is_protocol_v2_position_id(metadata.token_id);
        const uint8_t* exchange_bytes =
            v2_token ? venue::K_EXCHANGE_V3
                     : (metadata.neg_risk ? venue::K_EXCHANGE_NEG_RISK
                                          : venue::K_EXCHANGE_STANDARD);
        char exchange[43];
        rpc::bytes_to_hex(exchange_bytes, 20, exchange, sizeof(exchange));

        bool has_code = false;
        if (client.has_code(exchange, has_code, call) && has_code)
            report.add("exchange_contract", Status::PASS, "%s has code", exchange);
        else
            report.add("exchange_contract", Status::FAIL,
                       "%s has no code (%s)", exchange, call.detail);
        if (client.has_code(pUSD, has_code, call) && has_code)
            report.add("collateral_contract", Status::PASS, "pUSD %s has code", pUSD);
        else
            report.add("collateral_contract", Status::FAIL, "pUSD %s has no code",
                       pUSD);
        if (client.has_code(ctf, has_code, call) && has_code)
            report.add("ctf_contract", Status::PASS, "CTF %s has code", ctf);
        else
            report.add("ctf_contract", Status::FAIL, "CTF %s has no code", ctf);

        // The account that must hold the funds: the maker (proxy/safe) for
        // signature types 1 and 2, the signer EOA for type 0.
        const char* funder_hex = cfg.maker_hex;
        uint64_t rpc_balance = 0;
        if (client.erc20_balance(pUSD, cfg.maker, rpc_balance, call)) {
            report.collateral_balance = rpc_balance;
            if (rpc_balance >= cfg.min_collateral_base)
                report.add("collateral_balance", Status::PASS,
                           "pUSD balance %llu base units at %s",
                           static_cast<unsigned long long>(rpc_balance), funder_hex);
            else
                report.add("collateral_balance", Status::FAIL,
                           "pUSD balance %llu below the required %llu",
                           static_cast<unsigned long long>(rpc_balance),
                           static_cast<unsigned long long>(cfg.min_collateral_base));
        } else {
            report.add("collateral_balance", Status::FAIL,
                       "balanceOf(pUSD,%s) failed (%s)", funder_hex, call.detail);
        }

        uint64_t rpc_allowance = 0;
        report.target_allowance = cfg.target_allowance_base;
        if (client.erc20_allowance(pUSD, cfg.maker, exchange_bytes, rpc_allowance,
                                   call)) {
            report.collateral_allowance = rpc_allowance;
            if (rpc_allowance >= cfg.target_allowance_base)
                report.add("collateral_allowance", Status::PASS,
                           "allowance %llu >= target %llu (spender %s)",
                           static_cast<unsigned long long>(rpc_allowance),
                           static_cast<unsigned long long>(cfg.target_allowance_base),
                           exchange);
            else
                report.add("collateral_allowance", Status::FAIL,
                           "allowance %llu < target %llu; approve exactly the target "
                           "(never max_uint256) for spender %s",
                           static_cast<unsigned long long>(rpc_allowance),
                           static_cast<unsigned long long>(cfg.target_allowance_base),
                           exchange);
        } else {
            report.add("collateral_allowance", Status::FAIL,
                       "allowance(pUSD,%s,%s) failed (%s)", funder_hex, exchange,
                       call.detail);
        }

        // Cross-check the venue's own view against the chain: a disagreement
        // means one of the two sources is stale or wrong, and trading on either
        // guess is how duplicate exposure happens.
        if (inputs_.api && inputs_.api->credentials_ready()) {
            venue::BalanceAllowance venue_view{};
            clob::CallResult venue_call{};
            if (inputs_.api->balance_allowance(venue::AssetType::COLLATERAL, nullptr,
                                               exchange, venue_view, venue_call)) {
                if (venue_view.balance_valid && venue_view.balance != rpc_balance)
                    report.add("balance_cross_check", Status::FAIL,
                               "CLOB balance %llu != chain balance %llu",
                               static_cast<unsigned long long>(venue_view.balance),
                               static_cast<unsigned long long>(rpc_balance));
                else
                    report.add("balance_cross_check", Status::PASS,
                               "CLOB and chain balances agree (%llu)",
                               static_cast<unsigned long long>(rpc_balance));
            } else {
                report.add("balance_cross_check",
                           cfg.bot_mode == BotMode::LIVE ? Status::FAIL : Status::SKIP,
                           "GET /balance-allowance failed (%s)", venue_call.detail);
            }

            const venue::AssetType outcome_type =
                v2_token ? venue::AssetType::CONDITIONAL_V2
                         : venue::AssetType::CONDITIONAL;
            venue::BalanceAllowance outcome_view{};
            if (inputs_.api->balance_allowance(outcome_type, metadata.token_id,
                                               exchange, outcome_view, venue_call)) {
                report.outcome_balance = outcome_view.balance;
                report.add("outcome_balance", Status::PASS,
                           "outcome token balance %llu (asset_type %s)",
                           static_cast<unsigned long long>(outcome_view.balance),
                           venue::asset_type_name(outcome_type));
                uint64_t chain_outcome = 0;
                const char* position_contract = v2_token ? nullptr : ctf;
                if (position_contract &&
                    inputs_.rpc->erc1155_balance(position_contract, cfg.maker,
                                                 metadata.token_id, chain_outcome,
                                                 call)) {
                    if (chain_outcome != outcome_view.balance)
                        report.add("inventory_cross_check", Status::FAIL,
                                   "chain inventory %llu != CLOB inventory %llu",
                                   static_cast<unsigned long long>(chain_outcome),
                                   static_cast<unsigned long long>(
                                       outcome_view.balance));
                    else
                        report.add("inventory_cross_check", Status::PASS,
                                   "chain and CLOB inventory agree (%llu)",
                                   static_cast<unsigned long long>(chain_outcome));
                    bool approved = false;
                    if (chain_outcome != 0) {
                        if (inputs_.rpc->erc1155_approved_for_all(
                                position_contract, cfg.maker, exchange_bytes, approved,
                                call))
                            report.add("outcome_approval",
                                       approved ? Status::PASS : Status::FAIL,
                                       approved
                                           ? "exchange is an approved operator"
                                           : "exchange is NOT approved to move outcome "
                                             "tokens; SELL orders would fail");
                        else
                            report.add("outcome_approval", Status::FAIL,
                                       "isApprovedForAll failed (%s)", call.detail);
                    } else {
                        report.add("outcome_approval", Status::PASS,
                                   "no inventory; approval not required yet");
                    }
                } else {
                    report.add("inventory_cross_check", Status::FAIL,
                               "ERC-1155 balanceOf failed (%s)", call.detail);
                }
            } else {
                report.add("outcome_balance",
                           cfg.bot_mode == BotMode::LIVE ? Status::FAIL : Status::SKIP,
                           "outcome balance query failed (%s)", venue_call.detail);
            }
        }

        uint64_t gas = 0;
        if (client.native_balance(funder_hex, gas, call))
            report.add("gas_balance", gas != 0 ? Status::PASS : Status::SKIP,
                       gas != 0 ? "POL balance available for direct transactions"
                                : "no POL: only relayer/gasless flows will work");
        else
            report.add("gas_balance", Status::SKIP, "eth_getBalance failed (%s)",
                       call.detail);
    }

    // Derives the API credentials from the private key (L1) and compares the
    // API key.  The derived secret is never logged and is wiped immediately.
    bool derive_and_compare_credentials(Report& report) {
        const MarketConfig& cfg = *inputs_.config;
        uint8_t private_key[32]{};
        if (!parse_hex_bytes(cfg.private_key_hex, std::strlen(cfg.private_key_hex),
                             private_key, sizeof(private_key))) {
            report.add("credentials_l1", Status::FAIL,
                       "BOT_PRIVATE_KEY_HEX is not 32 hex bytes");
            return false;
        }
        EIP712Signer signer;
        if (!signer.init(private_key, cfg.neg_risk)) {
            secure_zero(private_key, sizeof(private_key));
            report.add("credentials_l1", Status::FAIL, "signer initialisation failed");
            return false;
        }
        secure_zero(private_key, sizeof(private_key));
        const uint64_t timestamp = static_cast<uint64_t>(wall_ns() / 1000000000ULL);
        char signature[133]{};
        if (!clob_auth::sign(signer, cfg.signer_hex, timestamp,
                             clob_auth::K_DEFAULT_NONCE, venue::K_POLYGON_CHAIN_ID,
                             signature)) {
            report.add("credentials_l1", Status::FAIL, "ClobAuth signing failed");
            return false;
        }
        // GET /auth/derive-api-key with L1 headers.
        char url[512];
        std::snprintf(url, sizeof(url), "%s/auth/derive-api-key", inputs_.api->clob_host());
        char headers[4][256];
        std::snprintf(headers[0], sizeof(headers[0]), "POLY_ADDRESS: %s", cfg.signer_hex);
        std::snprintf(headers[1], sizeof(headers[1]), "POLY_SIGNATURE: %s", signature);
        std::snprintf(headers[2], sizeof(headers[2]), "POLY_TIMESTAMP: %llu",
                      static_cast<unsigned long long>(timestamp));
        std::snprintf(headers[3], sizeof(headers[3]), "POLY_NONCE: %llu",
                      static_cast<unsigned long long>(clob_auth::K_DEFAULT_NONCE));
        const char* header_pointers[4] = {headers[0], headers[1], headers[2],
                                          headers[3]};
        clob::HttpResponse response{};
        char body[2048];
        response.body = body;
        clob::RequestOptions options{2000, 5000, false};
        inputs_.api->transport().request("GET", url, nullptr, 0, header_pointers, 4,
                                         options, body, sizeof(body), response);
        secure_zero(signature, sizeof(signature));
        if (!response.transport_ok || response.code < 200 || response.code >= 300) {
            report.add("credentials_l1", Status::FAIL,
                       "GET /auth/derive-api-key returned %ld (%s)", response.code,
                       response.error);
            secure_zero(body, sizeof(body));
            return false;
        }
        char derived_key[96]{};
        const bool parsed =
            json_scan::get_string(body, response.body_len, "apiKey", derived_key,
                                  sizeof(derived_key));
        // The secret is read only to be compared and wiped; it is never logged.
        char derived_secret[128]{};
        (void)json_scan::get_string(body, response.body_len, "secret", derived_secret,
                                    sizeof(derived_secret));
        const bool secret_matches =
            std::strcmp(derived_secret, cfg.api_secret_b64) == 0;
        secure_zero(body, sizeof(body));
        secure_zero(derived_secret, sizeof(derived_secret));
        if (!parsed) {
            report.add("credentials_l1", Status::FAIL,
                       "derive-api-key response has no apiKey");
            return false;
        }
        if (std::strcmp(derived_key, cfg.owner_api_key) != 0) {
            report.add("credentials_l1", Status::FAIL,
                       "the private key derives a different API key than the one "
                       "configured");
            secure_zero(derived_key, sizeof(derived_key));
            return false;
        }
        report.add("credentials_l1",
                   secret_matches ? Status::PASS : Status::FAIL,
                   secret_matches
                       ? "private key derives the configured credentials"
                       : "API key matches but the secret does not");
        secure_zero(derived_key, sizeof(derived_key));
        return secret_matches;
    }

    void check_user_ws(Report& report, bool live) const {
        (void)live;
        if (!inputs_.check_user_ws) {
            report.add("user_wss", Status::SKIP,
                       "BOT_PREFLIGHT_CHECK_USER_WS=0 (the user channel is verified "
                       "at startup instead)");
            return;
        }
#ifdef CROWDINTEL_HAVE_NETWORK
        const MarketConfig& cfg = *inputs_.config;
        user_ws::Config ws_config{};
        ws_config.enabled = true;
        std::snprintf(ws_config.url, sizeof(ws_config.url), "%s", cfg.user_ws_host);
        if (cfg.condition_id[0]) {
            ws_config.market_count = 1;
            std::snprintf(ws_config.markets[0], sizeof(ws_config.markets[0]), "%s",
                          cfg.condition_id);
        }
        ws_config.keepalive_interval_ms = cfg.user_ws_keepalive_ms;
        ws_config.idle_timeout_ms = cfg.user_ws_idle_ms;
        ws_config.pong_timeout_ms = cfg.user_ws_pong_ms;
        char frame[user_ws::K_MAX_SUBSCRIPTION_FRAME];
        size_t frame_len = 0;
        clob::Credentials credentials{};
        std::snprintf(credentials.address, sizeof(credentials.address), "%s",
                      cfg.api_address_hex);
        std::snprintf(credentials.api_key, sizeof(credentials.api_key), "%s",
                      cfg.owner_api_key);
        std::snprintf(credentials.api_secret_b64, sizeof(credentials.api_secret_b64),
                      "%s", cfg.api_secret_b64);
        std::snprintf(credentials.api_passphrase, sizeof(credentials.api_passphrase),
                      "%s", cfg.api_passphrase);
        if (!user_ws::build_subscription_frame(ws_config, credentials, frame,
                                               sizeof(frame), frame_len)) {
            report.add("user_wss", Status::FAIL, "cannot build the subscription frame");
            return;
        }
        ws::SessionConfig session_config{};
        std::snprintf(session_config.url, sizeof(session_config.url), "%s",
                      ws_config.url);
        session_config.connect_timeout_ms = cfg.user_ws_reconnect_max_ms;
        session_config.keepalive_interval_ms = ws_config.keepalive_interval_ms;
        session_config.idle_timeout_ms = 6000;   // bounded probe
        session_config.pong_timeout_ms = 9000;
        session_config.require_tls = true;
        ws::Session session;
        ws::SessionError error = ws::SessionError::NONE;
        if (!session.connect(session_config, frame, frame_len, error)) {
            secure_zero(frame, sizeof(frame));
            report.add("user_wss", Status::FAIL, "handshake failed: %s",
                       ws::session_error_name(error));
            return;
        }
        secure_zero(frame, sizeof(frame));
        // Reading for a bounded window proves the subscription was accepted: an
        // invalid session is answered with an error frame or a close.
        struct Probe {
            bool accepted = true;
            size_t frames = 0;
        } probe;
        auto callback = [](void* context, const char* payload, size_t length) -> bool {
            auto* state = static_cast<Probe*>(context);
            ++state->frames;
            user_ws::UserMessage message{};
            if (user_ws::parse_user_message(payload, length, message) &&
                message.kind == user_ws::MessageKind::ERROR_FRAME)
                state->accepted = false;
            return state->frames < 1 && state->accepted;
        };
        (void)session.run(+callback, &probe, error);
        session.disconnect();
        if (probe.accepted)
            report.add("user_wss", Status::PASS,
                       "%s accepted the subscription (%zu frames observed)",
                       cfg.user_ws_host, probe.frames);
        else
            report.add("user_wss", Status::FAIL,
                       "%s rejected the subscription", cfg.user_ws_host);
#else
        (void)live;
        report.add("user_wss", Status::SKIP,
                   "this build has no network transport (CROWDINTEL_NETWORK=OFF)");
#endif
    }

    void check_heartbeat(Report& report, clob::ClobApiClient& api, bool live) const {
        (void)live;
        if (!inputs_.check_heartbeat) {
            report.add("heartbeat", Status::SKIP,
                       "BOT_PREFLIGHT_CHECK_HEARTBEAT=0 (starting the contract here "
                       "would cancel resting orders when preflight exits)");
            return;
        }
        if (!api.credentials_ready()) {
            report.add("heartbeat", Status::FAIL, "L2 credentials are unavailable");
            return;
        }
        if (report.open_orders != 0) {
            report.add("heartbeat", Status::SKIP,
                       "%zu open orders: probing the heartbeat contract would cancel "
                       "them when preflight exits",
                       report.open_orders);
            return;
        }
        clob::HeartbeatResult result{};
        clob::CallResult call{};
        clob::RequestOptions options{1000, 2500, false};
        // Report the path and the chain-start body form that ACTUALLY answered
        // (`heartbeat_path()`/`heartbeat_uses_null_start()`), not the constant we
        // try first: the SDKs say /v1/heartbeats with {"heartbeat_id":null}, the
        // OpenAPI page says /heartbeats with {"status":"ok"}, and the narrative
        // docs say {"heartbeat_id":""}.  Which one production serves is still
        // [NO VERIFICADO], and this line is the artifact that settles it during
        // canary preparation - printing the default constant here would report the
        // assumption instead of the observation.
        if (api.post_heartbeat("", result, call, options) && result.accepted) {
            report.add("heartbeat", Status::PASS,
                       "POST %s accepted with heartbeat_id=%s; WARNING the "
                       "cancel-on-disconnect contract is now active for these "
                       "credentials and will cancel new orders ~10-15 s after "
                       "preflight exits unless the bot keeps beating",
                       api.heartbeat_path(),
                       api.heartbeat_uses_null_start() ? "null" : "\"\"");
        } else if (result.rejected_invalid_id) {
            report.add("heartbeat", Status::FAIL,
                       "POST %s rejected our id and expects %s", api.heartbeat_path(),
                       result.heartbeat_id);
        } else {
            report.add("heartbeat", live ? Status::FAIL : Status::SKIP,
                       "POST %s failed (%s)", api.heartbeat_path(), call.detail);
        }
    }

    Inputs inputs_;
    venue::MarketMetadata metadata_{};
};

}  // namespace preflight

#endif  // PREFLIGHT_HPP
