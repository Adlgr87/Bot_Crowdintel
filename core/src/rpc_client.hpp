#ifndef RPC_CLIENT_HPP
#define RPC_CLIENT_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Minimal Polygon JSON-RPC client used by preflight and reconciliation.
//
// Scope: exactly the read-only calls needed to prove that the process is talking
// to Polygon mainnet and that the account can actually trade — no transaction
// submission, no nonce management, no gas estimation.  Approvals are never sent
// by this code (see docs/DEPLOYMENT.md: the operator sets the allowance target
// explicitly; the bot refuses to run `approve(max_uint256)`).
//
//   eth_chainId                     → must equal 137
//   eth_getCode(addr, "latest")     → contract presence for exchange/CTF/pUSD
//   eth_call ERC20 balanceOf        → collateral (pUSD) balance
//   eth_call ERC20 allowance        → collateral approval for the exchange
//   eth_call ERC1155 balanceOf      → outcome-token inventory
//   eth_call ERC1155 isApprovedForAll → outcome-token operator approval
//   eth_gasPrice                    → sanity/observability only
//
// Function selectors are *derived* from the canonical signatures with the
// project's keccak256 at startup (`selector_for`) instead of being pasted as
// magic numbers, and tests/unit/test_live_safety.cpp asserts the derived values
// against the well-known selectors, so a typo cannot silently query the wrong
// method.
//
// Transport is the same HttpTransport abstraction as the CLOB client, so every
// call is testable offline with fixtures and failover to POLYGON_RPC_BACKUP_URL
// is exercised without a network.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../crypto/keccak256.hpp"
#include "../include/json_scan.hpp"
#include "../include/venue_metadata.hpp"
#include "clob_rest_client.hpp"

namespace rpc {

inline constexpr uint64_t K_POLYGON_CHAIN_ID = 137;

// ── Hex helpers ─────────────────────────────────────────────────────────────
inline int hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool hex_to_bytes(const char* text, size_t len, uint8_t* out, size_t cap,
                         size_t& out_len) noexcept {
    if (!text || len < 2 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return false;
    const size_t digits = len - 2;
    if (digits % 2 != 0 || digits / 2 > cap) return false;
    for (size_t i = 0; i < digits; i += 2) {
        const int high = hex_nibble(text[2 + i]);
        const int low = hex_nibble(text[3 + i]);
        if (high < 0 || low < 0) return false;
        out[i / 2] = static_cast<uint8_t>((high << 4) | low);
    }
    out_len = digits / 2;
    return true;
}

inline bool address_from_hex(const char* text, uint8_t out[20]) noexcept {
    size_t length = 0;
    return hex_to_bytes(text, std::strlen(text), out, 20, length) && length == 20;
}

inline void bytes_to_hex(const uint8_t* bytes, size_t len, char* out,
                         size_t cap) noexcept {
    static const char* digits = "0123456789abcdef";
    if (cap < len * 2 + 3) {
        if (cap) out[0] = '\0';
        return;
    }
    out[0] = '0';
    out[1] = 'x';
    for (size_t i = 0; i < len; ++i) {
        out[2 + i * 2] = digits[bytes[i] >> 4];
        out[3 + i * 2] = digits[bytes[i] & 0x0FU];
    }
    out[2 + len * 2] = '\0';
}

// Parses a 0x-prefixed big-endian quantity into u64.  Values that do not fit
// are rejected rather than truncated: a truncated balance would understate the
// account and could either block trading (safe) or, worse, be compared against
// a reservation and pass incorrectly.
inline bool hex_to_u64(const char* text, size_t len, uint64_t& out) noexcept {
    if (!text || len < 2 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return false;
    size_t i = 2;
    while (i < len && text[i] == '0') ++i;  // strip leading zeros
    if (len - i > 16) return false;
    uint64_t value = 0;
    for (; i < len; ++i) {
        const int nibble = hex_nibble(text[i]);
        if (nibble < 0) return false;
        value = value * 16 + static_cast<uint64_t>(nibble);
    }
    out = value;
    return true;
}

inline void address_to_word(const uint8_t address[20], char out[67]) noexcept {
    char hex[43];
    bytes_to_hex(address, 20, hex, sizeof(hex));
    out[0] = '0';
    out[1] = 'x';
    std::memset(out + 2, '0', 24);
    std::memcpy(out + 26, hex + 2, 40);
    out[66] = '\0';
}

inline void bytes32_to_word(const uint8_t value[32], char out[67]) noexcept {
    char hex[67];
    bytes_to_hex(value, 32, hex, sizeof(hex));
    std::memcpy(out, hex, 67);
}

// ── Selectors ───────────────────────────────────────────────────────────────
inline uint32_t selector_for(const char* signature) noexcept {
    uint8_t digest[32];
    keccak256_hash(reinterpret_cast<const uint8_t*>(signature),
                   std::strlen(signature), digest);
    return (static_cast<uint32_t>(digest[0]) << 24) |
           (static_cast<uint32_t>(digest[1]) << 16) |
           (static_cast<uint32_t>(digest[2]) << 8) | static_cast<uint32_t>(digest[3]);
}

inline uint32_t selector_erc20_balance_of() noexcept {
    return selector_for("balanceOf(address)");
}
inline uint32_t selector_erc20_allowance() noexcept {
    return selector_for("allowance(address,address)");
}
inline uint32_t selector_erc1155_balance_of() noexcept {
    return selector_for("balanceOf(address,uint256)");
}
inline uint32_t selector_erc1155_is_approved_for_all() noexcept {
    return selector_for("isApprovedForAll(address,address)");
}

inline void selector_to_hex(uint32_t selector, char out[11]) noexcept {
    static const char* digits = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 4; ++i) {
        const uint8_t byte = static_cast<uint8_t>((selector >> (8 * (3 - i))) & 0xFFU);
        out[2 + i * 2] = digits[byte >> 4];
        out[3 + i * 2] = digits[byte & 0x0FU];
    }
    out[10] = '\0';
}

// ── Calldata builders ───────────────────────────────────────────────────────
inline size_t build_erc20_balance_of(const uint8_t owner[20], char* out,
                                     size_t cap) noexcept {
    char selector[11];
    char word[67];
    selector_to_hex(selector_erc20_balance_of(), selector);
    address_to_word(owner, word);
    const int written = std::snprintf(out, cap, "%s%s", selector, word + 2);
    return written > 0 && static_cast<size_t>(written) < cap
               ? static_cast<size_t>(written) : 0;
}

inline size_t build_erc20_allowance(const uint8_t owner[20], const uint8_t spender[20],
                                    char* out, size_t cap) noexcept {
    char selector[11];
    char owner_word[67];
    char spender_word[67];
    selector_to_hex(selector_erc20_allowance(), selector);
    address_to_word(owner, owner_word);
    address_to_word(spender, spender_word);
    const int written = std::snprintf(out, cap, "%s%s%s", selector, owner_word + 2,
                                      spender_word + 2);
    return written > 0 && static_cast<size_t>(written) < cap
               ? static_cast<size_t>(written) : 0;
}

// `token_id` is a decimal uint256 string (CLOB asset ids do not fit in u64).
inline size_t build_erc1155_balance_of(const uint8_t owner[20], const char* token_id,
                                       char* out, size_t cap) noexcept {
    uint64_t limbs[4] = {0, 0, 0, 0};
    if (!venue::parse_uint256_limbs(token_id, std::strlen(token_id), limbs)) return 0;
    char selector[11];
    char owner_word[67];
    selector_to_hex(selector_erc1155_balance_of(), selector);
    address_to_word(owner, owner_word);
    size_t written = static_cast<size_t>(
        std::snprintf(out, cap, "%s%s", selector, owner_word + 2));
    if (written == 0 || written >= cap) return 0;
    // Words are big endian: limb3 is the most significant.
    for (int word = 3; word >= 0; --word) {
        char limb_hex[17];
        static const char* digits = "0123456789abcdef";
        for (int i = 0; i < 8; ++i) {
            const uint8_t byte =
                static_cast<uint8_t>((limbs[word] >> (8 * (7 - i))) & 0xFFULL);
            limb_hex[i * 2] = digits[byte >> 4];
            limb_hex[i * 2 + 1] = digits[byte & 0x0FU];
        }
        limb_hex[16] = '\0';
        const int n = std::snprintf(out + written, cap - written, "%s", limb_hex);
        if (n <= 0 || static_cast<size_t>(n) >= cap - written) return 0;
        written += static_cast<size_t>(n);
    }
    return written;
}

inline size_t build_erc1155_is_approved_for_all(const uint8_t owner[20],
                                                const uint8_t operator_address[20],
                                                char* out, size_t cap) noexcept {
    char selector[11];
    char owner_word[67];
    char operator_word[67];
    selector_to_hex(selector_erc1155_is_approved_for_all(), selector);
    address_to_word(owner, owner_word);
    address_to_word(operator_address, operator_word);
    const int written = std::snprintf(out, cap, "%s%s%s", selector, owner_word + 2,
                                      operator_word + 2);
    return written > 0 && static_cast<size_t>(written) < cap
               ? static_cast<size_t>(written) : 0;
}

// ── Response parsing ────────────────────────────────────────────────────────
inline bool extract_result(const char* data, size_t len, char* out, size_t cap) noexcept {
    if (!data || len == 0 || !out || cap == 0) return false;
    out[0] = '\0';
    // An error object means the call failed; never read a partial result.
    size_t start = 0;
    size_t end = 0;
    if (json_scan::find_object(data, len, "error", start, end)) return false;
    if (!json_scan::get_string(data, len, "result", out, cap)) return false;
    return out[0] != '\0';
}

// ── The client ──────────────────────────────────────────────────────────────
class JsonRpcClient {
public:
    static constexpr size_t K_MAX_CALLEDATA = 400;
    static constexpr size_t K_MAX_RESULT = 4096;

    JsonRpcClient(clob::HttpTransport& transport, const char* primary_url,
                  const char* backup_url)
        : transport_(transport) {
        std::snprintf(primary_, sizeof(primary_), "%s", primary_url ? primary_url : "");
        std::snprintf(backup_, sizeof(backup_), "%s", backup_url ? backup_url : "");
    }

    bool configured() const noexcept { return primary_[0] != '\0'; }
    bool has_backup() const noexcept { return backup_[0] != '\0'; }
    const char* primary_url() const noexcept { return primary_; }

    // Issues one JSON-RPC call, failing over to the backup endpoint when the
    // primary fails *unambiguously* (read-only calls are idempotent, so a
    // failover cannot double-apply anything).
    bool call(const char* method, const char* params_json, char* result,
              size_t result_cap, clob::CallResult& call_result) {
        if (!configured()) {
            std::snprintf(call_result.detail, sizeof(call_result.detail),
                          "POLYGON_RPC_URL is not configured");
            return false;
        }
        char body[1024];
        const int written = std::snprintf(
            body, sizeof(body),
            "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"%s\",\"params\":%s}",
            static_cast<unsigned long long>(++request_id_), method,
            params_json && *params_json ? params_json : "[]");
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(body)) return false;

        if (call_once(primary_, body, static_cast<size_t>(written), result, result_cap,
                      call_result))
            return true;
        if (!has_backup() || call_result.ambiguous) return false;
        used_backup_ = true;
        return call_once(backup_, body, static_cast<size_t>(written), result,
                         result_cap, call_result);
    }

    bool chain_id(uint64_t& out, clob::CallResult& call_result) {
        char result[K_MAX_RESULT];
        if (!call("eth_chainId", "[]", result, sizeof(result), call_result)) return false;
        return json_scan::parse_fixed(result, std::strlen(result), 1, out) ||
               hex_to_u64(result, std::strlen(result), out);
    }

    bool has_code(const char* address_hex, bool& out, clob::CallResult& call_result) {
        char params[256];
        const int written = std::snprintf(params, sizeof(params),
                                          "[\"%s\",\"latest\"]", address_hex);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(params)) return false;
        char result[K_MAX_RESULT];
        if (!call("eth_getCode", params, result, sizeof(result), call_result))
            return false;
        // "0x" (empty bytecode) is a successful answer meaning "no contract
        // here"; only transport/JSON failures return false.
        const size_t len = std::strlen(result);
        out = len > 2 && result[0] == '0' && (result[1] == 'x' || result[1] == 'X');
        return true;
    }

    // Native POL balance (gas).  Informational: proxy/safe accounts trade
    // through the relayer and need no gas of their own.
    bool native_balance(const char* address_hex, uint64_t& wei,
                        clob::CallResult& call_result) {
        char params[256];
        const int written = std::snprintf(params, sizeof(params), "[\"%s\",\"latest\"]",
                                          address_hex);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(params)) return false;
        char result[K_MAX_RESULT];
        if (!call("eth_getBalance", params, result, sizeof(result), call_result))
            return false;
        return hex_to_u64(result, std::strlen(result), wei);
    }

    bool eth_call(const char* to_address, const char* calldata, uint64_t& out,
                  clob::CallResult& call_result) {
        char params[1024];
        const int written = std::snprintf(params, sizeof(params),
                                          "[{\"to\":\"%s\",\"data\":\"%s\"},\"latest\"]",
                                          to_address, calldata);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(params)) return false;
        char result[K_MAX_RESULT];
        if (!call("eth_call", params, result, sizeof(result), call_result)) return false;
        return hex_to_u64(result, std::strlen(result), out);
    }

    bool erc20_balance(const char* token_address, const uint8_t owner[20],
                       uint64_t& out, clob::CallResult& call_result) {
        char calldata[K_MAX_CALLEDATA];
        if (!build_erc20_balance_of(owner, calldata, sizeof(calldata))) return false;
        return eth_call(token_address, calldata, out, call_result);
    }

    bool erc20_allowance(const char* token_address, const uint8_t owner[20],
                         const uint8_t spender[20], uint64_t& out,
                         clob::CallResult& call_result) {
        char calldata[K_MAX_CALLEDATA];
        if (!build_erc20_allowance(owner, spender, calldata, sizeof(calldata)))
            return false;
        return eth_call(token_address, calldata, out, call_result);
    }

    bool erc1155_balance(const char* token_address, const uint8_t owner[20],
                         const char* token_id, uint64_t& out,
                         clob::CallResult& call_result) {
        char calldata[K_MAX_CALLEDATA];
        if (!build_erc1155_balance_of(owner, token_id, calldata, sizeof(calldata)))
            return false;
        return eth_call(token_address, calldata, out, call_result);
    }

    bool erc1155_approved_for_all(const char* token_address, const uint8_t owner[20],
                                  const uint8_t operator_address[20], bool& out,
                                  clob::CallResult& call_result) {
        char calldata[K_MAX_CALLEDATA];
        if (!build_erc1155_is_approved_for_all(owner, operator_address, calldata,
                                               sizeof(calldata)))
            return false;
        uint64_t value = 0;
        if (!eth_call(token_address, calldata, value, call_result)) return false;
        out = value != 0;
        return true;
    }

    bool used_backup() const noexcept { return used_backup_; }

private:
    bool call_once(const char* url, const char* body, size_t body_len, char* result,
                   size_t result_cap, clob::CallResult& call_result) {
        const char* headers[1] = {"Content-Type: application/json"};
        clob::HttpResponse response{};
        char buffer[K_MAX_RESULT + 256];
        response.body = buffer;
        clob::RequestOptions options{2000, 5000, false};
        transport_.request("POST", url, body, body_len, headers, 1, options, buffer,
                           sizeof(buffer), response);
        call_result.response = response;
        call_result.response.body = buffer;
        call_result.ambiguous = response.ambiguous;
        call_result.http_ok = response.transport_ok && response.code >= 200 &&
                              response.code < 300;
        call_result.ok = call_result.http_ok && !response.truncated;
        if (!call_result.ok) {
            std::snprintf(call_result.detail, sizeof(call_result.detail), "%s",
                          response.error[0] ? response.error : "rpc call failed");
            return false;
        }
        if (!extract_result(buffer, response.body_len, result, result_cap)) {
            std::snprintf(call_result.detail, sizeof(call_result.detail),
                          "rpc response has no result");
            call_result.ok = false;
            return false;
        }
        return true;
    }

    clob::HttpTransport& transport_;
    char primary_[256]{};
    char backup_[256]{};
    uint64_t request_id_ = 0;
    bool used_backup_ = false;
};

}  // namespace rpc

#endif  // RPC_CLIENT_HPP
