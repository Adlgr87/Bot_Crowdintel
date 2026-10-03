// ═══════════════════════════════════════════════════════════════════════════
// crowdintel-config — configuration validator / renderer (Phase 7, option B).
//
// Decision: the repository has **no** TOML file and never had one (`config.prod.toml`
// does not exist anywhere in the tree; every operational setting is read from the
// environment).  Rather than add a TOML parser dependency and a second source of
// truth that the binary could silently ignore, the operational configuration is a
// plain KEY=VALUE file at /etc/crowdintel/config, consumed by systemd as an
// EnvironmentFile, and this tool is what makes it safe:
//
//   crowdintel-config validate <file>      structure, key allowlist, no secrets,
//                                          no duplicates, ranges and enumerations
//   crowdintel-config render-env <file>    validate, then emit the file for
//                                          systemd EnvironmentFile=
//   crowdintel-config fingerprint          load the effective configuration from
//                                          the environment and print its SHA-256
//                                          fingerprint plus non-secret values
//   crowdintel-config keys                 print the recognised key list
//
// Secrets never live in this file: BOT_PRIVATE_KEY_HEX, CLOB_SECRET,
// CLOB_PASSPHRASE, CLOB_API_KEY and BOT_ALPHA_BEARER_TOKEN are rejected here and
// must be provided through /etc/crowdintel/credentials/* with the *_FILE
// indirection (mode 0400, root-owned) or systemd LoadCredential.
//
// Exit codes: 0 = ok, 1 = validation failed, 2 = usage/IO error.
// ═══════════════════════════════════════════════════════════════════════════

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "market_config.hpp"

namespace {

constexpr size_t K_MAX_FILE = 64 * 1024;
constexpr size_t K_MAX_LINES = 512;

struct Finding {
    size_t line = 0;
    char key[96]{};
    char message[160]{};
    bool fatal = true;
};

struct ValidationResult {
    Finding findings[64]{};
    size_t finding_count = 0;
    size_t key_count = 0;
    bool ok = true;

    void add(size_t line, const char* key, const char* message, bool fatal = true) {
        if (finding_count < sizeof(findings) / sizeof(findings[0])) {
            Finding& finding = findings[finding_count++];
            finding.line = line;
            std::snprintf(finding.key, sizeof(finding.key), "%s", key ? key : "");
            std::snprintf(finding.message, sizeof(finding.message), "%s", message);
            finding.fatal = fatal;
        }
        if (fatal) ok = false;
    }
};

bool read_file(const char* path, std::string& out, char* error, size_t cap) {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        std::snprintf(error, cap, "cannot open %s", path);
        return false;
    }
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 0 ||
        static_cast<uint64_t>(metadata.st_size) > K_MAX_FILE) {
        ::close(fd);
        std::snprintf(error, cap, "%s is not a regular file within %zu bytes", path,
                      K_MAX_FILE);
        return false;
    }
    out.resize(static_cast<size_t>(metadata.st_size));
    size_t total = 0;
    while (total < out.size()) {
        const ssize_t n = ::read(fd, &out[total], out.size() - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            std::snprintf(error, cap, "cannot read %s", path);
            return false;
        }
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    ::close(fd);
    out.resize(total);
    return true;
}

bool valid_key_name(const char* key) {
    if (!key || !*key) return false;
    for (const char* p = key; *p; ++p) {
        const bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                        *p == '_';
        if (!ok) return false;
    }
    return true;
}

bool valid_value_text(const char* value, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c == 0x7F) return false;
        if (c == '"' || c == '\'' || c == '\\' || c == '`' || c == '$') return false;
    }
    return true;
}

// Enumeration and range rules for the keys that decide safety.  Anything not
// listed here is still checked for shape (known key, printable value) and is
// range-checked again by MarketConfig::load() when the process starts.
bool check_value_rules(const char* key, const char* value, char* message,
                       size_t cap) {
    // An empty value means "not set": the process falls back to its default or
    // fails closed later (for example POLYGON_RPC_URL in live mode).  Templates
    // ship optional keys empty on purpose.
    if (!value || !value[0]) return true;
    auto one_of = [&](std::initializer_list<const char*> allowed) {
        for (const char* candidate : allowed)
            if (std::strcmp(candidate, value) == 0) return true;
        return false;
    };
    if (std::strcmp(key, "BOT_MODE") == 0) {
        if (!one_of({"replay", "paper", "live"})) {
            std::snprintf(message, cap, "BOT_MODE must be replay, paper or live");
            return false;
        }
        return true;
    }
    if (std::strcmp(key, "BOT_ORDER_TYPE") == 0) {
        if (!one_of({"GTC", "GTD", "FOK", "FAK"})) {
            std::snprintf(message, cap, "BOT_ORDER_TYPE must be GTC, GTD, FOK or FAK");
            return false;
        }
        return true;
    }
    if (std::strcmp(key, "BOT_SIGNATURE_TYPE") == 0) {
        if (!one_of({"0", "1", "2"})) {
            std::snprintf(message, cap,
                          "BOT_SIGNATURE_TYPE must be 0, 1 or 2 (3 fails closed)");
            return false;
        }
        return true;
    }
    if (std::strcmp(key, "BOT_ENABLE_LIVE_TRADING") == 0 ||
        std::strcmp(key, "BOT_NEG_RISK") == 0 ||
        std::strcmp(key, "BOT_LEDGER_FSYNC") == 0 ||
        std::strcmp(key, "BOT_STRICT_ENV") == 0 ||
        std::strcmp(key, "BOT_USER_WS_ENABLED") == 0 ||
        std::strcmp(key, "BOT_HEARTBEAT_ENABLED") == 0 ||
        std::strcmp(key, "BOT_ALLOW_PROTOCOL_V2") == 0 ||
        std::strcmp(key, "BOT_PREFLIGHT_CHECK_L1") == 0 ||
        std::strcmp(key, "BOT_PREFLIGHT_CHECK_USER_WS") == 0 ||
        std::strcmp(key, "BOT_PREFLIGHT_CHECK_HEARTBEAT") == 0 ||
        std::strcmp(key, "CROWDINTEL_FORCE_MOCK") == 0) {
        if (!one_of({"0", "1"})) {
            std::snprintf(message, cap, "%s must be 0 or 1", key);
            return false;
        }
        return true;
    }
    if (std::strcmp(key, "CLOB_HOST") == 0 ||
        std::strcmp(key, "GAMMA_HOST") == 0 ||
        std::strcmp(key, "POLYGON_RPC_URL") == 0 ||
        std::strcmp(key, "POLYGON_RPC_BACKUP_URL") == 0) {
        if (std::strncmp(value, "https://", 8) != 0) {
            std::snprintf(message, cap, "%s must be an https:// URL", key);
            return false;
        }
        return true;
    }
    if (std::strcmp(key, "WS_HOST") == 0 ||
        std::strcmp(key, "BOT_USER_WS_HOST") == 0) {
        if (std::strncmp(value, "wss://", 6) != 0) {
            std::snprintf(message, cap, "%s must be a wss:// URL", key);
            return false;
        }
        return true;
    }
    if (std::strcmp(key, "BOT_LEDGER_DIR") == 0 ||
        std::strcmp(key, "BOT_PREFLIGHT_TOKEN_FILE") == 0) {
        if (value[0] != '/') {
            std::snprintf(message, cap, "%s must be an absolute path", key);
            return false;
        }
        return true;
    }
    // Numeric keys: parse and bound.  Suffix matching (not substring) so that
    // BOT_MARKET_SLUG is not mistaken for a "_S" duration.
    auto ends_with = [&](const char* suffix) {
        const size_t key_len = std::strlen(key);
        const size_t suffix_len = std::strlen(suffix);
        return key_len >= suffix_len &&
               std::strcmp(key + (key_len - suffix_len), suffix) == 0;
    };
    const bool numeric_key =
        ends_with("_MS") || ends_with("_S") || ends_with("_USD") ||
        ends_with("_PORT") || ends_with("_CPU") || ends_with("SHARES") ||
        std::strcmp(key, "BOT_TICKS") == 0 ||
        std::strcmp(key, "BOT_RECON_MAX_PAGES") == 0 ||
        std::strcmp(key, "BOT_LEDGER_CHECKPOINT_EVERY") == 0 ||
        std::strcmp(key, "BOT_HEARTBEAT_MAX_FAILURES") == 0 ||
        std::strcmp(key, "BOT_MIN_COLLATERAL") == 0 ||
        std::strcmp(key, "BOT_TARGET_ALLOWANCE") == 0 ||
        std::strcmp(key, "BOT_MAX_TAKER_FEE_RATE") == 0 ||
        std::strcmp(key, "BOT_TICK_SIZE") == 0;
    if (numeric_key) {
        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (errno == ERANGE || !end || *end != '\0') {
            std::snprintf(message, cap, "%s must be a plain number", key);
            return false;
        }
        if (parsed < -1.0 || parsed > 1e15) {
            std::snprintf(message, cap, "%s is out of range", key);
            return false;
        }
    }
    return true;
}

bool validate(const char* path, ValidationResult& result, std::string& content) {
    char error[192]{};
    if (!read_file(path, content, error, sizeof(error))) {
        result.add(0, "", error);
        return false;
    }
    size_t line_number = 0;
    size_t cursor = 0;
    std::string seen_keys;
    while (cursor <= content.size()) {
        size_t end = content.find('\n', cursor);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(cursor, end - cursor);
        cursor = end + 1;
        ++line_number;
        if (line_number > K_MAX_LINES) {
            result.add(line_number, "", "configuration file has too many lines");
            return false;
        }
        // Strip a trailing CR (files edited on other platforms).
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Trim leading whitespace.
        size_t begin = 0;
        while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin])))
            ++begin;
        if (begin == line.size()) continue;
        if (line[begin] == '#' || line[begin] == ';') continue;
        if (begin) line = line.substr(begin);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            result.add(line_number, "", "expected KEY=VALUE");
            continue;
        }
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        if (!valid_key_name(key.c_str())) {
            result.add(line_number, key.c_str(), "key must be [A-Z0-9_]+");
            continue;
        }
        if (!is_known_config_key(key.c_str())) {
            result.add(line_number, key.c_str(),
                       "unknown key: this binary would ignore it");
            continue;
        }
        if (is_secret_config_key(key.c_str())) {
            result.add(line_number, key.c_str(),
                       "secret keys are not allowed in this file; use "
                       "/etc/crowdintel/credentials/* with the *_FILE indirection");
            continue;
        }
        if (!valid_value_text(value.c_str(), value.size())) {
            result.add(line_number, key.c_str(),
                       "value contains control characters, quotes or shell "
                       "metacharacters");
            continue;
        }
        if (seen_keys.find("|" + key + "|") != std::string::npos) {
            result.add(line_number, key.c_str(), "duplicate key");
            continue;
        }
        seen_keys += "|" + key + "|";
        char message[192]{};
        if (!check_value_rules(key.c_str(), value.c_str(), message, sizeof(message))) {
            result.add(line_number, key.c_str(), message);
            continue;
        }
        if (message[0]) result.add(line_number, key.c_str(), message, false);
        ++result.key_count;
    }
    // A configuration that says live must also arm the gate it depends on.
    if (content.find("BOT_MODE=live") != std::string::npos &&
        content.find("POLYGON_RPC_URL=") == std::string::npos) {
        result.add(0, "POLYGON_RPC_URL",
                   "live mode requires POLYGON_RPC_URL so the chain id can be "
                   "verified before any constant is trusted");
    }
    if (content.find("BOT_MODE=live") != std::string::npos &&
        content.find("BOT_USER_WS_ENABLED=0") != std::string::npos) {
        result.add(0, "BOT_USER_WS_ENABLED",
                   "live mode requires the user channel: without it there is no "
                   "fill visibility");
    }
    if (content.find("BOT_HEARTBEAT_ENABLED=1") != std::string::npos) {
        result.add(0, "BOT_HEARTBEAT_ENABLED",
                   "heartbeat enabled: these credentials must be dedicated to this "
                   "process, because a lapsed heartbeat cancels every order that "
                   "owns them",
                   false);
    }
    return result.ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: crowdintel-config validate <file>\n"
                     "       crowdintel-config render-env <file>\n"
                     "       crowdintel-config fingerprint\n"
                     "       crowdintel-config keys\n");
        return 2;
    }
    const std::string command = argv[1];

    if (command == "keys") {
        for (size_t i = 0; i < K_CONFIG_KEY_COUNT; ++i)
            std::printf("%s\n", K_CONFIG_KEYS[i]);
        return 0;
    }

    if (command == "fingerprint") {
        MarketConfig config;
        const bool live = std::getenv("BOT_MODE") &&
                          std::strcmp(std::getenv("BOT_MODE"), "live") == 0;
        if (const char* error = config.load(live, false)) {
            std::fprintf(stderr, "CONFIG_ERROR: %s\n", error);
            return 1;
        }
        std::printf("mode=%s\n", bot_mode_name(config.bot_mode));
        std::printf("config_fingerprint=%s\n", config.config_fingerprint);
        std::printf("clob_host=%s\n", config.clob_host);
        std::printf("ws_host=%s\n", config.ws_host);
        std::printf("gamma_host=%s\n", config.gamma_host);
        std::printf("user_ws_host=%s\n", config.user_ws_host);
        std::printf("polygon_rpc_url=%s\n", config.polygon_rpc_url);
        std::printf("market_slug=%s\n", config.market_slug);
        std::printf("condition_id=%s\n", config.condition_id);
        std::printf("token_id=%s\n", config.token_id_dec);
        std::printf("token_id_source=%s\n", config.token_id_source());
        std::printf("order_type=%s\n", config.order_type);
        std::printf("signature_type=%u\n", static_cast<unsigned>(config.signature_type));
        std::printf("ledger_dir=%s\n", config.ledger_dir);
        std::printf("heartbeat=%s\n", config.heartbeat_enabled ? "enabled" : "disabled");
        std::printf("user_ws=%s\n", config.user_ws_enabled ? "enabled" : "disabled");
        std::printf("preflight_token=%s\n", config.preflight_token_file);
        std::printf("target_allowance_base=%llu\n",
                    static_cast<unsigned long long>(config.target_allowance_base));
        std::printf("min_collateral_base=%llu\n",
                    static_cast<unsigned long long>(config.min_collateral_base));
        return 0;
    }

    if (command != "validate" && command != "render-env") {
        std::fprintf(stderr, "unknown command: %s\n", command.c_str());
        return 2;
    }
    if (argc < 3) {
        std::fprintf(stderr, "%s requires a file argument\n", command.c_str());
        return 2;
    }
    ValidationResult result;
    std::string content;
    const bool valid = validate(argv[2], result, content);
    for (size_t i = 0; i < result.finding_count; ++i) {
        const Finding& finding = result.findings[i];
        std::printf("%s line=%zu key=%s %s\n", finding.fatal ? "ERROR" : "NOTE",
                    finding.line, finding.key, finding.message);
    }
    std::printf("keys=%zu errors=%zu ok=%s\n", result.key_count, result.finding_count,
                valid ? "true" : "false");
    if (!valid) return 1;
    if (command == "render-env") {
        // Emit exactly what systemd should place in the environment.  Secrets are
        // absent by construction (rejected above) and must be supplied through
        // LoadCredential / *_FILE.
        std::fputs(content.c_str(), stdout);
    }
    return 0;
}
