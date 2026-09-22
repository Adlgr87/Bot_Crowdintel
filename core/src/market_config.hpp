#ifndef MARKET_CONFIG_HPP
#define MARKET_CONFIG_HPP

// ─────────────────────────────────────────────────────────────────────────────
// MarketConfig: cold-path, start-up-time configuration (env-driven).
// Everything expensive (hex strings, uint256 decimal, domain) is derived once
// here so the hot path only touches plain bytes.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../crypto/eip712_signer.hpp"

struct MarketConfig {
    // ── Identity / credentials ──────────────────────────────────────────────
    uint8_t  maker[20] = {0};        // funder wallet (defaults to signer addr)
    uint8_t  signer[20] = {0};       // derived from the private key at init
    char     maker_hex[43] = {0};
    char     signer_hex[43] = {0};
    char     owner_api_key[64] = {0};       // CLOB API key (POLY_API_KEY)
    char     api_secret_b64[128] = {0};     // CLOB secret (base64url, as given)
    char     api_passphrase[128] = {0};

    // ── Market ──────────────────────────────────────────────────────────────
    char     token_id_dec[80] = {0};        // decimal string for the wire body
    uint8_t  token_id_be[32] = {0};         // big-endian for ABI/hash
    uint64_t tick_size = 10000;             // 0.01 in ×1e6 fixed
    bool     neg_risk = false;              // selects the verifying contract
    uint8_t  signature_type = 0;            // 0 EOA / 1 POLY_PROXY / 2 SAFE / 3 POLY_1271
    char     order_type[8] = "GTC";         // GTC | GTD | FOK | FAK
    char     clob_host[128] = "https://clob.polymarket.com";
    char     ws_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/market";

    // ── Strategy / risk ─────────────────────────────────────────────────────
    double   bankroll_usd = 10000.0;
    double   kelly_fraction = 0.25;         // cap on Kelly multiplier
    double   min_edge = 0.02;               // minimum |p_win − price| to trade
    double   min_confidence = 0.85;         // CrowdIntel signal floor
    double   max_q_value = 0.05;            // FDR ceiling
    uint64_t min_size_shares = 5000000;     // 5 shares in ×1e6
    uint64_t presign_ttl_ms = 30000;        // pre-signed pool refresh period

    // ── Runtime ─────────────────────────────────────────────────────────────
    int      pin_cpu = -1;                  // hot-thread core (-1 = no pinning)
    long     max_ticks = 0;                 // 0 = run until SIGINT
    bool     mock_mode = false;             // no network, synthetic signals
    char     tls_pin[128] = {0};            // optional sha256// pin for clob host

    static const char* env(const char* k, const char* dflt) {
        const char* v = std::getenv(k);
        return (v && *v) ? v : dflt;
    }
    static double env_d(const char* k, double dflt) {
        const char* v = std::getenv(k);
        return (v && *v) ? std::atof(v) : dflt;
    }
    static long env_l(const char* k, long dflt) {
        const char* v = std::getenv(k);
        return (v && *v) ? std::atol(v) : dflt;
    }

    // Loads env config. Returns error string or nullptr on success.
    // `need_trading_creds`: false for offline/bench usage.
    const char* load(bool need_trading_creds) {
        // Private key (REQUIRED — there is no demo fallback; a bot without a
        // wallet must not silently run on a dummy key).
        const char* key_hex = std::getenv("BOT_PRIVATE_KEY_HEX");
        if (!key_hex || std::strlen(key_hex) != 64)
            return "BOT_PRIVATE_KEY_HEX must be 64 hex chars (32 bytes)";
        uint8_t key[32];
        if (!parse_hex_bytes(key_hex, 64, key, 32))
            return "BOT_PRIVATE_KEY_HEX is not valid hex";

        neg_risk        = std::getenv("BOT_NEG_RISK") && std::getenv("BOT_NEG_RISK")[0] == '1';
        signature_type  = (uint8_t)env_l("BOT_SIGNATURE_TYPE", 0);
        tick_size       = (uint64_t)(env_d("BOT_TICK_SIZE", 0.01) * 1000000.0 + 0.5);
        bankroll_usd    = env_d("BOT_BANKROLL_USD", bankroll_usd);
        kelly_fraction  = env_d("BOT_KELLY_FRACTION", kelly_fraction);
        min_edge        = env_d("BOT_MIN_EDGE", min_edge);
        min_confidence  = env_d("BOT_MIN_CONFIDENCE", min_confidence);
        max_q_value     = env_d("BOT_MAX_Q_VALUE", max_q_value);
        min_size_shares = (uint64_t)(env_d("BOT_MIN_SIZE_SHARES", 5.0) * 1000000.0);
        presign_ttl_ms  = (uint64_t)env_l("BOT_PRESIGN_TTL_MS", (long)presign_ttl_ms);
        pin_cpu         = (int)env_l("BOT_PIN_CPU", -1);
        max_ticks       = env_l("BOT_TICKS", 0);
        mock_mode       = std::getenv("BOT_MODE") && std::string(std::getenv("BOT_MODE")) == "mock";
        if (std::getenv("BOT_ORDER_TYPE"))
            std::snprintf(order_type, sizeof(order_type), "%s", std::getenv("BOT_ORDER_TYPE"));
        if (std::getenv("CLOB_HOST"))
            std::snprintf(clob_host, sizeof(clob_host), "%s", std::getenv("CLOB_HOST"));
        if (std::getenv("WS_HOST"))
            std::snprintf(ws_host, sizeof(ws_host), "%s", std::getenv("WS_HOST"));
        if (std::getenv("BOT_TLS_PIN"))
            std::snprintf(tls_pin, sizeof(tls_pin), "%s", std::getenv("BOT_TLS_PIN"));

        if (need_trading_creds) {
            const char* k = std::getenv("CLOB_API_KEY");
            const char* s = std::getenv("CLOB_SECRET");
            const char* p = std::getenv("CLOB_PASSPHRASE");
            if (!k || !s || !p)
                return "CLOB_API_KEY, CLOB_SECRET, CLOB_PASSPHRASE must be set";
            std::snprintf(owner_api_key, sizeof(owner_api_key), "%s", k);
            std::snprintf(api_secret_b64, sizeof(api_secret_b64), "%s", s);
            std::snprintf(api_passphrase, sizeof(api_passphrase), "%s", p);
        } else {
            std::snprintf(owner_api_key, sizeof(owner_api_key), "00000000-0000-0000-0000-000000000000");
        }

        // Token (mock mode tolerates a default so local runs work out of the box).
        const char* tok = env("BOT_TOKEN_ID",
            "71321045679252212594626395510336467040167069592778062791519851593659551227755");
        std::snprintf(token_id_dec, sizeof(token_id_dec), "%s", tok);
        if (!parse_uint256_dec(tok, std::strlen(tok), token_id_be))
            return "BOT_TOKEN_ID is not a valid uint256 decimal";

        // Signer (from key) + maker (funder; default = signer for EOA flow).
        EIP712Signer tmp;
        tmp.init(key, neg_risk);
        std::memcpy(signer, tmp.signer_address(), 20);
        const char* maker_env = std::getenv("BOT_MAKER_ADDRESS");
        if (maker_env) {
            if (!parse_hex_bytes(maker_env, std::strlen(maker_env), maker, 20))
                return "BOT_MAKER_ADDRESS is not a valid 20-byte hex address";
        } else {
            std::memcpy(maker, signer, 20);
        }

        maker_hex[0] = '0'; maker_hex[1] = 'x';
        bytes_to_hex(maker, 20, maker_hex + 2);
        signer_hex[0] = '0'; signer_hex[1] = 'x';
        bytes_to_hex(signer, 20, signer_hex + 2);
        return nullptr;
    }
};

#endif // MARKET_CONFIG_HPP
