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

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"
#include "polymarket_order.hpp"
#include "alpha_receiver.hpp"

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
    bool mock_mode = false;
    bool live_armed = false;
    char tls_pin[128]{};
    char kill_switch_file[192] = "/tmp/crowdintel.kill";

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
        if (mode && *mode && std::strcmp(mode, "mock") != 0 &&
            std::strcmp(mode, "live") != 0)
            return "BOT_MODE must be mock or live";
        mock_mode = force_mock || (mode && std::strcmp(mode, "mock") == 0);
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
