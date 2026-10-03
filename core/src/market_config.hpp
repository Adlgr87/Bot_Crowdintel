#ifndef MARKET_CONFIG_HPP
#define MARKET_CONFIG_HPP

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// environ is used to reject unknown prefixed configuration variables.
extern char** environ;

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"
#include "polymarket_order.hpp"
#include "alpha_receiver.hpp"

// Every environment variable this binary reads.  A variable with one of our
// prefixes that is not in this list is a configuration the process would
// silently ignore, which strict mode rejects (Phase 7, option B: the file at
// /etc/crowdintel/config is consumed as a systemd EnvironmentFile, so an
// ignored key must be a hard error rather than a surprise).
inline constexpr const char* K_CONFIG_PREFIXES[] = {
    "BOT_", "CLOB_", "POLY_", "WS_", "POLYGON_", "GAMMA_", "CROWDINTEL_"};

inline constexpr const char* K_CONFIG_KEYS[] = {
    "BOT_ALPHA_BEARER_TOKEN", "BOT_ALPHA_BEARER_TOKEN_FILE", "BOT_ALPHA_BIND",
    "BOT_ALPHA_PORT", "BOT_ALLOW_PROTOCOL_V2", "BOT_API_ADDRESS",
    "BOT_BANKROLL_USD", "BOT_COLD_CPU", "BOT_CONDITION_ID",
    "BOT_ENABLE_LIVE_TRADING", "BOT_GTD_TTL_SECONDS",
    "BOT_HEARTBEAT_ASSUME_CANCELLED_MS", "BOT_HEARTBEAT_BLOCK_MS",
    "BOT_HEARTBEAT_ENABLED", "BOT_HEARTBEAT_INTERVAL_MS",
    "BOT_HEARTBEAT_MAX_FAILURES", "BOT_HEARTBEAT_WARN_MS",
    "BOT_INITIAL_POSITION_SHARES", "BOT_KELLY_FRACTION", "BOT_KILL_SWITCH_FILE",
    "BOT_LEDGER_CHECKPOINT_EVERY", "BOT_LEDGER_DIR", "BOT_LEDGER_FSYNC",
    "BOT_MAKER_ADDRESS", "BOT_MARKET_SLUG", "BOT_MAX_BOOK_AGE_MS",
    "BOT_MAX_CLOCK_SKEW_S", "BOT_MAX_DAILY_LOSS_USD", "BOT_MAX_EXPOSURE_USD",
    "BOT_MAX_ORDER_USD", "BOT_MAX_Q_VALUE", "BOT_MAX_TAKER_FEE_RATE",
    "BOT_METADATA_MAX_AGE_MS", "BOT_METADATA_REFRESH_MS", "BOT_MIN_COLLATERAL",
    "BOT_MIN_CONFIDENCE", "BOT_MIN_EDGE", "BOT_MIN_SIZE_SHARES", "BOT_MODE",
    "BOT_NEG_RISK", "BOT_ORDER_TYPE", "BOT_PIN_CPU",
    "BOT_PREFLIGHT_CHECK_HEARTBEAT", "BOT_PREFLIGHT_CHECK_L1",
    "BOT_PREFLIGHT_CHECK_USER_WS", "BOT_PREFLIGHT_MAX_AGE_S",
    "BOT_PREFLIGHT_TOKEN_FILE", "BOT_PRESIGN_TTL_MS", "BOT_PRIVATE_KEY_HEX",
    "BOT_PRIVATE_KEY_HEX_FILE", "BOT_RECON_MAX_PAGES",
    "BOT_SIGNAL_TTL_MS", "BOT_SESSION_TIMEOUT_MS", "BOT_SIGNATURE_TYPE", "BOT_STRICT_ENV",
    "BOT_TAKER_FEE_RATE",
    "BOT_TARGET_ALLOWANCE", "BOT_TICKS", "BOT_TICK_SIZE", "BOT_TLS_PIN",
    "BOT_TOKEN_ID", "BOT_USER_WS_ENABLED", "BOT_USER_WS_HOST",
    "BOT_USER_WS_IDLE_MS", "BOT_USER_WS_KEEPALIVE_MS", "BOT_USER_WS_PONG_MS",
    "BOT_USER_WS_RECONNECT_MAX_MS", "BOT_USER_WS_RECONNECT_MIN_MS",
    "CLOB_API_KEY", "CLOB_API_KEY_FILE", "CLOB_HOST", "CLOB_PASSPHRASE",
    "CLOB_PASSPHRASE_FILE", "CLOB_SECRET", "CLOB_SECRET_FILE",
    "CROWDINTEL_FORCE_MOCK", "GAMMA_HOST", "POLYGON_RPC_BACKUP_URL",
    "POLYGON_RPC_URL", "WS_HOST"};

inline constexpr size_t K_CONFIG_KEY_COUNT =
    sizeof(K_CONFIG_KEYS) / sizeof(K_CONFIG_KEYS[0]);

// Keys that must never appear in a configuration file: they belong in the
// credential store (/etc/crowdintel/credentials/*, systemd LoadCredential) and
// are read through the *_FILE indirection.
inline constexpr const char* K_SECRET_KEYS[] = {
    "BOT_PRIVATE_KEY_HEX", "CLOB_SECRET", "CLOB_PASSPHRASE", "CLOB_API_KEY",
    "BOT_ALPHA_BEARER_TOKEN"};

inline bool is_known_config_key(const char* key) noexcept {
    if (!key) return false;
    for (size_t i = 0; i < K_CONFIG_KEY_COUNT; ++i)
        if (std::strcmp(K_CONFIG_KEYS[i], key) == 0) return true;
    return false;
}

inline bool is_secret_config_key(const char* key) noexcept {
    if (!key) return false;
    for (const char* candidate : K_SECRET_KEYS)
        if (std::strcmp(candidate, key) == 0) return true;
    return false;
}

inline bool has_config_prefix(const char* key) noexcept {
    if (!key) return false;
    for (const char* prefix : K_CONFIG_PREFIXES) {
        const size_t length = std::strlen(prefix);
        if (std::strncmp(key, prefix, length) == 0) return true;
    }
    return false;
}

// Operating mode.  Kept explicit and never inferred from other settings: the
// transport, the fill source and the preflight requirement all follow from it.
enum class BotMode : uint8_t { REPLAY = 0, PAPER = 1, LIVE = 2 };

inline const char* bot_mode_name(BotMode mode) noexcept {
    switch (mode) {
        case BotMode::REPLAY: return "replay";
        case BotMode::PAPER: return "paper";
        case BotMode::LIVE: return "live";
    }
    return "invalid";
}

struct MarketConfig {
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
    char token_id_dec[80]{};
    uint8_t token_id_be[32]{};
    char market_slug[96] = "mock-market";
    uint64_t market_hash = 0;
    uint64_t tick_size = 10000;
    bool neg_risk = false;
    uint8_t signature_type = 0;
    char order_type[8] = "FAK";
    uint64_t gtd_ttl_seconds = 0;
    char clob_host[128] = "https://clob.polymarket.com";
    char ws_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/market";

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
    bool mock_mode = false;   // true for replay/paper (no venue transport)
    bool live_armed = false;
    char tls_pin[128]{};
    char kill_switch_file[192] = "/tmp/crowdintel.kill";

    // ── Operating mode ──────────────────────────────────────────────────────
    // Three distinct modes, never inferred:
    //   REPLAY – offline; reads recorded ticks, no venue transport at all
    //   PAPER  – live *market data*, no order egress, simulated fills
    //   LIVE   – real orders; requires a fresh preflight pass token
    BotMode bot_mode = BotMode::PAPER;

    // ── Venue metadata (Phase 1) ────────────────────────────────────────────
    // Nothing the venue can change is taken from the environment in live mode:
    // tick size, minimum order size, negative-risk flag, fee schedule and
    // market status are fetched and validated before trading is enabled.  The
    // values below are *expectations* used to cross-check the venue in live
    // mode and as fixtures in replay/paper mode.
    char condition_id[70]{};           // optional; resolved from the slug
    char gamma_host[128] = "https://gamma-api.polymarket.com";
    char user_ws_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/user";
    char polygon_rpc_url[192]{};
    char polygon_rpc_backup_url[192]{};
    // The refresh period must be short enough that the age guard below never
    // trips in steady state: validate() requires max_age >= 2 * refresh.
    uint64_t metadata_max_age_ms = 90000;
    uint64_t metadata_refresh_ms = 30000;
    bool allow_protocol_v2_positions = false;
    double max_taker_fee_rate = 0.07;  // highest documented category rate
    bool token_id_from_metadata = true;

    // ── Persistent ledger (Phase 2) ─────────────────────────────────────────
    char ledger_dir[192] = "/var/lib/crowdintel";
    bool ledger_fsync = true;
    uint64_t ledger_checkpoint_every = 512;

    // ── User channel (Phase 3) ──────────────────────────────────────────────
    bool user_ws_enabled = true;
    uint32_t user_ws_keepalive_ms = 8000;
    uint32_t user_ws_idle_ms = 20000;
    uint32_t user_ws_pong_ms = 30000;
    uint32_t user_ws_reconnect_min_ms = 250;
    uint32_t user_ws_reconnect_max_ms = 5000;

    // ── Order heartbeat (Phase 4) ───────────────────────────────────────────
    // Opt-in venue contract: once started, the venue cancels every open order
    // owned by these credentials if a valid heartbeat is missing for 10 s
    // (checked every 5 s).  Requires dedicated credentials.
    bool heartbeat_enabled = false;
    uint32_t heartbeat_interval_ms = 5000;
    uint32_t heartbeat_warn_ms = 7000;
    uint32_t heartbeat_block_ms = 9000;
    uint32_t heartbeat_assume_cancelled_ms = 10000;
    uint32_t heartbeat_max_failures = 2;

    // ── Reconciliation and readiness (Phase 5) ──────────────────────────────
    uint64_t recon_max_pages = 4;

    // ── Preflight (Phase 6) ─────────────────────────────────────────────────
    char preflight_token_file[192] = "/run/crowdintel/preflight.pass";
    uint64_t preflight_max_age_s = 900;
    uint64_t max_clock_skew_s = 10;
    uint64_t min_collateral_base = 1000000;   // 1 pUSD (1e6 base units)
    uint64_t target_allowance_base = 0;       // 0 → derived from exposure caps
    bool preflight_check_l1 = false;
    bool preflight_check_user_ws = false;
    bool preflight_check_heartbeat = false;

    // Bounded session length (0 = run until signalled).  Used by smoke tests and
    // canary rehearsals so a run without alpha signals still terminates.
    uint64_t session_timeout_ms = 0;

    // ── Configuration hygiene (Phase 7) ─────────────────────────────────────
    bool strict_env = true;   // reject unknown BOT_*/CLOB_*/POLY_*/WS_* vars
    char config_fingerprint[65]{};

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

    const char* load(bool need_trading_creds, bool force_mock = false) {
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

        long neg_risk_l = 0, sig = 0, presign_l = 0, signal_l = 0;
        long book_age_l = 0, pin_l = -1, cold_l = -1, ticks_l = 0;
        long armed_l = 0, gtd_l = 0;
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
            !env_l("BOT_GTD_TTL_SECONDS", 0, gtd_l))
            return "invalid numeric configuration value";

        if ((neg_risk_l != 0 && neg_risk_l != 1) ||
            (armed_l != 0 && armed_l != 1))
            return "BOT_NEG_RISK and BOT_ENABLE_LIVE_TRADING must be 0 or 1";
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
            pin_l < -1 || cold_l < -1 || pin_l > INT_MAX || cold_l > INT_MAX)
            return "runtime duration/CPU configuration is out of range";

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
        const char* mode = std::getenv("BOT_MODE");
        if (!mode || !*mode) {
            bot_mode = BotMode::PAPER;
        } else if (std::strcmp(mode, "replay") == 0) {
            bot_mode = BotMode::REPLAY;
        } else if (std::strcmp(mode, "paper") == 0) {
            bot_mode = BotMode::PAPER;
        } else if (std::strcmp(mode, "live") == 0) {
            bot_mode = BotMode::LIVE;
        } else if (std::strcmp(mode, "mock") == 0) {
            // Legacy spelling kept so existing CI/deploy scripts keep working.
            bot_mode = BotMode::PAPER;
            std::fprintf(stderr,
                         "WARNING: BOT_MODE=mock is deprecated; use paper\n");
        } else {
            return "BOT_MODE must be replay, paper or live";
        }
        if (force_mock && bot_mode == BotMode::LIVE)
            return "CROWDINTEL_FORCE_MOCK cannot be combined with BOT_MODE=live";
        if (force_mock) bot_mode = BotMode::PAPER;
        mock_mode = bot_mode != BotMode::LIVE;
        live_armed = armed_l == 1;
        gtd_ttl_seconds = static_cast<uint64_t>(gtd_l);

        if (!copy_env(order_type, "BOT_ORDER_TYPE", order_type) ||
            !copy_env(clob_host, "CLOB_HOST", clob_host) ||
            !copy_env(ws_host, "WS_HOST", ws_host) ||
            !copy_env(tls_pin, "BOT_TLS_PIN", "") ||
            !copy_env(kill_switch_file, "BOT_KILL_SWITCH_FILE",
                      kill_switch_file) ||
            !copy_env(market_slug, "BOT_MARKET_SLUG",
                      mock_mode ? "mock-market" : "") ||
            !copy_env(alpha_bind, "BOT_ALPHA_BIND", alpha_bind))
            return "configuration string exceeds its bounded capacity";
        if (!safe_config_text(order_type) || !safe_config_text(clob_host) ||
            !safe_config_text(ws_host) || !safe_config_text(tls_pin) ||
            !safe_config_text(kill_switch_file) ||
            !safe_config_text(market_slug) || !safe_config_text(alpha_bind))
            return "configuration strings cannot contain control characters";
        if (tls_pin[0] && !valid_tls_pins(tls_pin))
            return "BOT_TLS_PIN must contain one or two sha256// pins separated by a semicolon";
        long alpha_port_l = alpha_port;
        if (!env_l("BOT_ALPHA_PORT", alpha_port, alpha_port_l) ||
            alpha_port_l < 1 || alpha_port_l > 65535)
            return "BOT_ALPHA_PORT is invalid";
        alpha_port = static_cast<uint16_t>(alpha_port_l);
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
        const double scaled_tick = tick * 1000000.0;
        if (std::fabs(scaled_tick - std::round(scaled_tick)) > 1e-6 ||
            tick_size == 0 || tick_size >= 1000000 ||
            1000000 % tick_size != 0 || amount_quantum_for_tick(tick_size) == 0)
            return "BOT_TICK_SIZE is not a supported exact V2 tick";
        if (bankroll_usd <= 0 || kelly_fraction <= 0 || kelly_fraction > 1 ||
            min_edge < 0 || min_edge >= 1 || min_confidence < 0 ||
            min_confidence > 1 || max_q_value < 0 || max_q_value > 1 ||
            taker_fee_rate < 0 || taker_fee_rate > 1 || max_order_usd <= 0 ||
            max_exposure_usd <= 0 || max_daily_loss_usd <= 0)
            return "invalid strategy/risk configuration";
        if (!valid_market_slug(market_slug))
            return "BOT_MARKET_SLUG must be a non-empty URL-safe slug";
        if (!mock_mode && !valid_secure_url(clob_host, "https://", false))
            return "CLOB_HOST must be an origin-only https:// URL in live mode";
        if (!mock_mode && !valid_secure_url(ws_host, "wss://", true))
            return "WS_HOST must be a valid wss:// URL in live mode";

        if (need_trading_creds) {
            if (!live_armed)
                return "live mode is disarmed; set BOT_ENABLE_LIVE_TRADING=1 after completing the preflight checklist";
            if (!read_secret("CLOB_API_KEY", owner_api_key, sizeof(owner_api_key)) ||
                !read_secret("CLOB_SECRET", api_secret_b64, sizeof(api_secret_b64)) ||
                !read_secret("CLOB_PASSPHRASE", api_passphrase,
                             sizeof(api_passphrase)))
                return "CLOB_API_KEY/SECRET/PASSPHRASE (or *_FILE) are required";
            if (!printable_secret(owner_api_key) ||
                !printable_secret(api_secret_b64) ||
                !printable_secret(api_passphrase) ||
                std::strchr(owner_api_key, '"') ||
                std::strchr(owner_api_key, '\\'))
                return "CLOB credentials contain unsafe whitespace/control/JSON characters";
            uint8_t decoded_secret[64]{};
            const size_t decoded_secret_len = base64url_decode(
                api_secret_b64, std::strlen(api_secret_b64), decoded_secret,
                sizeof(decoded_secret));
            secure_zero(decoded_secret, sizeof(decoded_secret));
            if (decoded_secret_len == 0 || decoded_secret_len == SIZE_MAX)
                return "CLOB_SECRET is not valid bounded base64/base64url";
            if (!read_secret("BOT_ALPHA_BEARER_TOKEN", alpha_bearer_token,
                             sizeof(alpha_bearer_token)) ||
                std::strlen(alpha_bearer_token) < 16 ||
                !printable_secret(alpha_bearer_token))
                return "BOT_ALPHA_BEARER_TOKEN (or *_FILE) must contain at least 16 printable chars";
        } else {
            std::snprintf(owner_api_key, sizeof(owner_api_key),
                          "00000000-0000-0000-0000-000000000000");
        }

        if (const char* live_ops_error = load_live_ops()) return live_ops_error;

        // No hardcoded token id in any mode that can reach the venue.  LIVE and
        // PAPER must either be given BOT_TOKEN_ID explicitly (and then it is
        // cross-checked against venue metadata) or resolve it from the market's
        // metadata at startup; `token_id_from_metadata` records which of the two
        // happened and the startup banner prints it.  Only REPLAY - which by
        // definition has no venue to ask - falls back to the documented test
        // vector so recorded sessions stay runnable offline.  Giving paper the
        // vector too meant a rehearsal without BOT_TOKEN_ID silently skipped
        // identity resolution and ran on a fixed operational value.
        const char* tok = env("BOT_TOKEN_ID",
            bot_mode == BotMode::REPLAY
                ? "71321045679252212594626395510336467040167069592778062791519851593659551227755"
                : "");
        const size_t token_len = std::strlen(tok);
        if (token_len == 0) {
            // Resolved from metadata at startup (Phase 1); live refuses to
            // trade until that resolution succeeds and validates.
            token_id_dec[0] = '\0';
            std::memset(token_id_be, 0, sizeof(token_id_be));
            token_id_from_metadata = true;
            return nullptr;
        }
        token_id_from_metadata = false;
        if (token_len >= sizeof(token_id_dec) ||
            (token_len > 1 && tok[0] == '0') ||
            !parse_uint256_dec(tok, token_len, token_id_be))
            return "BOT_TOKEN_ID is not a canonical uint256 decimal";
        bool token_nonzero = false;
        for (const uint8_t byte : token_id_be) token_nonzero |= byte != 0;
        if (!token_nonzero) return "BOT_TOKEN_ID cannot be zero";
        std::memcpy(token_id_dec, tok, token_len + 1);
        return nullptr;
    }

    // Provenance of the outcome token id, for logs and for the config tool:
    // pinned by the operator, resolved from venue metadata, or the built-in
    // REPLAY vector.  `token_id_from_metadata` is decided at load time and is not
    // rewritten when main later resolves the id, so this stays truthful.
    const char* token_id_source() const noexcept {
        if (token_id_from_metadata) return "venue-metadata";
        return env("BOT_TOKEN_ID", "")[0] ? "BOT_TOKEN_ID" : "replay-test-vector";
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

    // ── Live-operations configuration (Phases 1-7) ─────────────────────────
    //
    // Parsed after the legacy strategy/transport block so a failure here cannot
    // leave a half-configured object in use.  Live mode additionally refuses
    // values that the venue can change, and refuses unknown prefixed
    // environment variables so a config file the binary ignores is a hard error
    // instead of a silent surprise.
    const char* load_live_ops() {
        if (!copy_env(gamma_host, "GAMMA_HOST", gamma_host) ||
            !copy_env(user_ws_host, "BOT_USER_WS_HOST", user_ws_host) ||
            !copy_env(polygon_rpc_url, "POLYGON_RPC_URL", "") ||
            !copy_env(polygon_rpc_backup_url, "POLYGON_RPC_BACKUP_URL", "") ||
            !copy_env(ledger_dir, "BOT_LEDGER_DIR", ledger_dir) ||
            !copy_env(preflight_token_file, "BOT_PREFLIGHT_TOKEN_FILE",
                      preflight_token_file) ||
            !copy_env(condition_id, "BOT_CONDITION_ID", ""))
            return "live-operations configuration string exceeds its bound";
        for (const char* value : {gamma_host, user_ws_host, polygon_rpc_url,
                                  polygon_rpc_backup_url, ledger_dir,
                                  preflight_token_file, condition_id})
            if (!safe_config_text(value))
                return "live-operations configuration contains control characters";

        long metadata_age_l = 0, metadata_refresh_l = 0, ledger_fsync_l = 1;
        long checkpoint_l = 0, recon_pages_l = 0, preflight_age_l = 0;
        long skew_l = 0, user_ws_enabled_l = 1, strict_l = 1;
        long keepalive_l = 0, idle_l = 0, pong_l = 0, reconnect_min_l = 0;
        long reconnect_max_l = 0, heartbeat_enabled_l = 0, interval_l = 0;
        long warn_l = 0, block_l = 0, assume_l = 0, failures_l = 0;
        long allow_v2_l = 0, check_l1_l = 0;
        long check_ws_l = 0, check_hb_l = 0, session_l = 0;
        double max_fee = 0.0;
        double min_collateral = 0.0;
        double target_allowance = 0.0;
        if (!env_l("BOT_METADATA_MAX_AGE_MS", 90000, metadata_age_l) ||
            !env_l("BOT_METADATA_REFRESH_MS", 30000, metadata_refresh_l) ||
            !env_l("BOT_LEDGER_FSYNC", 1, ledger_fsync_l) ||
            !env_l("BOT_LEDGER_CHECKPOINT_EVERY", 512, checkpoint_l) ||
            !env_l("BOT_RECON_MAX_PAGES", 4, recon_pages_l) ||
            !env_l("BOT_PREFLIGHT_MAX_AGE_S", 900, preflight_age_l) ||
            !env_l("BOT_MAX_CLOCK_SKEW_S", 10, skew_l) ||
            !env_l("BOT_USER_WS_ENABLED", 1, user_ws_enabled_l) ||
            !env_l("BOT_STRICT_ENV", 1, strict_l) ||
            !env_l("BOT_USER_WS_KEEPALIVE_MS", 8000, keepalive_l) ||
            !env_l("BOT_USER_WS_IDLE_MS", 20000, idle_l) ||
            !env_l("BOT_USER_WS_PONG_MS", 30000, pong_l) ||
            !env_l("BOT_USER_WS_RECONNECT_MIN_MS", 250, reconnect_min_l) ||
            !env_l("BOT_USER_WS_RECONNECT_MAX_MS", 5000, reconnect_max_l) ||
            !env_l("BOT_HEARTBEAT_ENABLED", 0, heartbeat_enabled_l) ||
            !env_l("BOT_HEARTBEAT_INTERVAL_MS", 5000, interval_l) ||
            !env_l("BOT_HEARTBEAT_WARN_MS", 7000, warn_l) ||
            !env_l("BOT_HEARTBEAT_BLOCK_MS", 9000, block_l) ||
            !env_l("BOT_HEARTBEAT_ASSUME_CANCELLED_MS", 10000, assume_l) ||
            !env_l("BOT_HEARTBEAT_MAX_FAILURES", 2, failures_l) ||
            !env_l("BOT_ALLOW_PROTOCOL_V2", 0, allow_v2_l) ||
            !env_l("BOT_PREFLIGHT_CHECK_L1", 0, check_l1_l) ||
            !env_l("BOT_PREFLIGHT_CHECK_USER_WS", 0, check_ws_l) ||
            !env_l("BOT_PREFLIGHT_CHECK_HEARTBEAT", 0, check_hb_l) ||
            !env_l("BOT_SESSION_TIMEOUT_MS", 0, session_l) ||
            !env_d("BOT_MAX_TAKER_FEE_RATE", max_taker_fee_rate, max_fee) ||
            !env_d("BOT_MIN_COLLATERAL", 1.0, min_collateral) ||
            !env_d("BOT_TARGET_ALLOWANCE", 0.0, target_allowance))
            return "invalid live-operations numeric configuration";

        for (const long flag : {ledger_fsync_l, user_ws_enabled_l, strict_l,
                                heartbeat_enabled_l, allow_v2_l,
                                check_l1_l, check_ws_l, check_hb_l})
            if (flag != 0 && flag != 1)
                return "live-operations boolean flags must be 0 or 1";
        if (metadata_age_l < 1000 || metadata_age_l > 3600000 ||
            metadata_refresh_l < 5000 || metadata_refresh_l > 3600000 ||
            checkpoint_l < 1 || checkpoint_l > 1000000 ||
            recon_pages_l < 1 || recon_pages_l > 64 ||
            preflight_age_l < 10 || preflight_age_l > 86400 ||
            skew_l < 1 || skew_l > 60)
            return "live-operations durations are out of range";
        // BOT_METADATA_MAX_AGE_MS is enforced by the supervisor: venue metadata
        // older than that blocks egress until a refresh republishes it.  A
        // refresh period longer than half the age budget would block trading
        // between refreshes by construction, so the pair is rejected.
        if (metadata_age_l < metadata_refresh_l * 2)
            return "BOT_METADATA_MAX_AGE_MS must be at least twice "
                   "BOT_METADATA_REFRESH_MS: the age guard would otherwise block "
                   "trading between two scheduled refreshes";
        // User channel: keepalive < idle < pong, keepalive within the documented
        // 10 s application-level cadence.
        if (keepalive_l < 1000 || keepalive_l > 10000 ||
            idle_l <= keepalive_l || idle_l > 120000 ||
            pong_l <= idle_l || pong_l > 300000 ||
            reconnect_min_l < 50 || reconnect_min_l > reconnect_max_l ||
            reconnect_max_l > 60000)
            return "BOT_USER_WS_* timing configuration is out of range";
        // Heartbeat: the venue cancels resting orders 10 s after the last valid
        // heartbeat and checks every 5 s, so the local block threshold must stay
        // below 10 s and the assume-cancelled threshold at or below the same
        // 10 s: the venue may cancel from that instant, so waiting for its
        // documented 5 s check cadence (~15 s worst case) would leave us
        // believing in orders the venue is already entitled to kill.
        // Venue contract: a valid heartbeat must arrive every 10 s or every open
        // order owned by these credentials is cancelled (py-clob-client
        // client.py:715, clob-client client.ts:1144, docs
        // trading/manage-orders#order-heartbeats).  Local thresholds must
        // therefore block before 10 s and treat orders as possibly cancelled from
        // 10 s onwards.  heartbeat::Config::validate() re-checks the full set,
        // including the request timeout margin, before the contract is started.
        if (interval_l < 1000 || interval_l > 6000 || warn_l <= interval_l ||
            block_l <= warn_l || block_l > 10000 || assume_l < block_l ||
            assume_l > 10000 || failures_l < 1 || failures_l > 8)
            return "BOT_HEARTBEAT_* thresholds are out of range (venue cancels at 10 s)";
        if (max_fee < 0.0 || max_fee > 0.2 || min_collateral < 0.0 ||
            target_allowance < 0.0)
            return "live-operations fee/collateral configuration is out of range";

        metadata_max_age_ms = static_cast<uint64_t>(metadata_age_l);
        metadata_refresh_ms = static_cast<uint64_t>(metadata_refresh_l);
        ledger_fsync = ledger_fsync_l == 1;
        ledger_checkpoint_every = static_cast<uint64_t>(checkpoint_l);
        recon_max_pages = static_cast<uint64_t>(recon_pages_l);
        preflight_max_age_s = static_cast<uint64_t>(preflight_age_l);
        max_clock_skew_s = static_cast<uint64_t>(skew_l);
        user_ws_enabled = user_ws_enabled_l == 1;
        strict_env = strict_l == 1;
        user_ws_keepalive_ms = static_cast<uint32_t>(keepalive_l);
        user_ws_idle_ms = static_cast<uint32_t>(idle_l);
        user_ws_pong_ms = static_cast<uint32_t>(pong_l);
        user_ws_reconnect_min_ms = static_cast<uint32_t>(reconnect_min_l);
        user_ws_reconnect_max_ms = static_cast<uint32_t>(reconnect_max_l);
        heartbeat_enabled = heartbeat_enabled_l == 1;
        heartbeat_interval_ms = static_cast<uint32_t>(interval_l);
        heartbeat_warn_ms = static_cast<uint32_t>(warn_l);
        heartbeat_block_ms = static_cast<uint32_t>(block_l);
        heartbeat_assume_cancelled_ms = static_cast<uint32_t>(assume_l);
        heartbeat_max_failures = static_cast<uint32_t>(failures_l);
        allow_protocol_v2_positions = allow_v2_l == 1;
        preflight_check_l1 = check_l1_l == 1;
        preflight_check_user_ws = check_ws_l == 1;
        preflight_check_heartbeat = check_hb_l == 1;
        if (session_l < 0 || session_l > 86400000)
            return "BOT_SESSION_TIMEOUT_MS must be within 0..86400000 ms";
        session_timeout_ms = static_cast<uint64_t>(session_l);
        max_taker_fee_rate = max_fee;
        if (min_collateral > 1e12 || min_collateral * 1e6 > static_cast<double>(UINT64_MAX))
            return "BOT_MIN_COLLATERAL is out of range";
        min_collateral_base = static_cast<uint64_t>(min_collateral * 1000000.0);
        target_allowance_base = static_cast<uint64_t>(target_allowance * 1000000.0);

        if (bot_mode == BotMode::LIVE) {
            if (!polygon_rpc_url[0])
                return "POLYGON_RPC_URL is required in live mode (chain id must be "
                       "verified before any constant is trusted)";
            if (std::strncmp(polygon_rpc_url, "https://", 8) != 0)
                return "POLYGON_RPC_URL must be an https:// URL";
            if (polygon_rpc_backup_url[0] &&
                std::strncmp(polygon_rpc_backup_url, "https://", 8) != 0)
                return "POLYGON_RPC_BACKUP_URL must be an https:// URL";
            if (!valid_secure_url(gamma_host, "https://", false))
                return "GAMMA_HOST must be an origin-only https:// URL";
            if (std::strncmp(user_ws_host, "wss://", 6) != 0)
                return "BOT_USER_WS_HOST must be a wss:// URL";
            if (ledger_dir[0] != '/')
                return "BOT_LEDGER_DIR must be an absolute path";
            if (initial_position_shares != 0)
                return "BOT_INITIAL_POSITION_SHARES must be 0 in live mode: inventory "
                       "comes from reconciliation, never from configuration";
            if (!user_ws_enabled)
                return "BOT_USER_WS_ENABLED=0 is not permitted in live mode: without "
                       "the user channel there is no fill visibility";
            if (heartbeat_enabled && heartbeat_interval_ms > 6000)
                return "heartbeat interval must not exceed 6 s in live mode";
        }
        if (condition_id[0] &&
            !(std::strlen(condition_id) == 66 && condition_id[0] == '0' &&
              (condition_id[1] == 'x' || condition_id[1] == 'X')))
            return "BOT_CONDITION_ID must be a 0x-prefixed 32-byte hex value";
        if (target_allowance_base == 0)
            target_allowance_base = derived_target_allowance();
        if (strict_env) {
            const char* unknown = unknown_prefixed_env();
            if (unknown) return unknown;
        }
        compute_config_fingerprint();
        return nullptr;
    }

    // Allowance target = exposure limit + operating margin.  Never max_uint256:
    // an unlimited approval turns any exchange-contract bug into a total loss of
    // the wallet balance.
    uint64_t derived_target_allowance() const noexcept {
        const double exposure = max_exposure_usd > 0 ? max_exposure_usd : 0.0;
        const double with_margin = exposure * 1.10 + 1.0;  // +10 % and 1 pUSD
        if (with_margin <= 0 || with_margin > 1e12) return 0;
        return static_cast<uint64_t>(with_margin * 1000000.0);
    }

    // SHA-256 over the effective non-secret configuration.  Logged and written
    // into the preflight token so an operator can prove which configuration a
    // pass/fail verdict belonged to.  Secrets are excluded by construction.
    void compute_config_fingerprint() noexcept {
        Sha256Ctx ctx;
        sha256_init(ctx);
        char line[512];
        auto mix = [&](const char* format, auto... args) {
            const int written = std::snprintf(line, sizeof(line), format, args...);
            if (written > 0 && static_cast<size_t>(written) < sizeof(line))
                sha256_update(ctx, reinterpret_cast<const uint8_t*>(line),
                              static_cast<size_t>(written));
        };
        mix("mode=%s;", bot_mode_name(bot_mode));
        mix("clob=%s;ws=%s;gamma=%s;userws=%s;", clob_host, ws_host, gamma_host,
            user_ws_host);
        mix("rpc=%s;rpc_backup=%s;", polygon_rpc_url, polygon_rpc_backup_url);
        mix("slug=%s;condition=%s;token=%s;", market_slug, condition_id, token_id_dec);
        mix("order_type=%s;signature_type=%u;neg_risk=%d;gtd=%llu;", order_type,
            static_cast<unsigned>(signature_type), neg_risk ? 1 : 0,
            static_cast<unsigned long long>(gtd_ttl_seconds));
        mix("tick=%llu;min_size=%llu;max_fee=%llu;",
            static_cast<unsigned long long>(tick_size),
            static_cast<unsigned long long>(min_size_shares),
            static_cast<unsigned long long>(max_taker_fee_rate * 1000000.0));
        mix("bankroll=%llu;edge=%llu;kelly=%llu;conf=%llu;q=%llu;order=%llu;"
            "exposure=%llu;loss=%llu;",
            static_cast<unsigned long long>(bankroll_usd * 1000.0),
            static_cast<unsigned long long>(min_edge * 1000000.0),
            static_cast<unsigned long long>(kelly_fraction * 1000000.0),
            static_cast<unsigned long long>(min_confidence * 1000000.0),
            static_cast<unsigned long long>(max_q_value * 1000000.0),
            static_cast<unsigned long long>(max_order_usd * 1000.0),
            static_cast<unsigned long long>(max_exposure_usd * 1000.0),
            static_cast<unsigned long long>(max_daily_loss_usd * 1000.0));
        mix("ledger=%s;fsync=%d;checkpoint=%llu;", ledger_dir, ledger_fsync ? 1 : 0,
            static_cast<unsigned long long>(ledger_checkpoint_every));
        mix("heartbeat=%d;%u;%u;%u;%u;%u;", heartbeat_enabled ? 1 : 0,
            heartbeat_interval_ms, heartbeat_warn_ms, heartbeat_block_ms,
            heartbeat_assume_cancelled_ms, heartbeat_max_failures);
        mix("userws=%d;%u;%u;%u;%u;%u;", user_ws_enabled ? 1 : 0,
            user_ws_keepalive_ms, user_ws_idle_ms, user_ws_pong_ms,
            user_ws_reconnect_min_ms, user_ws_reconnect_max_ms);
        mix("preflight=%s;age=%llu;skew=%llu;min_collateral=%llu;target=%llu;",
            preflight_token_file, static_cast<unsigned long long>(preflight_max_age_s),
            static_cast<unsigned long long>(max_clock_skew_s),
            static_cast<unsigned long long>(min_collateral_base),
            static_cast<unsigned long long>(target_allowance_base));
        mix("kill=%s;tls_pin=%s;strict_env=%d;session_timeout=%llu;",
            kill_switch_file, tls_pin, strict_env ? 1 : 0,
            static_cast<unsigned long long>(session_timeout_ms));
        uint8_t digest[32];
        sha256_final(ctx, digest);
        static const char* digits = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            config_fingerprint[i * 2] = digits[digest[i] >> 4];
            config_fingerprint[i * 2 + 1] = digits[digest[i] & 0x0FU];
        }
        config_fingerprint[64] = '\0';
        secure_zero(digest, sizeof(digest));
    }

    // Every environment variable with one of our prefixes must be read by this
    // binary; anything else is a configuration the process would silently
    // ignore, which is exactly the failure mode this check exists to prevent.
    // Every environment variable with one of our prefixes must be read by this
    // binary; anything else is a configuration the process would silently
    // ignore, which is exactly the failure mode this check exists to prevent.
    static const char* unknown_prefixed_env() noexcept {
        static char message[160];
        for (char** entry = environ; entry && *entry; ++entry) {
            const char* equals = std::strchr(*entry, '=');
            if (!equals) continue;
            const size_t name_len = static_cast<size_t>(equals - *entry);
            char name[96];
            if (name_len == 0 || name_len >= sizeof(name)) continue;
            std::memcpy(name, *entry, name_len);
            name[name_len] = '\0';
            if (!has_config_prefix(name)) continue;
            if (is_known_config_key(name)) continue;
            std::snprintf(message, sizeof(message),
                          "unknown configuration variable %s (this binary would "
                          "ignore it)", name);
            return message;
        }
        return nullptr;
    }

private:
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
