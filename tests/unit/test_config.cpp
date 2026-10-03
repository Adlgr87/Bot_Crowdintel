// Phase 7 unit tests: explicit modes (replay/paper/live), role separation and
// strict environment validation.
//
// These tests are pure configuration: no network, no venue, no signer. Every
// case starts from an empty configuration environment, so the assertions are
// about what MarketConfig::load accepts and rejects — not about ambient state.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../core/src/market_config.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__);      \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

constexpr const char* kKey =
    "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
constexpr const char* kSlug = "aliens-before-2027";
constexpr const char* kToken =
    "71321045679252212594626395510336467040167069592778062791519851593659551227755";

// Removes every BOT_/CLOB_/GAMMA_/WS_ variable from the process environment;
// unsetenv() rewrites `environ`, so the scan restarts after each removal.
void clear_config_env() {
    for (;;) {
        bool removed = false;
        for (char** entry = environ; entry && *entry; ++entry) {
            const char* name = *entry;
            const char* equals = std::strchr(name, '=');
            const size_t length =
                equals ? static_cast<size_t>(equals - name) : std::strlen(name);
            const bool prefixed =
                (length > 4 && std::memcmp(name, "BOT_", 4) == 0) ||
                (length > 5 && std::memcmp(name, "CLOB_", 5) == 0) ||
                (length > 6 && std::memcmp(name, "GAMMA_", 6) == 0) ||
                (length > 3 && std::memcmp(name, "WS_", 3) == 0);
            if (!prefixed) continue;
            const std::string key(name, length);
            unsetenv(key.c_str());
            removed = true;
            break;
        }
        if (!removed) break;
    }
}

MarketConfig::LoadOptions trader_options() {
    return MarketConfig::LoadOptions{};  // mode comes from BOT_MODE
}

MarketConfig::LoadOptions replay_options() {
    MarketConfig::LoadOptions options;
    options.force_replay = true;
    return options;
}

MarketConfig::LoadOptions tool_options() {
    MarketConfig::LoadOptions options;
    options.role = MarketConfig::Role::kTool;
    return options;
}

MarketConfig::LoadOptions inspector_options() {
    MarketConfig::LoadOptions options;
    options.role = MarketConfig::Role::kInspector;
    return options;
}

void set_replay_basics() {
    setenv("BOT_PRIVATE_KEY_HEX", kKey, 1);
    setenv("BOT_MARKET_SLUG", kSlug, 1);
}

void set_paper_basics() {
    set_replay_basics();
    setenv("BOT_ALPHA_BEARER_TOKEN", "alpha-token-123456", 1);
}

void set_live_basics() {
    set_paper_basics();
    setenv("BOT_LEDGER_PATH", "/tmp/crowdintel-config-test.journal", 1);
    setenv("BOT_ENABLE_LIVE_TRADING", "1", 1);
    setenv("CLOB_API_KEY", "api-key-1", 1);
    setenv("CLOB_SECRET", "c2VjcmV0", 1);
    setenv("CLOB_PASSPHRASE", "passphrase", 1);
}

void test_mode_is_mandatory() {
    std::printf("mode_is_mandatory\n");
    clear_config_env();
    set_replay_basics();
    {
        MarketConfig cfg;
        const char* error = cfg.load();
        CHECK(error != nullptr, "no BOT_MODE never falls back to a live default");
        CHECK(error && std::strstr(error, "BOT_MODE") != nullptr,
              "the error names the missing variable");
    }
    setenv("BOT_MODE", "mock", 1);
    {
        MarketConfig cfg;
        const char* error = cfg.load();
        CHECK(error != nullptr && std::strstr(error, "replay") != nullptr &&
                  std::strstr(error, "paper") != nullptr,
              "the removed mock alias explains both replacements");
    }
    setenv("BOT_MODE", "REPLAY", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr, "mode names are exact");
    }
    setenv("BOT_MODE", "replay", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() == nullptr, "replay loads with a signing key and slug");
        CHECK(cfg.replay_mode() && !cfg.live_transport(),
              "replay is fully offline");
        CHECK(cfg.trading_parameters_ready(),
              "replay parameters come from the fixture config");
    }
    clear_config_env();
    set_replay_basics();
    {
        MarketConfig cfg;
        CHECK(cfg.load(replay_options()) == nullptr,
              "an offline build implies replay when BOT_MODE is absent");
        CHECK(cfg.replay_mode(), "the implied mode is replay");
    }
    clear_config_env();
    set_replay_basics();
    {
        MarketConfig cfg;
        const char* error = cfg.load(trader_options());
        CHECK(error != nullptr, "an absent mode is an error on a network build");
    }
}

void test_replay_rules() {
    std::printf("replay_rules\n");
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("BOT_ENABLE_LIVE_TRADING", "1", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr,
              "replay cannot carry the live arming flag");
    }
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("BOT_LEDGER_PATH", "/tmp/crowdintel-config-test.journal", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr,
              "replay cannot point at the venue order journal");
    }
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("BOT_LEDGER_FSYNC", "1", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr,
              "every ledger variable is live-only, not just the path");
    }
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("BOT_TICK_SIZE", "0.01", 1);
    setenv("BOT_MIN_SIZE_SHARES", "5", 1);
    setenv("BOT_TAKER_FEE_RATE", "0.07", 1);
    setenv("BOT_NEG_RISK", "0", 1);
    setenv("BOT_TOKEN_ID", kToken, 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() == nullptr,
              "replay is the only mode that may use venue fixtures");
        CHECK(cfg.tick_size == 10000 && cfg.min_size_shares == 5000000,
              "replay fixtures reach the engine unchanged");
        CHECK(std::strcmp(cfg.token_id_dec, kToken) == 0,
              "the replay fixture token is the configured one");
    }
}

void test_paper_rules() {
    std::printf("paper_rules\n");
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    set_paper_basics();
    {
        MarketConfig cfg;
        CHECK(cfg.load() == nullptr, "paper loads with alpha access and a key");
        CHECK(cfg.paper_mode() && cfg.live_transport(),
              "paper uses the real transports");
        CHECK(!cfg.trading_parameters_ready(),
              "paper waits for the venue metadata like live");
        CHECK(cfg.tick_size == 0 && cfg.effective_tick() == 0,
              "paper never borrows a configured tick");
        CHECK(cfg.ledger_path[0] == '\0', "paper owns no journal");
    }
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    set_paper_basics();
    setenv("BOT_ENABLE_LIVE_TRADING", "1", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr, "paper cannot be armed");
    }
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    set_paper_basics();
    setenv("BOT_TICK_SIZE", "0.01", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr,
              "paper rejects venue parameters like live does");
    }
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    set_paper_basics();
    setenv("BOT_LEDGER_PATH", "/tmp/crowdintel-config-test.journal", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr,
              "paper never writes simulated orders into the venue journal");
    }
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    set_replay_basics();
    {
        MarketConfig cfg;
        const char* error = cfg.load();
        CHECK(error != nullptr && std::strstr(error, "ALPHA") != nullptr,
              "paper without alpha access is not a paper run");
    }
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    set_paper_basics();
    setenv("CLOB_HOST", "http://clob.invalid", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr,
              "paper cannot downgrade the market data transport");
    }
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    setenv("BOT_PRIVATE_KEY_HEX", kKey, 1);
    setenv("BOT_ALPHA_BEARER_TOKEN", "alpha-token-123456", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr,
              "paper still needs a market selector (slug or condition id)");
    }
}

void test_live_rules() {
    std::printf("live_rules\n");
    clear_config_env();
    setenv("BOT_MODE", "live", 1);
    set_live_basics();
    {
        MarketConfig cfg;
        CHECK(cfg.load() == nullptr, "live loads with the full set");
        CHECK(cfg.live_mode() && cfg.live_armed, "live is armed explicitly");
        CHECK(std::strcmp(cfg.owner_api_key, "api-key-1") == 0,
              "live keeps the venue credentials");
    }
    clear_config_env();
    setenv("BOT_MODE", "live", 1);
    set_paper_basics();
    setenv("BOT_ENABLE_LIVE_TRADING", "1", 1);
    setenv("CLOB_API_KEY", "api-key-1", 1);
    setenv("CLOB_SECRET", "c2VjcmV0", 1);
    setenv("CLOB_PASSPHRASE", "passphrase", 1);
    {
        MarketConfig cfg;
        const char* error = cfg.load();
        CHECK(error != nullptr && std::strstr(error, "LEDGER_PATH") != nullptr,
              "live without a journal has no recovery source");
    }
    clear_config_env();
    setenv("BOT_MODE", "live", 1);
    set_live_basics();
    unsetenv("BOT_ENABLE_LIVE_TRADING");
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr, "live without arming stays disarmed");
    }
    clear_config_env();
    setenv("BOT_MODE", "live", 1);
    set_live_basics();
    unsetenv("BOT_ALPHA_BEARER_TOKEN");
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr, "live without alpha access fails");
    }
}

void test_roles() {
    std::printf("roles\n");
    clear_config_env();
    setenv("BOT_MODE", "live", 1);
    set_live_basics();
    unsetenv("BOT_PRIVATE_KEY_HEX");
    {
        MarketConfig cfg;
        CHECK(cfg.load(tool_options()) == nullptr,
              "the preflight runs with L2 credentials and no signing key");
        CHECK(cfg.private_key_hex[0] == '\0' && cfg.owner_api_key[0] != '\0',
              "the tool role reads exactly the credentials it needs");
    }
    clear_config_env();
    setenv("BOT_MODE", "live", 1);
    set_live_basics();
    {
        MarketConfig cfg;
        const char* error = cfg.load(tool_options());
        CHECK(error != nullptr && std::strstr(error, "PRIVATE_KEY") != nullptr,
              "the read-only preflight refuses to run with key material");
    }
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    set_paper_basics();
    {
        MarketConfig cfg;
        CHECK(cfg.load(tool_options()) != nullptr,
              "the account preflight only exists against the live venue");
    }
    clear_config_env();
    setenv("BOT_MODE", "live", 1);
    setenv("BOT_MARKET_SLUG", kSlug, 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(inspector_options()) == nullptr,
              "metadata inspection needs no credential and no arming");
    }
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(inspector_options()) != nullptr,
              "metadata inspection resolves a live document, not a fixture");
    }
}

void test_strict_names() {
    std::printf("strict_names\n");
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("BOT_MAX_ORDER_USDD", "100", 1);
    {
        MarketConfig cfg;
        const char* error = cfg.load();
        CHECK(error != nullptr &&
                  std::strstr(error, "BOT_MAX_ORDER_USDD") != nullptr,
              "a typo in a known variable is an error, not a default");
    }
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("CLOB_OWNER", "me", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr, "unknown CLOB_ names are rejected too");
    }
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("WS_HOSTX", "wss://example.invalid", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr, "unknown WS_ names are rejected too");
    }
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("PATH_LIKE_VARIABLE", "not-ours", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() == nullptr,
              "names that are not ours are left to the rest of the system");
    }
}

void test_secret_hygiene() {
    std::printf("secret_hygiene\n");
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("CLOB_API_KEY", "key\r\nInjected: yes", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr,
              "a present credential must be well-formed even in replay");
    }
    clear_config_env();
    setenv("BOT_MODE", "replay", 1);
    set_replay_basics();
    setenv("CLOB_API_KEY", "api-key-1", 1);
    setenv("CLOB_SECRET", "c2VjcmV0", 1);
    setenv("CLOB_PASSPHRASE", "passphrase", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() == nullptr, "well-formed venue credentials load");
    }
    CHECK(std::getenv("CLOB_API_KEY") == nullptr &&
              std::getenv("CLOB_SECRET") == nullptr &&
              std::getenv("CLOB_PASSPHRASE") == nullptr,
          "loaded credentials are removed from the environment");
    clear_config_env();
    setenv("BOT_MODE", "paper", 1);
    set_paper_basics();
    setenv("BOT_ALPHA_BEARER_TOKEN", "short", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load() != nullptr, "a short alpha token fails closed");
    }
    clear_config_env();
}

}  // namespace

int main() {
    std::printf("== CROWDINTEL configuration mode tests ==\n");
    test_mode_is_mandatory();
    test_replay_rules();
    test_paper_rules();
    test_live_rules();
    test_roles();
    test_strict_names();
    test_secret_hygiene();
    clear_config_env();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
