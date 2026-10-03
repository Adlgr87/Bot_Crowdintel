#ifndef MARKET_CONFIG_HPP
#define MARKET_CONFIG_HPP

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Strict name validation walks the process environment; only POSIX platforms
// define `environ`.
#if defined(__unix__) || defined(__APPLE__)
extern char** environ;
#endif

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"
#include "market_metadata.hpp"
#include "order_state_machine.hpp"
#include "polymarket_order.hpp"
#include "alpha_receiver.hpp"

struct MarketConfig {
    // Three explicit execution modes. There is no implicit default and no
    // `mock` alias: "deterministic offline replay", "live public data with
    // simulated fills" and "orders can reach the venue" have different
    // requirements, and conflating them is how an offline run ends up writing
    // a production journal or a paper run points at real credentials.
    //
    //   replay - no network at all; venue parameters may come from ENV
    //            fixtures; needs no credentials; writes no journal.
    //   paper  - live public market data + live metadata resolution; orders
    //            are simulated locally and never sent; no venue credentials
    //            and no journal (the journal is venue evidence).
    //   live   - the real thing: venue credentials, journal, arming flag.
    enum class Mode : uint8_t { kReplay = 0, kPaper = 1, kLive = 2 };

    // What a caller is allowed to do, and therefore which secrets it may see.
    // The tools are deliberately narrower than the trader:
    //   kTrader    - the bot: signs orders (needs the private key).
    //   kTool      - read-only authenticated tool (crowdintel-preflight): L2
    //                credentials but never the signing key.
    //   kInspector - public metadata inspection (crowdintel-metadata): reads
    //                no secret at all.
    enum class Role : uint8_t { kTrader = 0, kTool = 1, kInspector = 2 };

    struct LoadOptions {
        // A user-provided constructor instead of default member initializers:
        // `load()` uses LoadOptions{} as a default argument inside the
        // enclosing class, and default member initializers are not available
        // until that class is complete.
        LoadOptions() : role(Role::kTrader), force_replay(false) {}
        Role role;
        // Offline builds (no curl/OpenSSL) only have the replay transports;
        // asking for paper/live in one of them is a configuration error.
        bool force_replay;
    };

    // Identity / credentials
    char private_key_hex[65]{};
    uint8_t maker[20]{};
    uint8_t signer[20]{};
    char maker_hex[43]{};
    char signer_hex[43]{};
    char api_address_hex[43]{};  // address to which the supplied API key belongs
    char owner_api_key[64]{};
    char api_secret_b64[128]{};
    char api_passphrase[128]{};

    // Market
    //
    // The values below are *operator selections* in live mode, never venue
    // parameters: the venue-mutable ones (tick size, minimum order size, fee
    // rate/exponent, negative-risk flag, token id) are resolved from public
    // REST metadata into `runtime` before any thread starts, and live startup
    // rejects the corresponding ENV variables outright.
    char token_id_dec[80]{};      // mock/paper only; live fills from runtime
    uint8_t token_id_be[32]{};    // mock/paper only; live fills from runtime
    char market_slug[96] = "mock-market";
    char condition_id[67]{};      // optional operator selector (live)
    char outcome[32]{};           // optional operator selector (live)
    uint64_t market_hash = 0;
    uint64_t tick_size = 10000;   // mock/paper only; live -> runtime.tick_size
    bool neg_risk = false;        // mock/paper only; live -> runtime.neg_risk
    crowdintel::MarketRuntime runtime{};
    uint8_t signature_type = 0;
    char order_type[8] = "FAK";
    uint64_t gtd_ttl_seconds = 0;
    char clob_host[128] = "https://clob.polymarket.com";
    char gamma_host[128] = "https://gamma-api.polymarket.com";
    char ws_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/market";
    int64_t max_clock_offset_ms = 2000;

    // Strategy / risk.  Values are deliberately conservative until account
    // reconciliation is implemented.
    double bankroll_usd = 1000.0;
    double kelly_fraction = 0.10;
    double min_edge = 0.02;
    double min_confidence = 0.85;
    double max_q_value = 0.05;
    double taker_fee_rate = 0.07;       // conservative maximum current category
    double max_order_usd = 100.0;
    double max_exposure_usd = 250.0;
    double max_daily_loss_usd = 50.0;
    uint64_t initial_position_shares = 0;
    uint64_t min_size_shares = 5000000;
    uint64_t presign_ttl_ms = 3000;
    uint64_t signal_ttl_ms = 2000;
    uint64_t max_book_age_ms = 3000;

    // Alpha HTTP receiver (designed to sit behind a TLS/auth reverse proxy).
    char alpha_bind[64] = "127.0.0.1";
    uint16_t alpha_port = 8088;
    char alpha_bearer_token[128]{};

    // Runtime
    int pin_cpu = -1;
    int cold_cpu = -1;
    long max_ticks = 0;
    Mode mode = Mode::kReplay;
    bool live_armed = false;
    char tls_pin[128]{};
    char kill_switch_file[192] = "/tmp/crowdintel.kill";
    // User-data channel. Empty means "derive /ws/user from WS_HOST".
    char user_ws_host[192]{};

    // Phase 2 order ledger: append-only journal used for crash recovery. Live
    // mode requires an explicit path (a missing recovery source is not a
    // default); mock runs may leave it empty and trade without persistence.
    char ledger_path[192]{};
    bool ledger_fsync = true;
    uint64_t ledger_max_bytes = 64ull * 1024 * 1024;

    ~MarketConfig() {
        secure_zero(private_key_hex, sizeof(private_key_hex));
        secure_zero(api_secret_b64, sizeof(api_secret_b64));
        secure_zero(api_passphrase, sizeof(api_passphrase));
        secure_zero(alpha_bearer_token, sizeof(alpha_bearer_token));
    }

    MarketConfig() = default;
    MarketConfig(const MarketConfig&) = delete;
    MarketConfig& operator=(const MarketConfig&) = delete;

    static const char* env(const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return value && *value ? value : fallback;
    }

    static bool env_d(const char* key, double fallback, double& out) {
        const char* value = std::getenv(key);
        if (!value || !*value) { out = fallback; return true; }
        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(parsed))
            return false;
        out = parsed;
        return true;
    }

    static bool env_l(const char* key, long fallback, long& out) {
        const char* value = std::getenv(key);
        if (!value || !*value) { out = fallback; return true; }
        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (errno == ERANGE || !end || *end != '\0') return false;
        out = parsed;
        return true;
    }

    // Reads and validates the whole configuration exactly once. The mode is
    // never implicit: BOT_MODE must name replay, paper or live, so "I forgot
    // the mode" can never mean "live". Every BOT_/CLOB_/GAMMA_/WS_ variable
    // that this build does not read aborts startup instead of being ignored.
    const char* load(const LoadOptions& options = LoadOptions{}) {
        load_error_[0] = '\0';
        const bool role_trader = options.role == Role::kTrader;
        const bool role_tool = options.role == Role::kTool;
        const bool role_inspector = options.role == Role::kInspector;

        // 1. Mode.
        const char* mode_env = std::getenv("BOT_MODE");
        if (mode_env && *mode_env) {
            if (std::strcmp(mode_env, "replay") == 0) mode = Mode::kReplay;
            else if (std::strcmp(mode_env, "paper") == 0) mode = Mode::kPaper;
            else if (std::strcmp(mode_env, "live") == 0) mode = Mode::kLive;
            else if (std::strcmp(mode_env, "mock") == 0)
                return "BOT_MODE=mock was ambiguous and is gone: use replay "
                       "(deterministic, offline) or paper (live public data, "
                       "simulated fills)";
            else
                return failf("BOT_MODE must be replay, paper or live (got \"%s\")",
                             mode_env);
        } else if (!options.force_replay) {
            return "BOT_MODE is required: replay, paper or live (there is no "
                   "implicit mode)";
        }
        if (options.force_replay && mode != Mode::kReplay)
            return failf("this build has no network transports: BOT_MODE=%s is "
                         "unavailable, use replay", mode_name(mode));
        const bool replay = mode == Mode::kReplay;
        const bool live = mode == Mode::kLive;

        // 2. Name hygiene: typos and leftovers from an older deployment are
        // configuration errors, never silently ignored defaults.
        if (const char* unknown = check_environment_names()) return unknown;
        if (role_inspector && replay)
            return "a metadata inspection resolves the live market document: "
                   "BOT_MODE must be paper or live";
        if (role_tool && !live)
            return "the account preflight authenticates against the venue: "
                   "BOT_MODE must be live";

        // 3. Signing key: only the trader may hold one. The read-only
        // preflight refuses to run with key material in its environment.
        if (role_tool && (std::getenv("BOT_PRIVATE_KEY_HEX") ||
                          std::getenv("BOT_PRIVATE_KEY_HEX_FILE")))
            return "BOT_PRIVATE_KEY_HEX(_FILE) must be absent for the account "
                   "preflight: it never loads the signing key";
        if (role_trader) {
            if (!read_secret("BOT_PRIVATE_KEY_HEX", private_key_hex,
                             sizeof(private_key_hex)) ||
                std::strlen(private_key_hex) != 64)
                return "BOT_PRIVATE_KEY_HEX or BOT_PRIVATE_KEY_HEX_FILE must contain 64 hex chars";
            uint8_t key_check[32];
            const bool private_key_is_hex = parse_hex_bytes(
                private_key_hex, std::strlen(private_key_hex), key_check,
                sizeof(key_check));
            secure_zero(key_check, sizeof(key_check));
            if (!private_key_is_hex)
                return "BOT_PRIVATE_KEY_HEX contains non-hexadecimal characters";
        } else {
            private_key_hex[0] = '\0';
        }

        long neg_risk_l = 0, sig = 0, presign_l = 0, signal_l = 0;
        long book_age_l = 0, pin_l = -1, cold_l = -1, ticks_l = 0;
        long armed_l = 0, gtd_l = 0, clock_offset_l = 0;
        long ledger_fsync_l = 1, ledger_max_l = 67108864;
        double tick = 0, min_size = 0, initial_position = 0;
        if (!env_l("BOT_NEG_RISK", 0, neg_risk_l) ||
            !env_l("BOT_SIGNATURE_TYPE", 0, sig) ||
            !env_d("BOT_TICK_SIZE", 0.01, tick) ||
            !env_d("BOT_BANKROLL_USD", bankroll_usd, bankroll_usd) ||
            !env_d("BOT_KELLY_FRACTION", kelly_fraction, kelly_fraction) ||
            !env_d("BOT_MIN_EDGE", min_edge, min_edge) ||
            !env_d("BOT_MIN_CONFIDENCE", min_confidence, min_confidence) ||
            !env_d("BOT_MAX_Q_VALUE", max_q_value, max_q_value) ||
            !env_d("BOT_TAKER_FEE_RATE", taker_fee_rate, taker_fee_rate) ||
            !env_d("BOT_MAX_ORDER_USD", max_order_usd, max_order_usd) ||
            !env_d("BOT_MAX_EXPOSURE_USD", max_exposure_usd, max_exposure_usd) ||
            !env_d("BOT_MAX_DAILY_LOSS_USD", max_daily_loss_usd,
                   max_daily_loss_usd) ||
            !env_d("BOT_MIN_SIZE_SHARES", 5.0, min_size) ||
            !env_d("BOT_INITIAL_POSITION_SHARES", 0.0, initial_position) ||
            !env_l("BOT_PRESIGN_TTL_MS", static_cast<long>(presign_ttl_ms),
                   presign_l) ||
            !env_l("BOT_SIGNAL_TTL_MS", static_cast<long>(signal_ttl_ms),
                   signal_l) ||
            !env_l("BOT_MAX_BOOK_AGE_MS", static_cast<long>(max_book_age_ms),
                   book_age_l) ||
            !env_l("BOT_PIN_CPU", -1, pin_l) ||
            !env_l("BOT_COLD_CPU", -1, cold_l) ||
            !env_l("BOT_TICKS", 0, ticks_l) ||
            !env_l("BOT_ENABLE_LIVE_TRADING", 0, armed_l) ||
            !env_l("BOT_GTD_TTL_SECONDS", 0, gtd_l) ||
            !env_l("BOT_MAX_CLOCK_OFFSET_MS", 2000, clock_offset_l) ||
            !env_l("BOT_LEDGER_FSYNC", 1, ledger_fsync_l) ||
            !env_l("BOT_LEDGER_MAX_BYTES", 67108864, ledger_max_l))
            return "invalid numeric configuration value";

        if ((neg_risk_l != 0 && neg_risk_l != 1) ||
            (armed_l != 0 && armed_l != 1) ||
            (ledger_fsync_l != 0 && ledger_fsync_l != 1))
            return "BOT_NEG_RISK/BOT_ENABLE_LIVE_TRADING/BOT_LEDGER_FSYNC must be 0 or 1";
        if (ledger_max_l < 1048576 || ledger_max_l > 4294967296L)
            return "BOT_LEDGER_MAX_BYTES must be within [1 MiB, 4 GiB]";
        if (sig < 0 || sig > 3)
            return "BOT_SIGNATURE_TYPE must be 0, 1, 2, or 3";
        if (!(tick > 0.0 && tick < 1.0) ||
            tick * 1000000.0 > static_cast<double>(UINT64_MAX))
            return "BOT_TICK_SIZE is out of range";
        const double min_size_scaled = min_size * 1000000.0;
        const double initial_position_scaled = initial_position * 1000000.0;
        if (min_size <= 0.0 || initial_position < 0.0 ||
            min_size >= static_cast<double>(UINT64_MAX) / 1000000.0 ||
            initial_position >= static_cast<double>(UINT64_MAX) / 1000000.0 ||
            std::fabs(min_size_scaled - std::round(min_size_scaled)) > 1e-6 ||
            std::fabs(initial_position_scaled -
                      std::round(initial_position_scaled)) > 1e-6 ||
            static_cast<uint64_t>(std::round(min_size_scaled)) %
                    K_SHARE_QUANTUM != 0 ||
            static_cast<uint64_t>(std::round(initial_position_scaled)) %
                    K_SHARE_QUANTUM != 0)
            return "share configuration must be non-negative and exact to 0.01 shares";
        constexpr uint64_t MAX_SAFE_MS = UINT64_MAX / 1000000ULL;
        if (presign_l <= 0 || signal_l <= 0 || book_age_l <= 0 ||
            static_cast<uint64_t>(presign_l) > MAX_SAFE_MS ||
            static_cast<uint64_t>(signal_l) > MAX_SAFE_MS ||
            static_cast<uint64_t>(book_age_l) > MAX_SAFE_MS ||
            ticks_l < 0 || gtd_l < 0 || gtd_l > 31536000 ||
            pin_l < -1 || cold_l < -1 || pin_l > INT_MAX || cold_l > INT_MAX ||
            clock_offset_l < 100 || clock_offset_l > 60000)
            return "runtime duration/CPU/clock configuration is out of range";

        neg_risk = neg_risk_l == 1;
        signature_type = static_cast<uint8_t>(sig);
        if (signature_type == 3)
            return "POLY_1271 requires ERC-7739 wrapping; this build fails closed. Use type 0/1/2 or the official V2 signer sidecar";
        tick_size = static_cast<uint64_t>(std::round(tick * 1000000.0));
        min_size_shares = static_cast<uint64_t>(std::round(min_size_scaled));
        initial_position_shares =
            static_cast<uint64_t>(std::round(initial_position_scaled));
        presign_ttl_ms = static_cast<uint64_t>(presign_l);
        signal_ttl_ms = static_cast<uint64_t>(signal_l);
        max_book_age_ms = static_cast<uint64_t>(book_age_l);
        pin_cpu = static_cast<int>(pin_l);
        cold_cpu = static_cast<int>(cold_l);
        max_ticks = ticks_l;
        live_armed = armed_l == 1;
        gtd_ttl_seconds = static_cast<uint64_t>(gtd_l);
        max_clock_offset_ms = clock_offset_l;
        ledger_fsync = ledger_fsync_l == 1;
        ledger_max_bytes = static_cast<uint64_t>(ledger_max_l);

        // Paper and live must not carry venue-controlled parameters from
        // ENV/config. They are resolved from public REST metadata before
        // anything starts. Only replay may use fixtures.
        if (!replay) {
            static const char* const kVenueParams[] = {
                "BOT_TICK_SIZE", "BOT_MIN_SIZE_SHARES", "BOT_TAKER_FEE_RATE",
                "BOT_NEG_RISK", "BOT_TOKEN_ID"};
            for (const char* key : kVenueParams) {
                const char* value = std::getenv(key);
                if (value && *value)
                    return failf("%s is venue metadata resolved from the market "
                                 "document and must not be set in %s mode; use "
                                 "replay for fixtures", key, mode_name(mode));
            }
            // Unknown until market metadata is resolved; callers fail closed.
            tick_size = 0;
            min_size_shares = 0;
            neg_risk = false;
            taker_fee_rate = 0.0;
            token_id_dec[0] = '\0';
            std::memset(token_id_be, 0, sizeof(token_id_be));
        }

        if (!copy_env(order_type, "BOT_ORDER_TYPE", order_type) ||
            !copy_env(clob_host, "CLOB_HOST", clob_host) ||
            !copy_env(gamma_host, "GAMMA_HOST", gamma_host) ||
            !copy_env(ws_host, "WS_HOST", ws_host) ||
            !copy_env(user_ws_host, "BOT_USER_WS_HOST", "") ||
            !copy_env(condition_id, "BOT_CONDITION_ID", "") ||
            !copy_env(outcome, "BOT_OUTCOME", "") ||
            !copy_env(tls_pin, "BOT_TLS_PIN", "") ||
            !copy_env(kill_switch_file, "BOT_KILL_SWITCH_FILE",
                      kill_switch_file) ||
            !copy_env(ledger_path, "BOT_LEDGER_PATH", "") ||
            !copy_env(market_slug, "BOT_MARKET_SLUG",
                      replay ? "mock-market" : "") ||
            !copy_env(alpha_bind, "BOT_ALPHA_BIND", alpha_bind))
            return "configuration string exceeds its bounded capacity";
        if (!safe_config_text(order_type) || !safe_config_text(clob_host) ||
            !safe_config_text(gamma_host) || !safe_config_text(ws_host) ||
            !safe_config_text(user_ws_host) ||
            !safe_config_text(tls_pin) || !safe_config_text(kill_switch_file) ||
            !safe_config_text(market_slug) || !safe_config_text(condition_id) ||
            !safe_config_text(outcome) || !safe_config_text(alpha_bind) ||
            !safe_config_text(ledger_path))
            return "configuration strings cannot contain control characters";
        if (ledger_path[0] && ledger_path[0] != '/')
            return "BOT_LEDGER_PATH must be an absolute path";
        // The journal is venue evidence: only a live run may have one. A
        // paper/replay run pointed at the production journal would write
        // simulated orders into the record a later live run reconciles.
        if (!live && (std::getenv("BOT_LEDGER_PATH") ||
                      std::getenv("BOT_LEDGER_FSYNC") ||
                      std::getenv("BOT_LEDGER_MAX_BYTES")))
            return "BOT_LEDGER_PATH/BOT_LEDGER_FSYNC/BOT_LEDGER_MAX_BYTES are "
                   "live-only: replay and paper never write the venue journal";
        // Read-only tools (metadata resolver, account preflight) do not trade
        // and therefore do not need a journal; the trader that can place an
        // order does.
        if (live && role_trader && !ledger_path[0])
            return "live mode requires BOT_LEDGER_PATH: no recovery source, no arming";
        if (condition_id[0] && !crowdintel::valid_condition_id(condition_id))
            return "BOT_CONDITION_ID must be 0x followed by 64 lowercase hex chars";
        if (tls_pin[0] && !valid_tls_pins(tls_pin))
            return "BOT_TLS_PIN must contain one or two sha256// pins separated by a semicolon";
        long alpha_port_l = alpha_port;
        if (!env_l("BOT_ALPHA_PORT", alpha_port, alpha_port_l) ||
            alpha_port_l < 1 || alpha_port_l > 65535)
            return "BOT_ALPHA_PORT is invalid";
        alpha_port = static_cast<uint16_t>(alpha_port_l);
        if (!market_slug[0] && condition_id[0])
            std::memcpy(market_slug, "condition-pending", 18);  // resolved later
        market_hash = alpha_hash_bytes(market_slug, std::strlen(market_slug));

        if (std::strcmp(order_type, "GTC") != 0 &&
            std::strcmp(order_type, "GTD") != 0 &&
            std::strcmp(order_type, "FOK") != 0 &&
            std::strcmp(order_type, "FAK") != 0)
            return "BOT_ORDER_TYPE must be GTC, GTD, FOK, or FAK";
        if (std::strcmp(order_type, "GTD") == 0 && gtd_ttl_seconds < 120)
            return "GTD requires BOT_GTD_TTL_SECONDS >= 120 (plus the venue's 60-second buffer)";
        if (std::strcmp(order_type, "GTD") == 0 &&
            presign_ttl_ms >= gtd_ttl_seconds * 1000ULL)
            return "BOT_PRESIGN_TTL_MS must be shorter than the GTD lifetime";
        // Replay keeps the historic configured tick; paper and live already
        // cleared it and resolve the venue grid from REST metadata.
        if (replay) {
            const double scaled_tick = tick * 1000000.0;
            if (std::fabs(scaled_tick - std::round(scaled_tick)) > 1e-6 ||
                tick_size == 0 || tick_size >= 1000000 ||
                1000000 % tick_size != 0 ||
                amount_quantum_for_tick(tick_size) == 0)
                return "BOT_TICK_SIZE is not a supported exact V2 tick";
        }
        if (bankroll_usd <= 0 || kelly_fraction <= 0 || kelly_fraction > 1 ||
            min_edge < 0 || min_edge >= 1 || min_confidence < 0 ||
            min_confidence > 1 || max_q_value < 0 || max_q_value > 1 ||
            max_order_usd <= 0 || max_exposure_usd <= 0 ||
            max_daily_loss_usd <= 0)
            return "invalid strategy/risk configuration";
        if (replay && (taker_fee_rate < 0 || taker_fee_rate > 1))
            return "BOT_TAKER_FEE_RATE must be within [0,1] in replay mode";
        if (!replay && !condition_id[0] && !valid_market_slug(market_slug))
            return failf("%s mode requires a URL-safe BOT_MARKET_SLUG or "
                         "BOT_CONDITION_ID", mode_name(mode));
        if (replay && !valid_market_slug(market_slug))
            return "BOT_MARKET_SLUG must be a non-empty URL-safe slug";
        if (!replay && !valid_secure_url(clob_host, "https://", false))
            return failf("CLOB_HOST must be an origin-only https:// URL in %s mode",
                         mode_name(mode));
        if (!replay && !valid_secure_url(gamma_host, "https://", false))
            return failf("GAMMA_HOST must be an origin-only https:// URL in %s mode",
                         mode_name(mode));
        if (!replay && !valid_secure_url(ws_host, "wss://", true))
            return failf("WS_HOST must be a valid wss:// URL in %s mode",
                         mode_name(mode));
        if (user_ws_host[0] && !valid_secure_url(user_ws_host, "wss://", true))
            return "BOT_USER_WS_HOST must be a valid wss:// URL";

        // 4. Arming is a live-only value. Replay and paper cannot send an
        // order, so carrying the armed flag there is a contradiction.
        if (!live && live_armed)
            return "BOT_ENABLE_LIVE_TRADING=1 requires BOT_MODE=live: replay and "
                   "paper never send an order";
        if (live && !live_armed && !role_inspector)
            return "live mode is disarmed; set BOT_ENABLE_LIVE_TRADING=1 after "
                   "completing the preflight checklist";

        // 5. Secrets. A secret that is present must be well-formed (the read
        // also removes it from /proc/self/environ); a secret that is required
        // must be present. The metadata tool reads none of them.
        if (!role_inspector) {
            const bool creds_required = live || role_tool;
            bool creds_ok = load_secret(creds_required, "CLOB_API_KEY",
                                        owner_api_key);
            creds_ok = load_secret(creds_required, "CLOB_SECRET",
                                   api_secret_b64) && creds_ok;
            creds_ok = load_secret(creds_required, "CLOB_PASSPHRASE",
                                   api_passphrase) && creds_ok;
            if (!creds_ok)
                return "CLOB_API_KEY/CLOB_SECRET/CLOB_PASSPHRASE (or their "
                       "*_FILE forms) are required here and must be well-formed";
            if (owner_api_key[0] &&
                (!printable_secret(owner_api_key) ||
                 std::strchr(owner_api_key, '"') ||
                 std::strchr(owner_api_key, '\\')))
                return "CLOB credentials contain unsafe whitespace/control/JSON characters";
            if (api_secret_b64[0]) {
                uint8_t decoded_secret[64]{};
                const size_t decoded_secret_len = base64url_decode(
                    api_secret_b64, std::strlen(api_secret_b64), decoded_secret,
                    sizeof(decoded_secret));
                secure_zero(decoded_secret, sizeof(decoded_secret));
                if (decoded_secret_len == 0 || decoded_secret_len == SIZE_MAX)
                    return "CLOB_SECRET is not valid bounded base64/base64url";
            }
            if (api_passphrase[0] && !printable_secret(api_passphrase))
                return "CLOB credentials contain unsafe whitespace/control/JSON characters";

            const bool alpha_required = role_trader && !replay;
            if (!load_secret(alpha_required, "BOT_ALPHA_BEARER_TOKEN",
                             alpha_bearer_token) ||
                (alpha_bearer_token[0] &&
                 (std::strlen(alpha_bearer_token) < 16 ||
                  !printable_secret(alpha_bearer_token))))
                return "BOT_ALPHA_BEARER_TOKEN (or *_FILE) must contain at least "
                       "16 printable chars in paper/live";
        }
        // Bodies carry an owner field; replay/paper have no real API key, and a
        // placeholder makes it obvious in any log or capture that they cannot
        // be venue orders.
        if (owner_api_key[0] == '\0')
            std::snprintf(owner_api_key, sizeof(owner_api_key),
                          "00000000-0000-0000-0000-000000000000");

        // 6. Outcome token. In paper/live it comes from the resolved market
        // document (the ENV form is rejected above); only replay may set it,
        // and even then there is an explicit fixture default.
        if (replay) {
            const char* tok = env("BOT_TOKEN_ID",
                "71321045679252212594626395510336467040167069592778062791519851593659551227755");
            const size_t token_len = std::strlen(tok);
            if (token_len >= sizeof(token_id_dec) ||
                (token_len > 1 && tok[0] == '0') ||
                !parse_uint256_dec(tok, token_len, token_id_be))
                return "BOT_TOKEN_ID is not a canonical uint256 decimal";
            bool token_nonzero = false;
            for (const uint8_t byte : token_id_be) token_nonzero |= byte != 0;
            if (!token_nonzero) return "BOT_TOKEN_ID cannot be zero";
            std::memcpy(token_id_dec, tok, token_len + 1);
        } else {
            token_id_dec[0] = '\0';
            std::memset(token_id_be, 0, sizeof(token_id_be));
        }
        return nullptr;
    }

    // Called once after the only EIP712Signer has derived the EOA address.
    const char* finalize_identity(const uint8_t signer_address[20]) {
        std::memcpy(signer, signer_address, 20);
        const char* maker_env = std::getenv("BOT_MAKER_ADDRESS");
        if (maker_env) {
            if (!parse_hex_bytes(maker_env, std::strlen(maker_env), maker, 20))
                return "BOT_MAKER_ADDRESS is not a valid 20-byte hex address";
        } else {
            if (signature_type == 1 || signature_type == 2)
                return "BOT_MAKER_ADDRESS is required for proxy/safe signature types";
            std::memcpy(maker, signer, 20);
        }
        format_address(maker, maker_hex);
        format_address(signer, signer_hex);

        const char* api_address = std::getenv("BOT_API_ADDRESS");
        if (api_address) {
            uint8_t parsed[20];
            if (!parse_hex_bytes(api_address, std::strlen(api_address), parsed, 20))
                return "BOT_API_ADDRESS is not a valid 20-byte hex address";
            format_address(parsed, api_address_hex);
        } else {
            // L2 credentials for legacy types are normally bound to the EOA.
            std::memcpy(api_address_hex, signer_hex, sizeof(signer_hex));
        }
        return nullptr;
    }

    uint64_t wire_expiration(uint64_t now_seconds) const noexcept {
        if (std::strcmp(order_type, "GTD") != 0) return 0;
        const uint64_t offset = 60ULL + gtd_ttl_seconds;
        if (now_seconds > UINT64_MAX - offset) return 0;
        return now_seconds + offset;
    }

    // ── Runtime-parameter accessors ──────────────────────────────────────────
    // Live mode never falls back to a configured/hardcoded venue parameter:
    // `trading_parameters_ready()` is false until the resolver validated the
    // venue metadata, and callers must gate on it.

    static const char* mode_name(Mode value) noexcept {
        switch (value) {
            case Mode::kReplay: return "replay";
            case Mode::kPaper: return "paper";
            case Mode::kLive: return "live";
        }
        return "unknown";
    }

    bool replay_mode() const noexcept { return mode == Mode::kReplay; }
    bool paper_mode() const noexcept { return mode == Mode::kPaper; }
    bool live_mode() const noexcept { return mode == Mode::kLive; }

    // Paper and live both use the real network transports; only replay is
    // fully offline.
    bool live_transport() const noexcept { return mode != Mode::kReplay; }

    bool trading_parameters_ready() const noexcept {
        return replay_mode() || runtime.resolved;
    }

    uint64_t effective_tick() const noexcept {
        if (runtime.resolved) return runtime.tick_size;
        return replay_mode() ? tick_size : 0;  // 0 == unknown, callers fail closed
    }

    uint64_t effective_min_size() const noexcept {
        if (runtime.resolved) return runtime.min_order_size;
        return replay_mode() ? min_size_shares : 0;
    }

    bool effective_neg_risk() const noexcept {
        if (runtime.resolved) return runtime.neg_risk;
        return replay_mode() ? neg_risk : false;
    }

    const char* effective_token_id_dec() const noexcept {
        return runtime.resolved ? runtime.token_id_dec : token_id_dec;
    }

    const uint8_t* effective_token_id_be() const noexcept {
        return runtime.resolved ? runtime.token_id_be : token_id_be;
    }

    // Fee per share (1e-6 price units) from the venue fee curve
    // fee = C × rate × (p × (1-p))^exponent. Replay keeps the historic
    // configured coefficient with the published exponent 1.
    double fee_per_share(double price) const noexcept {
        if (runtime.resolved) return crowdintel::fee_per_share(runtime, price);
        if (!replay_mode()) return 0.0;  // unreachable once ready-gated; see above
        return taker_fee_rate * price * (1.0 - price);
    }


private:
    // Last dynamic error text; valid until the next load() call on this object.
    char load_error_[320]{};

    const char* failf(const char* format, ...) {
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(load_error_, sizeof(load_error_), format, arguments);
        va_end(arguments);
        return load_error_;
    }

    static bool is_config_prefix(const char* name, size_t length) noexcept {
        static const char* const kPrefixes[] = {"BOT_", "CLOB_", "GAMMA_", "WS_"};
        for (const char* prefix : kPrefixes) {
            const size_t prefix_length = std::strlen(prefix);
            if (length > prefix_length &&
                std::memcmp(name, prefix, prefix_length) == 0)
                return true;
        }
        return false;
    }

    // Every BOT_/CLOB_/GAMMA_/WS_ name this build reads. The secrets are
    // listed separately because their `_FILE` forms are derived, not written
    // out in the source.
    static bool known_env_name(const char* name, size_t length) noexcept {
        static const char* const kNames[] = {
            "BOT_ALPHA_BIND", "BOT_ALPHA_PORT", "BOT_API_ADDRESS",
            "BOT_BANKROLL_USD", "BOT_COLD_CPU", "BOT_CONDITION_ID",
            "BOT_ENABLE_LIVE_TRADING", "BOT_GTD_TTL_SECONDS",
            "BOT_INITIAL_POSITION_SHARES", "BOT_KELLY_FRACTION",
            "BOT_KILL_SWITCH_FILE", "BOT_LEDGER_FSYNC",
            "BOT_LEDGER_MAX_BYTES", "BOT_LEDGER_PATH", "BOT_MAKER_ADDRESS",
            "BOT_MARKET_SLUG", "BOT_MAX_BOOK_AGE_MS", "BOT_MAX_CLOCK_OFFSET_MS",
            "BOT_MAX_DAILY_LOSS_USD", "BOT_MAX_EXPOSURE_USD",
            "BOT_MAX_ORDER_USD", "BOT_MAX_Q_VALUE", "BOT_MIN_CONFIDENCE",
            "BOT_MIN_EDGE", "BOT_MIN_SIZE_SHARES", "BOT_MODE", "BOT_NEG_RISK",
            "BOT_ORDER_TYPE", "BOT_OUTCOME", "BOT_PIN_CPU",
            "BOT_PRESIGN_TTL_MS", "BOT_SIGNAL_TTL_MS", "BOT_SIGNATURE_TYPE",
            "BOT_TAKER_FEE_RATE", "BOT_TICKS", "BOT_TICK_SIZE", "BOT_TLS_PIN",
            "BOT_TOKEN_ID", "BOT_USER_WS_HOST", "CLOB_HOST", "GAMMA_HOST",
            "WS_HOST"};
        for (const char* candidate : kNames) {
            if (std::strlen(candidate) == length &&
                std::memcmp(candidate, name, length) == 0)
                return true;
        }
        static const char* const kSecrets[] = {
            "BOT_PRIVATE_KEY_HEX", "BOT_ALPHA_BEARER_TOKEN", "CLOB_API_KEY",
            "CLOB_SECRET", "CLOB_PASSPHRASE"};
        for (const char* base : kSecrets) {
            const size_t base_length = std::strlen(base);
            if (length == base_length &&
                std::memcmp(name, base, base_length) == 0)
                return true;
            if (length == base_length + 5 &&
                std::memcmp(name, base, base_length) == 0 &&
                std::memcmp(name + base_length, "_FILE", 5) == 0)
                return true;
        }
        return false;
    }

    // A configuration-looking name that this build does not read is an error:
    // a typo (BOT_MAX_ORDER_USDD) or a variable removed from the code must be
    // visible at startup, not silently ignored.
    const char* check_environment_names() {
#if defined(__unix__) || defined(__APPLE__)
        char offenders[3][64]{};
        size_t collected = 0;
        size_t total = 0;
        for (char** entry = environ; entry && *entry; ++entry) {
            const char* name = *entry;
            const char* equals = std::strchr(name, '=');
            const size_t length =
                equals ? static_cast<size_t>(equals - name) : std::strlen(name);
            if (!is_config_prefix(name, length)) continue;
            if (known_env_name(name, length)) continue;
            ++total;
            if (collected < 3) {
                const size_t copy = length < sizeof(offenders[0]) - 1
                                        ? length : sizeof(offenders[0]) - 1;
                std::memcpy(offenders[collected], name, copy);
                offenders[collected][copy] = '\0';
                ++collected;
            }
        }
        if (total == 1)
            return failf("unknown configuration variable %s: this build rejects "
                         "names it does not read, see docs/CONFIGURATION.md",
                         offenders[0]);
        if (total == 2)
            return failf("unknown configuration variables %s and %s: this build "
                         "rejects names it does not read, see "
                         "docs/CONFIGURATION.md", offenders[0], offenders[1]);
        if (total > 2)
            return failf("unknown configuration variables %s, %s and %zu more: "
                         "this build rejects names it does not read, see "
                         "docs/CONFIGURATION.md", offenders[0], offenders[1],
                         total - 2);
#endif
        return nullptr;
    }

    static bool secret_present(const char* key) noexcept {
        const char* value = std::getenv(key);
        if (value && *value) return true;
        char file_key[96];
        std::snprintf(file_key, sizeof(file_key), "%s_FILE", key);
        const char* path = std::getenv(file_key);
        return path && *path;
    }

    // Present => must be readable; required => must be present.
    template <size_t N>
    static bool load_secret(bool required, const char* key, char (&dst)[N]) {
        const bool present = secret_present(key);
        const bool read = read_secret(key, dst, N);
        if (read) return true;
        dst[0] = '\0';
        return !(required || present);
    }

    static bool valid_market_slug(const char* slug) noexcept {
        if (!slug || !*slug) return false;
        for (const unsigned char* p =
                 reinterpret_cast<const unsigned char*>(slug); *p; ++p) {
            const bool allowed = (*p >= 'a' && *p <= 'z') ||
                (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                *p == '-' || *p == '_' || *p == '.';
            if (!allowed) return false;
        }
        return true;
    }

    static bool valid_secure_url(const char* url, const char* scheme,
                                 bool allow_path) noexcept {
        if (!url || !scheme) return false;
        const size_t scheme_len = std::strlen(scheme);
        if (std::strncmp(url, scheme, scheme_len) != 0) return false;
        const char* authority = url + scheme_len;
        const char* slash = std::strchr(authority, '/');
        const char* authority_end = slash ? slash : authority + std::strlen(authority);
        if (authority == authority_end || (slash && !allow_path)) return false;
        for (const char* p = authority; p < authority_end; ++p)
            if (*p == '@' || *p == '?' || *p == '#' || *p == ' ')
                return false;
        if (slash && (std::strchr(slash, '#') || std::strchr(slash, ' ')))
            return false;
        return true;
    }

    static bool valid_tls_pins(const char* pins) noexcept {
        size_t count = 0;
        const char* cursor = pins;
        while (*cursor) {
            const char* separator = std::strchr(cursor, ';');
            const size_t length = separator
                ? static_cast<size_t>(separator - cursor) : std::strlen(cursor);
            if (++count > 2 || length != 52 || cursor[51] != '=' ||
                std::memcmp(cursor, "sha256//", 8) != 0)
                return false;
            for (size_t i = 8; i < 52; ++i) {
                const char c = cursor[i];
                const bool base64 = (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '+' || c == '/' || (c == '=' && i == 51);
                if (!base64) return false;
            }
            if (!separator) break;
            cursor = separator + 1;
            if (!*cursor) return false;
        }
        return count != 0;
    }

    static bool safe_config_text(const char* value) noexcept {
        if (!value) return false;
        for (const unsigned char* p =
                 reinterpret_cast<const unsigned char*>(value); *p; ++p)
            if (*p < 0x20U || *p == 0x7fU) return false;
        return true;
    }

    static bool printable_secret(const char* value) noexcept {
        if (!value || !*value) return false;
        for (const unsigned char* p =
                 reinterpret_cast<const unsigned char*>(value); *p; ++p)
            if (*p <= 0x20U || *p == 0x7fU) return false;
        return true;
    }

    static bool read_secret(const char* key, char* out, size_t cap) {
        if (!out || cap < 2) return false;
        out[0] = '\0';
        char file_key[96];
        std::snprintf(file_key, sizeof(file_key), "%s_FILE", key);
        const char* value = std::getenv(key);
        const char* path = std::getenv(file_key);
        if (value && *value && path && *path) {
            unsetenv(key);
            return false;  // reject ambiguous secret sources
        }
        if (value && *value) {
            const size_t length = std::strlen(value);
            if (length >= cap) {
                unsetenv(key);
                return false;
            }
            std::memcpy(out, value, length + 1);
            unsetenv(key);
            return true;
        }
        if (!path || !*path) return false;
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        struct stat metadata{};
        if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            (metadata.st_mode & S_IRUSR) == 0 ||
            (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            ::close(fd);
            return false;
        }
        char buffer[512];
        size_t total = 0;
        bool io_ok = true;
        while (total < sizeof(buffer) - 1) {
            const ssize_t amount = ::read(fd, buffer + total,
                                          sizeof(buffer) - 1 - total);
            if (amount < 0) {
                if (errno == EINTR) continue;
                io_ok = false;
                break;
            }
            if (amount == 0) break;
            total += static_cast<size_t>(amount);
        }
        char extra = 0;
        ssize_t extra_read;
        do { extra_read = ::read(fd, &extra, 1); }
        while (extra_read < 0 && errno == EINTR);
        const bool fits = io_ok && extra_read == 0;
        ::close(fd);
        if (!fits || total == 0) {
            secure_zero(buffer, sizeof(buffer));
            return false;
        }
        size_t length = total;
        while (length && (buffer[length - 1] == '\n' ||
                          buffer[length - 1] == '\r'))
            --length;
        if (length == 0 || length >= cap) {
            secure_zero(buffer, sizeof(buffer));
            return false;
        }
        std::memcpy(out, buffer, length);
        out[length] = '\0';
        secure_zero(buffer, sizeof(buffer));
        unsetenv(file_key);
        return true;
    }

    template <size_t N>
    static bool copy_env(char (&dst)[N], const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        const char* source = value && *value ? value : fallback;
        if (!source || source == dst) return true;
        if (std::strlen(source) >= N) return false;
        std::memcpy(dst, source, std::strlen(source) + 1);
        return true;
    }

    static void format_address(const uint8_t in[20], char out[43]) {
        out[0] = '0'; out[1] = 'x';
        bytes_to_hex(in, 20, out + 2);
        out[42] = '\0';
    }
};

#endif  // MARKET_CONFIG_HPP
