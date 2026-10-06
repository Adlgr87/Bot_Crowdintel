// ─────────────────────────────────────────────────────────────────────────────
// test_core: unit tests for the hot-path primitives.
//
//   SPSC ring buffer, OrderBookL2 seqlock, exact Kelly math, tick rounding,
//   amount math, wire-body golden build, HMAC midstates vs one-shot, base64url
//   vectors, decimal parsing, uint256 parsing, salt uniqueness.
//
// Exit code 0 = all pass. No external test framework (keeps deps at zero).
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/crypto/sha256_engine.hpp"
#include "../../core/crypto/fast_random.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/polymarket_order.hpp"
#include "../../core/src/presigned_pool.hpp"
#include "../../core/src/execution_engine.hpp"
#include "../../core/src/mock_client.hpp"
#include "../../core/src/order_gateway.hpp"
#include "../../core/src/alpha_http_receiver.hpp"
#include "alpha_parser.hpp"
#ifdef CROWDINTEL_HAVE_NETWORK
#include "../../core/src/ws_market_listener.hpp"
#include "../../core/src/lightweight_client.hpp"
#endif
#include "kelly_engine.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);         \
               ++g_failures; }                                                \
    } while (0)

// ── SPSC ─────────────────────────────────────────────────────────────────────
static void test_spsc_basic() {
    std::printf("spsc_basic\n");
    SPSC_RingBuffer<uint64_t, 16> q;
    uint64_t v = 0;
    CHECK(!q.try_pop(v), "empty pop fails");
    size_t pushed = 0;
    while (q.try_push(42)) ++pushed;
    CHECK(pushed == 15, "capacity-1 slots usable");
    CHECK(q.try_pop(v) && v == 42, "fifo first value");
    CHECK(q.try_push(7), "push after pop succeeds");
}

static void test_spsc_fifo_order() {
    std::printf("spsc_fifo_order\n");
    SPSC_RingBuffer<uint64_t, 64> q;
    for (uint64_t i = 0; i < 1000; ++i) {
        while (!q.try_push(i)) {}
        uint64_t out = 0;
        while (!q.try_pop(out)) {}
        if (out != i) { CHECK(false, "fifo order preserved"); return; }
    }
    CHECK(true, "fifo order preserved");
}

static void test_spsc_multithread() {
    std::printf("spsc_multithread\n");
    SPSC_RingBuffer<uint64_t, 1024> q;
    constexpr uint64_t N = 200000;
    std::thread prod([&] {
        for (uint64_t i = 0; i < N; ++i)
            while (!q.try_push(i)) {}
    });
    bool ok = true;
    uint64_t expected = 0, out;
    while (expected < N) {
        if (q.try_pop(out)) {
            if (out != expected) { ok = false; break; }
            ++expected;
        }
    }
    prod.join();
    CHECK(ok, "no loss/no reorder across 200k items");
}

// ── OrderBook ────────────────────────────────────────────────────────────────
static void test_order_book() {
    std::printf("order_book\n");
    OrderBookL2 book;
    const Level2Entry bids[3] = {{470000, 1000000}, {460000, 2000000}, {450000, 3000000}};
    const Level2Entry asks[3] = {{530000, 1000000}, {540000, 2000000}};
    book.set_book(bids, 3, asks, 2);

    OrderBookL2::Top top{};
    CHECK(book.read_top(top), "seqlock read succeeds");
    CHECK(top.bid.price == 470000 && top.ask.price == 530000, "best levels correct");
    CHECK(top.ask.size == 1000000, "ask size correct");
    CHECK(book.get_bid(999).size == 0, "out-of-range returns sentinel (no UB)");
    CHECK(book.sequence() >= 2, "sequence advances on atomic book publication");

    // Torn-read detection under concurrent writes: read_top must never return
    // an inconsistent (bid>ask) pair without reporting failure... it reports
    // failure instead. Run writer + reader and count failures.
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> false_reads{0}, torn{0};
    std::thread writer([&] {
        uint64_t i = 0;
        while (!stop.load()) {
            const Level2Entry b[1] = {{470000 + (i % 10), 1000000}};
            const Level2Entry a[1] = {{530000 + (i % 10), 1000000}};
            book.set_bids(b, 1);
            book.set_asks(a, 1);
            ++i;
        }
    });
    std::thread reader([&] {
        while (!stop.load()) {
            OrderBookL2::Top t{};
            if (book.read_top(t)) {
                if (t.bid.price >= t.ask.price) ++false_reads;
            } else {
                ++torn;
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop = true;
    writer.join();
    reader.join();
    CHECK(false_reads.load() == 0, "no inconsistent successful reads under write pressure");
    std::printf("    (torn reads retried: %llu)\n", (unsigned long long)torn.load());
}

// ── Kelly ────────────────────────────────────────────────────────────────────
static void test_kelly() {
    std::printf("kelly_exact\n");
    // f = (w - p)/(1 - p): w=0.60, p=0.50 → 0.20
    CHECK(std::abs(KellyEngine::kelly_buy(0.60, 0.50) - 0.20) < 1e-12, "buy kelly 0.60@0.50 = 0.20");
    // w=0.60, p=0.53 → 0.07/0.47
    CHECK(std::abs(KellyEngine::kelly_buy(0.60, 0.53) - 0.07 / 0.47) < 1e-12, "buy kelly 0.60@0.53");
    // No edge → 0
    CHECK(KellyEngine::kelly_buy(0.40, 0.50) == 0.0, "no edge → 0");
    // Sell: w=0.40, p=0.50 → (0.50-0.40)/0.50 = 0.20
    CHECK(std::abs(KellyEngine::kelly_sell(0.40, 0.50) - 0.20) < 1e-12, "sell kelly 0.40@0.50 = 0.20");
    // Position sizing caps at bankroll
    CHECK(KellyEngine::position_usd(2.0, 0.5, 10000.0) == 10000.0, "capped at bankroll");
    CHECK(std::abs(KellyEngine::position_usd(0.2, 0.25, 10000.0) - 500.0) < 1e-9, "fractional sizing");
    CHECK(KellyEngine::usd_to_shares_fixed(500.0, 0.5) == 1000000000ULL,
          "usd→shares fixed-point");
    CHECK(KellyEngine::kelly_buy(std::numeric_limits<double>::quiet_NaN(), 0.5) == 0.0 &&
          KellyEngine::usd_to_shares_fixed(
              std::numeric_limits<double>::infinity(), 0.5) == 0,
          "non-finite sizing inputs fail closed");
    CHECK(KellyEngine::usd_to_shares_fixed(1e300, 0.000001) == 0,
          "fixed-point share overflow fails closed");
}

// ── Tick rounding & amounts ──────────────────────────────────────────────────
static void test_amounts() {
    std::printf("amounts_and_ticks\n");
    const uint64_t tick = 10000;  // 0.01
    CHECK(round_price_to_tick(532999, tick) == 530000, "round down to tick");
    CHECK(round_price_to_tick(5, tick) == 10000, "clamp min tick");
    CHECK(round_price_to_tick(999999, tick) == 990000, "clamp max 1-tick");
    CHECK(round_price_to_tick(530000, 0) == 530000, "zero tick guard");

    uint64_t ma, ta;
    CHECK(compute_amounts(K_SIDE_BUY, 530000, 10000000, ma, ta), "buy amounts ok");
    CHECK(ma == 5300000 && ta == 10000000, "BUY: maker=USDC 5.30, taker=10 shares");
    CHECK(compute_amounts(K_SIDE_SELL, 530000, 10000000, ma, ta), "sell amounts ok");
    CHECK(ma == 10000000 && ta == 5300000, "SELL: maker=shares, taker=USDC");
    CHECK(!compute_amounts(K_SIDE_BUY, 0, 1000000, ma, ta), "zero price rejected");
    CHECK(!compute_amounts(K_SIDE_BUY, 1000000, 1000000, ma, ta), "price≥1 rejected");

    uint64_t effective = 0;
    CHECK(compute_order_amounts(K_SIDE_BUY, 530000, 903251706, 10000,
                                false, ma, ta, effective),
          "limit amounts quantize");
    CHECK(effective == 903250000 && ta == 903250000 && ma % 100 == 0,
          "size <=2dp and amount <=4dp at tick 0.01");
    CHECK(compute_order_amounts(K_SIDE_BUY, 530000, 903251706, 10000,
                                true, ma, ta, effective),
          "FAK/FOK BUY amounts quantize");
    CHECK(ma % 10000 == 0 && ta % 100 == 0,
          "market BUY maker <=2dp and taker <=4dp");
    CHECK(amount_quantum_for_tick(1234) == 0,
          "unknown venue tick fails closed");
}

// ── ceil_to_quantum + BUY market-order rounding (M1) ───────────────────────────
static void test_buy_market_rounding() {
    std::printf("buy_market_rounding\n");
    const uint64_t tick = 10000;  // 0.01

    // ceil_to_quantum basic
    CHECK(ceil_to_quantum(100, 10) == 100, "exact multiple: no round up");
    CHECK(ceil_to_quantum(101, 10) == 110, "ceil up to next quantum");
    CHECK(ceil_to_quantum(109, 10) == 110, "ceil up");
    CHECK(ceil_to_quantum(0, 10) == 0, "zero value");
    CHECK(ceil_to_quantum(1999, 1) == 1999, "quantum=1 is identity");
    CHECK(ceil_to_quantum(1999, 0) == 1999, "quantum=0 is identity (fail closed)");

    // BUG FIX (M1): BUY market order must ceil takerAmount so effective price
    // does not exceed the cap.
    // Scenario: price 0.52, budget $10 (10000000 raw).
    // Exact shares = 10/0.52 = 19.230769 → floored to quantum 100 = 19.2115
    // Floor gives effective price 10/19.2115 = 0.5206 > 0.52 → REJECTED by venue.
    // Ceil gives 19.2308 → effective price 10/19.2308 = 0.5200 ≤ 0.52 → ACCEPTED.
    uint64_t ma, ta, eff;
    CHECK(compute_order_amounts(K_SIDE_BUY, 520000, 10000000, tick,
                                true /*market*/, ma, ta, eff),
          "BUY market amount computes");
    // maker_amount is floored (conservative spend): floor(0.52 * shares) <= 2dp
    CHECK(ma <= 10000000, "maker_amount never exceeds budget");
    // taker_amount is ceiled: effective price = ma/ta must be <= price_u
    const auto effective_price = static_cast<double>(ma) / static_cast<double>(ta);
    const auto price = 0.52;
    CHECK(effective_price <= price + 1e-9,
          "M1: effective BUY price does not exceed cap");

    // Edge case: small order where floor=ceil
    CHECK(compute_order_amounts(K_SIDE_BUY, 500000, 10000000, tick,
                                true, ma, ta, eff),
          "BUY market at 0.50 computes");
    CHECK(static_cast<double>(ma) / static_cast<double>(ta) <= 0.50 + 1e-9,
          "M1: effective price respected at different price");
}

// ── Decimal parsing ──────────────────────────────────────────────────────────
static void test_parsing() {
    std::printf("decimal_parsing\n");
    uint64_t v;
    CHECK(parse_fixed1e6("0.48", 4, v) && v == 480000, "\"0.48\" → 480000");
    CHECK(parse_fixed1e6(".48", 3, v) && v == 480000, "\".48\" → 480000");
    CHECK(parse_fixed1e6("30", 2, v) && v == 30000000, "\"30\" → 30 shares");
    CHECK(parse_fixed1e6("0.0001", 6, v) && v == 100, "min tick precision");
    CHECK(!parse_fixed1e6("abc", 3, v), "garbage rejected");
    CHECK(!parse_fixed1e6("0.0000001", 9, v), "beyond 1e6 precision rejected");
    CHECK(!parse_fixed1e6("1.", 2, v), "decimal point requires a fraction");
    CHECK(!parse_fixed1e6("18446744073709551615", 20, v),
          "fixed-point integer overflow rejected");

    // uint256 (real Polymarket token id length)
    const char* tok = "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    uint8_t be[32];
    CHECK(parse_uint256_dec(tok, std::strlen(tok), be), "78-digit token parses");
    CHECK(be[31] != 0, "low limb populated");
    // Round-trip via Python cross-check (cross_check_v2.py asserts equality).
}

// ── base64url / HMAC ─────────────────────────────────────────────────────────
static void test_base64_hmac() {
    std::printf("base64_hmac\n");
    char out[64];
    // RFC 4648 base64url vectors
    CHECK(base64url_encode((const uint8_t*)"", 0, out) == 0, "b64url empty");
    base64url_encode((const uint8_t*)"f", 1, out); out[4] = 0;
    CHECK(std::strcmp(out, "Zg==") == 0, "b64url 'f'");
    base64url_encode((const uint8_t*)"fo", 2, out); out[4] = 0;
    CHECK(std::strcmp(out, "Zm8=") == 0, "b64url 'fo'");
    base64url_encode((const uint8_t*)"foo", 3, out); out[4] = 0;
    CHECK(std::strcmp(out, "Zm9v") == 0, "b64url 'foo'");
    base64url_encode((const uint8_t*)"foob", 4, out); out[8] = 0;
    CHECK(std::strcmp(out, "Zm9vYg==") == 0, "b64url 'foob'");
    // base64url alphabet: bytes 250-255 use '-'/'_'
    const uint8_t hi[3] = {0xfb, 0xff, 0xbf};
    base64url_encode(hi, 3, out); out[4] = 0;
    CHECK(std::strcmp(out, "-_-_") == 0, "b64url urlsafe alphabet");

    // Round trip
    uint8_t dec[8];
    CHECK(base64url_decode("-_-_", 4, dec, sizeof(dec)) == 3 && dec[0] == 0xfb,
          "b64url decode round trip");
    CHECK(base64url_decode("!!!", 3, dec, sizeof(dec)) == SIZE_MAX,
          "invalid base64 character rejected");
    CHECK(base64url_decode("AAAA", 4, dec, 2) == SIZE_MAX,
          "base64 decoder enforces output capacity");
    CHECK(base64url_decode("A===", 4, dec, sizeof(dec)) == SIZE_MAX,
          "malformed base64 padding rejected");

    // HMAC midstates must match the textbook two-pass definition
    HmacSha256 h;
    const uint8_t key[] = "0123456789abcdef0123456789abcdef";
    h.set_key(key, 32);
    const char* msg = "hello hmac world";
    uint8_t got[32], want[32];
    h.compute((const uint8_t*)msg, std::strlen(msg), got);
    // Naive: full two-pass with fresh contexts
    Sha256Ctx c;
    uint8_t ipad[64], opad[64], kblock[64] = {0};
    std::memcpy(kblock, key, 32);
    for (int i = 0; i < 64; ++i) { ipad[i] = kblock[i] ^ 0x36; opad[i] = kblock[i] ^ 0x5c; }
    sha256_init(c); sha256_update(c, ipad, 64);
    sha256_update(c, (const uint8_t*)msg, std::strlen(msg));
    uint8_t inner[32]; sha256_final(c, inner);
    sha256_init(c); sha256_update(c, opad, 64);
    sha256_update(c, inner, 32); sha256_final(c, want);
    CHECK(std::memcmp(got, want, 32) == 0, "hmac midstate == textbook hmac");

    // An unkeyed HMAC must be deterministic (no indeterminate midstate, which
    // would be undefined behaviour) and must never equal the keyed one.
    HmacSha256 unkeyed_a;
    HmacSha256 unkeyed_b;
    uint8_t a[32], b[32];
    unkeyed_a.compute((const uint8_t*)msg, std::strlen(msg), a);
    unkeyed_b.compute((const uint8_t*)msg, std::strlen(msg), b);
    CHECK(std::memcmp(a, b, 32) == 0, "unkeyed HMAC is deterministic");
    CHECK(std::memcmp(a, got, 32) != 0, "unkeyed HMAC never equals the keyed one");
    unkeyed_a.set_key(key, 32);
    unkeyed_a.compute((const uint8_t*)msg, std::strlen(msg), a);
    CHECK(std::memcmp(a, want, 32) == 0, "set_key after a default construction works");
}

// ── Salt uniqueness ──────────────────────────────────────────────────────────
static void test_salt_uniqueness() {
    std::printf("salt_uniqueness\n");
    FastRandom r;
    std::set<uint64_t> seen;
    bool unique = true, json_safe = true;
    for (int i = 0; i < 200000; ++i) {
        const uint64_t s = r.next_salt();
        if (s > ((1ULL << 53) - 1)) json_safe = false;
        if (!seen.insert(s).second) { unique = false; break; }
    }
    CHECK(unique, "200k salts, zero collisions");
    CHECK(json_safe, "all salts are exact JSON/JavaScript integers");
}

static void test_signer_key_validation() {
    std::printf("signer_key_validation\n");
    uint8_t zero_key[32]{};
    EIP712Signer signer;
    CHECK(!signer.init(zero_key, false),
          "invalid secp256k1 scalar fails without a partial signer");
    uint8_t valid_key[32];
    CHECK(parse_hex_bytes(
              "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b",
              64, valid_key, sizeof(valid_key)) && signer.init(valid_key, false),
          "signer can initialize cleanly after invalid scalar rejection");
    secure_zero(valid_key, sizeof(valid_key));
}

// ── Wire body golden build ───────────────────────────────────────────────────
static void test_wire_body() {
    std::printf("wire_body\n");
    // Reproduce the Python golden vector exactly (fixed salt/timestamp/sizes).
    // Zero-initialised and with the parse result checked: an unchecked
    // parse_hex_bytes() left `maker` indeterminate, which -Wuninitialized
    // reported and MSan/valgrind would flag as use of uninitialized memory.
    // The address is 20 bytes, so the hex length argument is 40 (it was 42,
    // which made parse_hex_bytes() fail its `len == out_len * 2` precondition and
    // return without writing; the previous code ignored that return value and
    // then memcpy'd an indeterminate buffer into OrderV2::maker — undefined
    // behaviour that the golden comparison could not see because build_wire_body
    // takes the maker/signer hex strings separately).
    uint8_t maker[20]{};
    CHECK(parse_hex_bytes("26972a79b73e93a0374afabd80302d19638051c9", 40, maker, 20),
          "golden maker address parses");
    OrderV2 o{};
    o.salt = 123456789012345678ULL;
    std::memcpy(o.maker, maker, 20);
    std::memcpy(o.signer, maker, 20);
    const char* tok = "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    CHECK(parse_uint256_dec(tok, std::strlen(tok), o.token_id),
          "golden token id parses");
    o.maker_amount = 5500000;
    o.taker_amount = 10000000;
    o.side = K_SIDE_BUY;
    o.signature_type = 0;
    o.timestamp_ms = 1758528000000ULL;

    uint8_t sig[65]{};
    CHECK(parse_hex_bytes(
              "2d1a7edd096f2073b55765c38cf5f575273f8163a3e16eb2099524c03a6261df"
              "4ddc6f54a6e8e39df1c6bdd311bf916a7c20b3afdc7c8dbd01a40a836baded821c",
              130, sig, 65),
          "golden signature parses");

    WireBody body{};
    const char* maker_hex_full = "0x26972a79b73e93a0374afabd80302d19638051c9";
    const bool ok = build_wire_body(o, sig, tok, maker_hex_full, maker_hex_full,
                                    "11111111-2222-3333-4444-555555555555", "GTC", body);
    CHECK(ok, "body builds");
    const char* want =
        "{\"deferExec\":false,\"order\":{"
        "\"builder\":\"0x0000000000000000000000000000000000000000000000000000000000000000\","
        "\"expiration\":\"0\","
        "\"maker\":\"0x26972a79b73e93a0374afabd80302d19638051c9\","
        "\"makerAmount\":\"5500000\","
        "\"metadata\":\"0x0000000000000000000000000000000000000000000000000000000000000000\","
        "\"salt\":123456789012345678,"
        "\"side\":\"BUY\","
        "\"signature\":\"0x2d1a7edd096f2073b55765c38cf5f575273f8163a3e16eb2099524c03a6261df"
        "4ddc6f54a6e8e39df1c6bdd311bf916a7c20b3afdc7c8dbd01a40a836baded821c\","
        "\"signatureType\":0,"
        "\"signer\":\"0x26972a79b73e93a0374afabd80302d19638051c9\","
        "\"takerAmount\":\"10000000\","
        "\"timestamp\":\"1758528000000\","
        "\"tokenId\":\"71321045679252212594626395510336467040167069592778062791519851593659551227755\"},"
        "\"orderType\":\"GTC\",\"owner\":\"11111111-2222-3333-4444-555555555555\"}";
    CHECK(body.len == std::strlen(want), "body length matches golden");
    CHECK(std::memcmp(body.buf, want, body.len) == 0, "body matches Python golden byte-for-byte");
}

// ── Network response/message parsing (no external requests) ──────────────────
#ifdef CROWDINTEL_HAVE_NETWORK
static void test_clob_responses() {
    std::printf("clob_response_semantics\n");
    const char* accepted =
        "{\"success\":true,\"errorMsg\":\"\",\"orderID\":\"0xabc\","
        "\"status\":\"live\"}";
    SubmitResult result = LightweightCLOBClient::classify_response(
        200, accepted, std::strlen(accepted));
    CHECK(result.ok && result.final && !result.retryable &&
          std::strcmp(result.order_id, "0xabc") == 0 &&
          std::strcmp(result.status, "live") == 0,
          "2xx success body is semantic acceptance");

    const char* rejected =
        "{\"success\":false,\"errorMsg\":\"invalid order\","
        "\"status\":\"rejected\"}";
    result = LightweightCLOBClient::classify_response(
        200, rejected, std::strlen(rejected));
    CHECK(!result.ok && !result.retryable &&
          std::strcmp(result.error, "invalid order") == 0,
          "2xx error body remains a final rejection");

    const char* missing = "{\"orderID\":\"0xdef\",\"status\":\"live\"}";
    result = LightweightCLOBClient::classify_response(
        200, missing, std::strlen(missing));
    CHECK(!result.ok && std::strstr(result.error, "missing success") != nullptr,
          "2xx without success field fails closed");

    const char* unidentified = "{\"success\":true,\"status\":\"live\"}";
    result = LightweightCLOBClient::classify_response(
        200, unidentified, std::strlen(unidentified));
    CHECK(!result.ok && std::strstr(result.error, "missing order ID") != nullptr,
          "success without an order identity fails closed");

    const char* throttled = "{\"success\":false,\"errorMsg\":\"rate limit\"}";
    result = LightweightCLOBClient::classify_response(
        429, throttled, std::strlen(throttled));
    CHECK(!result.ok && result.retryable,
          "explicit 429 without order ID is retryable");

    const char* gateway_error = "{\"success\":false,\"errorMsg\":\"upstream\"}";
    result = LightweightCLOBClient::classify_response(
        503, gateway_error, std::strlen(gateway_error));
    CHECK(!result.ok && !result.retryable,
          "ambiguous 5xx POST outcome is never replayed");

    const char* embedded =
        "{\"note\":\"\\\"success\\\":true\",\"orderID\":\"0xabc\","
        "\"status\":\"live\"}";
    result = LightweightCLOBClient::classify_response(
        200, embedded, std::strlen(embedded));
    CHECK(!result.ok && std::strstr(result.error, "missing success") != nullptr,
          "field-like text inside a JSON string cannot forge acceptance");

    const char* nested =
        "{\"result\":{\"success\":true,\"orderID\":\"0xabc\","
        "\"status\":\"live\"}}";
    result = LightweightCLOBClient::classify_response(
        200, nested, std::strlen(nested));
    CHECK(!result.ok, "nested fields cannot impersonate top-level semantics");

    const char* invalid_syntax =
        "{\"success\":true,,\"orderID\":\"0xabc\",\"status\":\"live\"}";
    result = LightweightCLOBClient::classify_response(
        200, invalid_syntax, std::strlen(invalid_syntax));
    CHECK(!result.ok && std::strstr(result.error, "malformed") != nullptr,
          "syntactically invalid response fails closed");

    const char* duplicate =
        "{\"success\":true,\"success\":false,\"orderID\":\"0xabc\","
        "\"status\":\"live\"}";
    result = LightweightCLOBClient::classify_response(
        200, duplicate, std::strlen(duplicate));
    CHECK(!result.ok && std::strstr(result.error, "duplicate") != nullptr,
          "duplicate semantic response fields fail closed");

    const char* truncated = "{\"success\":true";
    result = LightweightCLOBClient::classify_response(
        200, truncated, std::strlen(truncated));
    CHECK(!result.ok && std::strstr(result.error, "malformed") != nullptr,
          "truncated response fails closed");
}

static void test_ws_parsing() {
    std::printf("ws_market_parsing\n");
    // Real 'book' snapshot message from docs.polymarket.com (market channel)
    const char* book_msg =
        "{\"event_type\":\"book\",\"asset_id\":\"65818...\","
        "\"market\":\"0xbd31...\","
        "\"bids\":[{\"price\":\".48\",\"size\":\"30\"},"
                  "{\"price\":\".49\",\"size\":\"20\"},"
                  "{\"price\":\".50\",\"size\":\"15\"}],"
        "\"asks\":[{\"price\":\".52\",\"size\":\"25\"},"
                  "{\"price\":\".53\",\"size\":\"60\"}],"
        "\"timestamp\":\"123456789000\",\"hash\":\"0x0\"}";
    CHECK(WsMarketListener::msg_is_book(book_msg, std::strlen(book_msg)), "book event detected");
    CHECK(!WsMarketListener::msg_is_price_change(book_msg, std::strlen(book_msg)),
          "book not misclassified as price_change");

    MarketConfig cfg;
    std::snprintf(cfg.token_id_dec, sizeof(cfg.token_id_dec), "65818...");
    OrderBookL2 book;
    WsMarketListener ws(cfg, book);   // no start() — we only test parsing
    ws.handle_message(book_msg, std::strlen(book_msg));
    CHECK(book.get_bid(0).price == 500000 && book.get_bid(0).size == 15000000,
          "best bid sorted desc (.50 × 15)");
    CHECK(book.get_ask(0).price == 520000 && book.get_ask(0).size == 25000000,
          "best ask sorted asc (.52 × 25)");
    CHECK(book.get_bid(2).price == 480000, "third bid level preserved");

    // price_change: update + remove levels
    const char* chg_msg =
        "{\"event_type\":\"price_change\","
        "\"price_changes\":[{\"asset_id\":\"65818...\",\"price\":\".49\","
                             "\"side\":\"BUY\",\"size\":\"0\","
                             "\"best_bid\":\".50\",\"best_ask\":\".52\"},"
                            "{\"asset_id\":\"65818...\",\"price\":\".47\","
                             "\"side\":\"BUY\",\"size\":\"10\"},"
                            "{\"asset_id\":\"65818...\",\"price\":\".54\","
                             "\"side\":\"SELL\",\"size\":\"5\"}]}";
    CHECK(WsMarketListener::msg_is_price_change(chg_msg, std::strlen(chg_msg)),
          "price_change detected");
    ws.handle_message(chg_msg, std::strlen(chg_msg));
    // bids now: .50×15, .48×30, .47×10 (".49" removed)
    CHECK(book.get_bid(0).price == 500000, "best bid unchanged after delta");
    CHECK(book.get_bid(1).price == 480000, "removed level (.49) gone");
    CHECK(book.get_bid(2).price == 470000 && book.get_bid(2).size == 10000000,
          "new level .47 × 10 inserted");
    CHECK(book.get_ask(1).price == 530000 && book.get_ask(2).price == 540000 &&
          book.get_ask(2).size == 5000000, "ask .54 × 5 appended after .53");

    // Legacy pair-format arrays (["0.4","100"]) also parse
    const char* pair_msg =
        "{\"event_type\":\"book\",\"bids\":[[\"0.40\",\"100\"]],"
        "\"asks\":[[\"0.60\",\"200\"]]}";
    ws.handle_message(pair_msg, std::strlen(pair_msg));
    CHECK(book.get_bid(0).price == 400000 && book.get_bid(0).size == 100000000,
          "legacy [price,size] pair format parsed");
    const char* tick_change =
        "{\"event_type\":\"tick_size_change\",\"new_tick_size\":\"0.005\"}";
    ws.handle_message(tick_change, std::strlen(tick_change));
    CHECK(book.tick_size(10000) == 5000,
          "supported venue tick change is published");
    const char* unknown_tick =
        "{\"event_type\":\"tick_size_change\",\"new_tick_size\":\"0.003\"}";
    ws.handle_message(unknown_tick, std::strlen(unknown_tick));
    CHECK(book.get_bid(0).size == 0 && book.get_ask(0).size == 0,
          "unknown venue tick invalidates tradable depth");
    ws.handle_message(pair_msg, std::strlen(pair_msg));
    const char* empty_book =
        "{\"event_type\":\"book\",\"bids\":[],\"asks\":[]}";
    ws.handle_message(empty_book, std::strlen(empty_book));
    CHECK(book.get_bid(0).size == 0 && book.get_ask(0).size == 0,
          "authoritative empty snapshot clears stale depth");

    // No NUL terminator and hostile bytes directly after len: bounded parser
    // must neither consume the suffix nor report a different event.
    const char bounded[] = {'{','\"','e','v','e','n','t','_','t','y','p','e','\"',
                            ':','\"','b','o','o','k','\"','}',
                            'X','X','X','X','X'};
    CHECK(WsMarketListener::msg_is_book(bounded, 21),
          "event scanner is bounded by explicit length");
    const char* embedded_event =
        "{\"note\":\"\\\"event_type\\\":\\\"book\\\"\"}";
    CHECK(!WsMarketListener::msg_is_book(embedded_event,
                                         std::strlen(embedded_event)),
          "field-like text inside WSS strings is ignored");
    const char* malformed_event = "{\"event_type\":\"book\",}";
    CHECK(!WsMarketListener::msg_is_book(malformed_event,
                                         std::strlen(malformed_event)),
          "malformed WSS JSON fails closed");
    const char* duplicate_event =
        "{\"event_type\":\"book\",\"event_type\":\"price_change\"}";
    CHECK(!WsMarketListener::msg_is_book(duplicate_event,
                                         std::strlen(duplicate_event)),
          "duplicate WSS semantic fields fail closed");
}
#endif

static void test_alpha_parser() {
    std::printf("alpha_parser\n");
    SPSC_RingBuffer<AlphaSignal> queue;
    AlphaParser parser(queue, "target-market", 0.05, 0.85);
    CHECK(!parser.process("other-market", 12, 0.7, 0.9, 0.01),
          "wrong market rejected before queue");
    CHECK(!parser.process("target-market", 13, 0.7, 0.9, 0.01, 9),
          "invalid direction rejected");
    CHECK(parser.process("target-market", 13, 0.7, 0.9, 0.01, 0,
                         AlphaParser::realtime_ns(), 42),
          "validated signal enqueued");
    AlphaSignal signal{};
    CHECK(queue.try_pop(signal) && signal.direction_hint == 0 &&
          signal.signal_id == 42 && signal.market_hash == parser.expected_market_hash(),
          "normalized identity and BUY hint preserved");
}

static void init_pool_fixture(MarketConfig& cfg, EIP712Signer& signer) {
    const char* key_text =
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
    uint8_t key[32];
    const bool key_ok = parse_hex_bytes(key_text, 64, key, sizeof(key));
    CHECK(key_ok, "pool fixture signer key parsed");
    CHECK(key_ok && signer.init(key, false), "pool fixture signer initialized");
    secure_zero(key, sizeof(key));
    const char* token =
        "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    std::snprintf(cfg.token_id_dec, sizeof(cfg.token_id_dec), "%s", token);
    CHECK(parse_uint256_dec(token, std::strlen(token), cfg.token_id_be),
          "pool fixture token parsed");
    CHECK(cfg.finalize_identity(signer.signer_address()) == nullptr,
          "pool fixture identity finalized");
    std::snprintf(cfg.owner_api_key, sizeof(cfg.owner_api_key), "test-owner");
}

static void test_presigned_pool_concurrency() {
    std::printf("presigned_pool_concurrency\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_pool_fixture(cfg, signer);
    PresignedOrderPool pool(cfg, signer, 10000);
    CHECK(pool.rebuild(490000, 510000, 16000000, 10000),
          "initial signed ladder built");
    const size_t buy_slots = pool.built_count() / 2;
    {
        WireBody body{};
        uint64_t size = 0, maker = 0, taker = 0;
        CHECK(!pool.acquire_at_most(K_SIDE_BUY, 510000, 5000, 16000000,
                                    body, size, maker, taker),
              "pre-signed entry cannot cross a tick-size generation");
    }
    std::atomic<size_t> successes{0};
    std::atomic<size_t> invalid_amounts{0};
    std::mutex bodies_mu;
    std::set<std::string> bodies;
    std::vector<std::thread> consumers;
    for (size_t i = 0; i < PresignedOrderPool::SIZE_BUCKETS; ++i) {
        consumers.emplace_back([&] {
            WireBody body{};
            uint64_t size = 0, maker = 0, taker = 0;
            if (pool.acquire_at_most(K_SIDE_BUY, 510000, 10000, 16000000,
                                     body, size, maker, taker)) {
                if (size == 0 || maker == 0 || taker == 0)
                    invalid_amounts.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(bodies_mu);
                bodies.emplace(body.buf, body.len);
                successes.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : consumers) thread.join();
    CHECK(successes.load() == buy_slots, "all BUY slots consumed exactly once");
    CHECK(invalid_amounts.load() == 0,
          "concurrent claims return their signed amounts");
    CHECK(bodies.size() == successes.load(), "no signed body delivered twice");

    std::atomic<bool> done{false};
    std::atomic<uint64_t> acquired_copies{0};
    std::atomic<uint64_t> invalid_copies{0};
    consumers.clear();
    for (int n = 0; n < 4; ++n) {
        consumers.emplace_back([&] {
            while (!done.load(std::memory_order_acquire)) {
                WireBody body{};
                uint64_t size = 0, maker = 0, taker = 0;
                if (pool.acquire_at_most(K_SIDE_BUY, 510000, 10000, 16000000,
                                         body, size, maker, taker)) {
                    acquired_copies.fetch_add(1, std::memory_order_relaxed);
                    static constexpr char PREFIX[] =
                        "{\"deferExec\":false,\"order\":{";
                    if (body.len <= sizeof(PREFIX) - 1 ||
                        body.len >= sizeof(body.buf) ||
                        std::memcmp(body.buf, PREFIX, sizeof(PREFIX) - 1) != 0 ||
                        body.buf[body.len] != '\0')
                        invalid_copies.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int i = 0; i < 100; ++i) {
        (void)pool.rebuild(490000, 510000, 16000000, 10000);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    done.store(true, std::memory_order_release);
    for (auto& thread : consumers) thread.join();
    CHECK(acquired_copies.load() > 0,
          "readers acquire bodies while producer republishes");
    CHECK(invalid_copies.load() == 0,
          "readers copy coherent bodies while producer republishes");
}

static void test_signal_deduplication() {
    std::printf("signal_deduplication\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_pool_fixture(cfg, signer);
    std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "dedupe-market");
    cfg.market_hash = alpha_hash_bytes(cfg.market_slug, std::strlen(cfg.market_slug));
    cfg.max_order_usd = 10.0;
    cfg.max_exposure_usd = 100.0;
    cfg.max_daily_loss_usd = 100.0;

    OrderBookL2 book;
    book.set_tick_size(10000);
    const Level2Entry bids[] = {{470000, 50000000}};
    const Level2Entry asks[] = {{530000, 50000000}};
    book.set_book(bids, 1, asks, 1);
    SPSC_RingBuffer<AlphaSignal> signals;
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    CHECK(pool.rebuild(470000, 530000, 20000000, 10000),
          "dedupe fixture ladder built");
    MockCLOBClient client(cfg);
    ExecutionEngine<MockCLOBClient> engine(
        cfg, book, signals, signer, pool, client);

    AlphaSignal signal{};
    signal.direction_hint = K_SIDE_BUY;
    signal.p_win = 0.75;
    signal.confidence = 0.95;
    signal.q_value = 0.01;
    signal.timestamp_ns = AlphaParser::realtime_ns();
    signal.market_hash = cfg.market_hash;
    signal.signal_id = 42;
    CHECK(signals.try_push(signal) &&
          engine.run_tick() == TickResult::SUBMITTED,
          "first authenticated signal can submit");
    signal.timestamp_ns = AlphaParser::realtime_ns();
    CHECK(signals.try_push(signal) &&
          engine.run_tick() == TickResult::DUPLICATE_SIGNAL &&
          client.submissions() == 1,
          "same signal identity cannot submit twice inside TTL");
}

// Verifies the fee-in-cost-cap fix: the taker fee is now included in the
// worst-case cap check (BUY: notional + fee, SELL: notional - fee) and the
// pre-sizing budget is reduced by (1 + buy_fee_rate) so orders at the cap
// boundary are not unnecessarily rejected.
static void test_fee_in_cost_cap() {
    std::printf("fee_in_cost_cap\n");

    // --- BUY: high fee must not cause spurious rejection at the cap boundary ---
    {
        MarketConfig cfg;
        EIP712Signer signer;
        init_pool_fixture(cfg, signer);
        std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "fee-cap-buy");
        cfg.market_hash =
            alpha_hash_bytes(cfg.market_slug, std::strlen(cfg.market_slug));
        cfg.max_order_usd = 10.0;
        cfg.max_exposure_usd = 1000.0;
        cfg.max_daily_loss_usd = 1000.0;
        cfg.taker_fee_rate = 0.50;  // 50 % — fee ≈ $1.90 on a $10 notional

        OrderBookL2 book;
        book.set_tick_size(10000);
        const Level2Entry bids[] = {{470000, 50000000}};
        const Level2Entry asks[] = {{530000, 50000000}};
        book.set_book(bids, 1, asks, 1);
        SPSC_RingBuffer<AlphaSignal> signals;
        PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
        CHECK(pool.rebuild(470000, 530000, 20000000, 10000),
              "fee-cap BUY fixture ladder built");
        MockCLOBClient client(cfg);
        ExecutionEngine<MockCLOBClient> engine(
            cfg, book, signals, signer, pool, client);

        AlphaSignal signal{};
        signal.direction_hint = K_SIDE_BUY;
        signal.p_win = 0.75;
        signal.confidence = 0.95;
        signal.q_value = 0.01;
        signal.timestamp_ns = AlphaParser::realtime_ns();
        signal.market_hash = cfg.market_hash;
        signal.signal_id = 7;

        // Without the pre-sizing fee reservation, notional ≈ $10 and
        // notional + fee ≈ $11.90 > max_order_usd (10) → RISK_REJECTED.
        // With the fix, the budget is capped to $10 / (1+fee_rate) ≈ $8.10
        // so the worst-case cost stays at ≈ $10 → SUBMITTED.
        CHECK(signals.try_push(signal) &&
              engine.run_tick() == TickResult::SUBMITTED,
              "BUY at cap boundary submits with 50 % fee");

        // Committed exposure must be fee-inclusive: it should be close to
        // max_order_usd (notional + fee ≈ $10), NOT noticeably below it.
        // Without the fee fix, exposure would be ≈ notional only (≈ $8.10).
        const double exposure = engine.committed_exposure_usd();
        CHECK(exposure > 9.5, "committed exposure includes taker fee");
    }

    // --- SELL: fee is subtracted from the worst-case net-value notional ---
    {
        MarketConfig cfg;
        EIP712Signer signer;
        init_pool_fixture(cfg, signer);
        std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "fee-cap-sell");
        cfg.market_hash =
            alpha_hash_bytes(cfg.market_slug, std::strlen(cfg.market_slug));
        cfg.max_order_usd = 10.0;
        cfg.max_exposure_usd = 1000.0;
        cfg.max_daily_loss_usd = 1000.0;
        cfg.taker_fee_rate = 0.07;
        cfg.initial_position_shares = 100 * 1000000ULL;  // 100 human shares

        OrderBookL2 book;
        book.set_tick_size(10000);
        const Level2Entry bids[] = {{470000, 50000000}};
        const Level2Entry asks[] = {{530000, 50000000}};
        book.set_book(bids, 1, asks, 1);
        SPSC_RingBuffer<AlphaSignal> signals;
        PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
        CHECK(pool.rebuild(470000, 530000, 20000000, 10000),
              "fee-cap SELL fixture ladder built");
        MockCLOBClient client(cfg);
        ExecutionEngine<MockCLOBClient> engine(
            cfg, book, signals, signer, pool, client);

        AlphaSignal signal{};
        signal.direction_hint = K_SIDE_SELL;
        signal.p_win = 0.25;  // SELL edge: price(0.47) - p_win(0.25) - fee > 0.02
        signal.confidence = 0.95;
        signal.q_value = 0.01;
        signal.timestamp_ns = AlphaParser::realtime_ns();
        signal.market_hash = cfg.market_hash;
        signal.signal_id = 8;

        // For SELL the check uses net_value = notional - fee.  The fee is
        // subtracted (conservative worst-case), so the net value is smaller
        // and well within the cap.
        CHECK(signals.try_push(signal) &&
              engine.run_tick() == TickResult::SUBMITTED,
              "SELL with fee subtracted from notional submits");
    }
}

struct ScriptedGatewayClient {
    std::atomic<int> calls{0};
    void warmup() { warmed.store(true, std::memory_order_release); }
    SubmitResult submit(const WireBody&) {
        SubmitResult result{};
        const int call = calls.fetch_add(1, std::memory_order_relaxed);
        if (call < 2) {
            result.retryable = true;
            std::snprintf(result.error, sizeof(result.error), "safe pre-send failure");
        } else {
            result.ok = true;
            result.final = true;
            std::snprintf(result.status, sizeof(result.status), "live");
        }
        return result;
    }
    std::atomic<bool> warmed{false};
};

static void test_order_gateway() {
    std::printf("order_gateway\n");
    ScriptedGatewayClient client;
    OrderGateway<ScriptedGatewayClient, 4> gateway(client);
    gateway.start();
    WireBody body{};
    std::memcpy(body.buf, "{}", 3);
    body.len = 2;
    const SubmitResult queued = gateway.submit(body);
    CHECK(queued.ok && !queued.final && std::strcmp(queued.status, "queued") == 0,
          "gateway distinguishes enqueue from venue acceptance");
    gateway.stop(true);
    CHECK(client.warmed.load() && client.calls.load() == 3,
          "gateway warms and retries only client-classified safe failures");
    CHECK(gateway.enqueued() == 1 && gateway.accepted() == 1 &&
          gateway.retried() == 2 && gateway.rejected() == 0,
          "gateway outcome metrics reflect final worker result");

    ScriptedGatewayClient blocked_client;
    OrderGateway<ScriptedGatewayClient, 2> blocked(blocked_client);
    CHECK(blocked.submit(body).ok, "bounded gateway accepts first queued body");
    CHECK(!blocked.submit(body).ok && blocked.dropped() == 1,
          "bounded gateway rejects on queue exhaustion");
    blocked.stop(false);
    CHECK(blocked.cancelled() == 1 && blocked_client.calls.load() == 0,
          "emergency stop discards unsent queued orders");

    ScriptedGatewayClient disabled_client;
    std::atomic<bool> disabled{false};
    OrderGateway<ScriptedGatewayClient, 4> disabled_gateway(
        disabled_client, &disabled);
    CHECK(!disabled_gateway.submit(body).ok &&
          disabled_gateway.cancelled() == 1 && disabled_client.calls.load() == 0,
          "kill switch rejects before gateway enqueue");
}

static std::string http_exchange(uint16_t port, const std::string& request) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return {};
    }
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = ::send(fd, request.data() + sent,
                                 request.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
    ::shutdown(fd, SHUT_WR);
    std::string response;
    char chunk[512];
    for (;;) {
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        response.append(chunk, static_cast<size_t>(n));
    }
    ::close(fd);
    return response;
}

static std::string signal_request(const char* token, const std::string& body,
                                  const char* target = "/signal") {
    char headers[512];
    const int n = std::snprintf(headers, sizeof(headers),
        "POST %s HTTP/1.1\r\nHost: localhost\r\n"
        "Authorization: Bearer %s\r\nContent-Length: %zu\r\n\r\n",
        target, token, body.size());
    return std::string(headers, static_cast<size_t>(n)) + body;
}

static void test_alpha_http_receiver() {
    std::printf("alpha_http_receiver\n");
    MarketConfig cfg;
    cfg.alpha_port = 0;
    std::snprintf(cfg.alpha_bind, sizeof(cfg.alpha_bind), "127.0.0.1");
    std::snprintf(cfg.alpha_bearer_token, sizeof(cfg.alpha_bearer_token),
                  "0123456789abcdef-test-token");
    std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "target-market");
    SPSC_RingBuffer<AlphaSignal> queue;
    AlphaParser parser(queue, cfg.market_slug, 0.05, 0.85);
    AlphaHttpReceiver receiver(cfg, parser);
    CHECK(receiver.start() && receiver.bound_port() != 0,
          "HTTP receiver binds an ephemeral loopback port");
    if (receiver.bound_port() == 0) return;

    const std::string body =
        "{\"market\":\"target-market\",\"p_win\":0.7,"
        "\"confidence\":0.9,\"q_value\":0.01,"
        "\"direction\":\"BUY\",\"signal_id\":42}";
    const uint16_t port = receiver.bound_port();
    CHECK(http_exchange(port, signal_request("wrong-token", body)).find(" 401 ") !=
              std::string::npos,
          "HTTP receiver rejects invalid bearer");
    CHECK(http_exchange(port, signal_request(cfg.alpha_bearer_token, body, "/other"))
              .find(" 404 ") != std::string::npos,
          "HTTP receiver rejects wrong route");
    CHECK(http_exchange(port, signal_request(cfg.alpha_bearer_token,
          "{\"market\":\"wrong\",\"p_win\":0.7,\"confidence\":0.9,"
          "\"q_value\":0.01}" )).find(" 422 ") != std::string::npos,
          "HTTP receiver rejects wrong market");
    CHECK(http_exchange(port, signal_request(cfg.alpha_bearer_token, body))
              .find(" 202 ") != std::string::npos,
          "HTTP receiver accepts a valid bounded signal");
    const std::string duplicate_auth =
        std::string("POST /signal HTTP/1.1\r\nAuthorization: Bearer ") +
        cfg.alpha_bearer_token +
        "\r\nAuthorization: Bearer duplicate\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    CHECK(http_exchange(port, duplicate_auth).find(" 401 ") != std::string::npos,
          "HTTP receiver rejects ambiguous duplicate authorization");
    const std::string transfer_encoded =
        std::string("POST /signal HTTP/1.1\r\nAuthorization: Bearer ") +
        cfg.alpha_bearer_token +
        "\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n0";
    CHECK(http_exchange(port, transfer_encoded).find(" 400 ") != std::string::npos,
          "HTTP receiver rejects transfer-encoding smuggling surface");
    const std::string embedded_fields =
        "{\"market\":\"target-market\","
        "\"note\":\"\\\"p_win\\\":0.7\","
        "\"confidence\":0.9,\"q_value\":0.01}";
    CHECK(http_exchange(port, signal_request(cfg.alpha_bearer_token,
          embedded_fields)).find(" 422 ") != std::string::npos,
          "field-like text inside JSON strings is not parsed as alpha");
    CHECK(http_exchange(port, signal_request(cfg.alpha_bearer_token,
          body + " trailing")).find(" 422 ") != std::string::npos,
          "HTTP receiver rejects trailing malformed JSON");
    const std::string invalid_json =
        "{\"market\":\"target-market\",,\"p_win\":0.7,"
        "\"confidence\":0.9,\"q_value\":0.01}";
    CHECK(http_exchange(port, signal_request(cfg.alpha_bearer_token,
          invalid_json)).find(" 422 ") != std::string::npos,
          "syntactically invalid alpha JSON fails closed");
    const std::string duplicate_json =
        "{\"market\":\"target-market\",\"p_win\":0.7,\"p_win\":0.8,"
        "\"confidence\":0.9,\"q_value\":0.01}";
    CHECK(http_exchange(port, signal_request(cfg.alpha_bearer_token,
          duplicate_json)).find(" 422 ") != std::string::npos,
          "duplicate alpha fields fail closed");
    const std::string oversized =
        std::string("POST /signal HTTP/1.1\r\nAuthorization: Bearer ") +
        cfg.alpha_bearer_token + "\r\nContent-Length: 4097\r\n\r\n";
    CHECK(http_exchange(port, oversized).find(" 413 ") != std::string::npos,
          "HTTP receiver rejects an oversized declared body");

    AlphaSignal signal{};
    CHECK(queue.try_pop(signal) && signal.signal_id == 42 &&
          signal.direction_hint == 0,
          "accepted HTTP signal reaches normalized SPSC queue");
    CHECK(receiver.accepted() == 1 && receiver.rejected() == 10,
          "HTTP ingress acceptance/rejection metrics are exact");
    receiver.stop();
}

static void test_config_fail_closed() {
    std::printf("config_fail_closed\n");
    static constexpr char KEY[] =
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
    auto arm_test_key = [] { setenv("BOT_PRIVATE_KEY_HEX", KEY, 1); };

    arm_test_key();
    setenv("BOT_MAX_ORDER_USD", "not-a-number", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "invalid numeric text never falls back to a trading default");
    }
    unsetenv("BOT_MAX_ORDER_USD");

    arm_test_key();
    setenv("BOT_NEG_RISK", "2", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "boolean-like identity values require exactly 0 or 1");
    }
    unsetenv("BOT_NEG_RISK");

    arm_test_key();
    setenv("BOT_INITIAL_POSITION_SHARES", "-1", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "negative inventory cannot wrap into an unsigned balance");
    }
    unsetenv("BOT_INITIAL_POSITION_SHARES");

    arm_test_key();
    setenv("BOT_MIN_SIZE_SHARES", "5.005", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "share configuration must align to venue precision");
    }
    unsetenv("BOT_MIN_SIZE_SHARES");

    arm_test_key();
    setenv("BOT_TICK_SIZE", "0.003", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "unknown V2 tick fails at startup");
    }
    unsetenv("BOT_TICK_SIZE");

    arm_test_key();
    const std::string oversized_market(200, 'x');
    setenv("BOT_MARKET_SLUG", oversized_market.c_str(), 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "oversized configuration is rejected rather than truncated");
    }
    unsetenv("BOT_MARKET_SLUG");

    arm_test_key();
    setenv("BOT_MODE", "live", 1);
    setenv("BOT_MARKET_SLUG", "target-market", 1);
    setenv("CLOB_HOST", "http://clob.invalid", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, false) != nullptr,
              "live CLOB transport cannot downgrade from TLS");
    }
    unsetenv("BOT_MODE");
    unsetenv("BOT_MARKET_SLUG");
    unsetenv("CLOB_HOST");

    arm_test_key();
    setenv("BOT_MODE", "live", 1);
    setenv("BOT_MARKET_SLUG", "target-market", 1);
    setenv("CLOB_HOST", "https://clob.invalid/unexpected-base", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, false) != nullptr,
              "CLOB origin rejects an ambiguous base path");
    }
    unsetenv("BOT_MODE");
    unsetenv("BOT_MARKET_SLUG");
    unsetenv("CLOB_HOST");

    arm_test_key();
    setenv("BOT_ENABLE_LIVE_TRADING", "1", 1);
    setenv("CLOB_API_KEY", "key\r\nInjected: yes", 1);
    setenv("CLOB_SECRET", "c2VjcmV0", 1);
    setenv("CLOB_PASSPHRASE", "passphrase", 1);
    setenv("BOT_ALPHA_BEARER_TOKEN", "alpha-token-123456", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(true, true) != nullptr,
              "credential control characters cannot inject headers");
    }
    unsetenv("BOT_ENABLE_LIVE_TRADING");
    unsetenv("CLOB_API_KEY");
    unsetenv("CLOB_SECRET");
    unsetenv("CLOB_PASSPHRASE");
    unsetenv("BOT_ALPHA_BEARER_TOKEN");

    arm_test_key();
    setenv("BOT_TLS_PIN", "sha256//not-a-real-pin", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "malformed TLS pin fails startup");
    }
    unsetenv("BOT_TLS_PIN");

    arm_test_key();
    const std::string pin = "sha256//" + std::string(43, 'A') + "=";
    const std::string dual_pin = pin + ";" + pin;
    setenv("BOT_TLS_PIN", dual_pin.c_str(), 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) == nullptr,
              "overlapping TLS pins support safe rotation");
    }
    unsetenv("BOT_TLS_PIN");

    // BOT_METADATA_MAX_AGE_MS is enforced by the supervisor, so the refresh
    // period must leave margin: a refresh slower than half the age budget would
    // block trading between two scheduled refreshes by construction.
    arm_test_key();
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) == nullptr, "metadata defaults are coherent");
        CHECK(cfg.metadata_max_age_ms >= cfg.metadata_refresh_ms * 2,
              "the default age budget covers two refresh periods");
    }
    arm_test_key();
    setenv("BOT_METADATA_MAX_AGE_MS", "60000", 1);
    setenv("BOT_METADATA_REFRESH_MS", "300000", 1);
    {
        MarketConfig cfg;
        const char* error = cfg.load(false, true);
        CHECK(error != nullptr &&
                  std::strstr(error, "BOT_METADATA_MAX_AGE_MS") != nullptr,
              "a refresh period longer than half the age budget is rejected");
    }
    unsetenv("BOT_METADATA_MAX_AGE_MS");
    unsetenv("BOT_METADATA_REFRESH_MS");

    // Where the outcome token id comes from must be observable: it decides
    // whether the venue resolves the identity for us or the operator pinned it.
    arm_test_key();
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) == nullptr, "configuration without a token id");
        CHECK(cfg.token_id_from_metadata && cfg.token_id_dec[0] == '\0',
              "no BOT_TOKEN_ID means the id will be resolved from venue metadata");
        CHECK(std::strcmp(cfg.token_id_source(), "venue-metadata") == 0,
              cfg.token_id_source());
    }
    arm_test_key();
    setenv("BOT_MODE", "replay", 1);
    {
        MarketConfig cfg;
        // force_mock (the second argument) rewrites the mode to paper, so this
        // case loads without it to keep BOT_MODE=replay.
        CHECK(cfg.load(false, false) == nullptr, "replay configuration loads");
        CHECK(!cfg.token_id_from_metadata && cfg.token_id_dec[0] != '\0',
              "only replay keeps the documented test vector, it has no venue to ask");
        CHECK(std::strcmp(cfg.token_id_source(), "replay-test-vector") == 0,
              "the built-in vector is reported as such, not as operator-provided");
    }
    unsetenv("BOT_MODE");
    arm_test_key();
    setenv("BOT_TOKEN_ID",
           "65818650605255155769223052139070506823072525854034053159594882546554163"
           "536512",
           1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) == nullptr, "configuration with a token id");
        CHECK(!cfg.token_id_from_metadata && cfg.token_id_dec[0] != '\0',
              "an explicit BOT_TOKEN_ID is recorded as operator-provided");
        CHECK(std::strcmp(cfg.token_id_source(), "BOT_TOKEN_ID") == 0,
              cfg.token_id_source());
    }
    unsetenv("BOT_TOKEN_ID");
    arm_test_key();
    setenv("BOT_TOKEN_ID", "12x34", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "a non-canonical token id is rejected instead of resolved later");
    }
    unsetenv("BOT_TOKEN_ID");

    arm_test_key();
    setenv("BOT_METADATA_MAX_AGE_MS", "60000", 1);
    setenv("BOT_METADATA_REFRESH_MS", "30000", 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) == nullptr,
              "an age budget of two refresh periods is accepted");
        CHECK(cfg.metadata_max_age_ms == 60000 && cfg.metadata_refresh_ms == 30000,
              "both values reach the configuration");
    }
    unsetenv("BOT_METADATA_MAX_AGE_MS");
    unsetenv("BOT_METADATA_REFRESH_MS");
}

static void test_secret_file_loading() {
    std::printf("secret_file_loading\n");
    static constexpr char KEY[] =
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
    char path[] = "/tmp/crowdintel-secret-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0, "temporary credential file created");
    if (fd < 0) return;
    auto rewrite = [&](const std::string& value) {
        CHECK(ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0,
              "credential fixture reset");
        CHECK(value.empty() ||
              write(fd, value.data(), value.size()) ==
                  static_cast<ssize_t>(value.size()),
              "credential fixture written");
    };
    auto loads = [&](const std::string& value) {
        rewrite(value);
        setenv("BOT_PRIVATE_KEY_HEX_FILE", path, 1);
        MarketConfig cfg;
        const bool ok = cfg.load(false, true) == nullptr;
        if (!ok) unsetenv("BOT_PRIVATE_KEY_HEX_FILE");
        return ok;
    };

    CHECK(loads(KEY), "credential without newline loads exactly");
    CHECK(loads(std::string(KEY) + "\n"),
          "credential with LF terminator loads exactly");
    CHECK(loads(std::string(KEY) + "\r\n"),
          "credential with CRLF terminator loads exactly");
    CHECK(std::getenv("BOT_PRIVATE_KEY_HEX_FILE") == nullptr,
          "credential path removed from environment after loading");
    rewrite(KEY);
    setenv("BOT_PRIVATE_KEY_HEX", KEY, 1);
    setenv("BOT_PRIVATE_KEY_HEX_FILE", path, 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "ambiguous direct and file secret sources fail closed");
    }
    unsetenv("BOT_PRIVATE_KEY_HEX");
    unsetenv("BOT_PRIVATE_KEY_HEX_FILE");
    CHECK(!loads(""), "empty credential file fails closed");
    CHECK(!loads(std::string(600, 'a')),
          "oversized credential file fails closed without truncation");

    rewrite(KEY);
    CHECK(fchmod(fd, 0) == 0, "credential fixture permissions removed");
    setenv("BOT_PRIVATE_KEY_HEX_FILE", path, 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "unreadable credential file fails closed");
    }
    unsetenv("BOT_PRIVATE_KEY_HEX_FILE");
    CHECK(fchmod(fd, S_IRUSR | S_IWUSR | S_IROTH) == 0,
          "credential fixture made overly permissive");
    setenv("BOT_PRIVATE_KEY_HEX_FILE", path, 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "group/other credential permissions fail closed");
    }
    unsetenv("BOT_PRIVATE_KEY_HEX_FILE");
    CHECK(fchmod(fd, S_IRUSR | S_IWUSR) == 0,
          "credential fixture permissions restored");

    const std::string link_path = std::string(path) + ".link";
    CHECK(symlink(path, link_path.c_str()) == 0,
          "credential symlink fixture created");
    setenv("BOT_PRIVATE_KEY_HEX_FILE", link_path.c_str(), 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "credential symlink fails closed");
    }
    unsetenv("BOT_PRIVATE_KEY_HEX_FILE");
    unlink(link_path.c_str());

    const std::string missing = std::string(path) + ".missing";
    setenv("BOT_PRIVATE_KEY_HEX_FILE", missing.c_str(), 1);
    {
        MarketConfig cfg;
        CHECK(cfg.load(false, true) != nullptr,
              "absent credential file fails closed");
    }
    unsetenv("BOT_PRIVATE_KEY_HEX_FILE");
    close(fd);
    unlink(path);
}

// ═══════════════════════════════════════════════════════════════════════════
// Environment plumbing: every non-secret knob must reach the field it names
// ═══════════════════════════════════════════════════════════════════════════
// Range validation is covered per struct elsewhere; what was not covered is the
// env -> field assignment itself.  A swapped or dropped assignment changes
// behaviour without tripping any range check (the same family as the dead fee
// assignment found in main_hot_path.cpp), so every knob is set to a distinct,
// valid, non-default value and read back.  Runs last: it sets a lot of
// environment.
void test_config_env_plumbing() {
    static constexpr char KEY[] =
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
    static constexpr char TOKEN[] =
        "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    static constexpr char CONDITION[] =
        "0x1111111111111111111111111111111111111111111111111111111111111111";
    const std::string pin = "sha256//" + std::string(43, 'A') + "=";
    const char* text[][2] = {
        {"BOT_MODE", "paper"},
        {"BOT_MARKET_SLUG", "plumbing-market"},
        {"BOT_CONDITION_ID", CONDITION},
        {"BOT_TOKEN_ID", TOKEN},
        {"BOT_ORDER_TYPE", "GTD"},
        {"BOT_ALPHA_BIND", "127.0.0.1"},
        {"BOT_KILL_SWITCH_FILE", "/tmp/crowdintel-plumbing.kill"},
        {"BOT_TLS_PIN", pin.c_str()},
        {"CLOB_HOST", "https://clob.plumbing.test"},
        {"WS_HOST", "wss://ws.plumbing.test/ws/market"},
        {"GAMMA_HOST", "https://gamma.plumbing.test"},
        {"BOT_USER_WS_HOST", "wss://ws.plumbing.test/ws/user"},
        {"POLYGON_RPC_URL", "https://rpc.plumbing.test"},
        {"POLYGON_RPC_BACKUP_URL", "https://rpc2.plumbing.test"},
        {"BOT_LEDGER_DIR", "/tmp/crowdintel-plumbing-ledger"},
        {"BOT_PREFLIGHT_TOKEN_FILE", "/tmp/crowdintel-plumbing.pass"},
        {"BOT_MAKER_ADDRESS", "2222222222222222222222222222222222222222"},
        {"BOT_API_ADDRESS", "3333333333333333333333333333333333333333"},
    };
    const char* numbers[][2] = {
        {"BOT_NEG_RISK", "1"},
        {"BOT_SIGNATURE_TYPE", "1"},
        {"BOT_GTD_TTL_SECONDS", "600"},
        {"BOT_TICK_SIZE", "0.005"},
        {"BOT_TICKS", "2500"},
        {"BOT_BANKROLL_USD", "2500"},
        {"BOT_KELLY_FRACTION", "0.05"},
        {"BOT_MIN_EDGE", "0.03"},
        {"BOT_MIN_CONFIDENCE", "0.9"},
        {"BOT_MAX_Q_VALUE", "0.04"},
        {"BOT_TAKER_FEE_RATE", "0.04"},
        {"BOT_MAX_TAKER_FEE_RATE", "0.05"},
        {"BOT_MAX_ORDER_USD", "42.5"},
        {"BOT_MAX_EXPOSURE_USD", "321"},
        {"BOT_MAX_DAILY_LOSS_USD", "77"},
        {"BOT_MIN_SIZE_SHARES", "7.5"},
        {"BOT_INITIAL_POSITION_SHARES", "15"},
        {"BOT_PRESIGN_TTL_MS", "2500"},
        {"BOT_SIGNAL_TTL_MS", "1200"},
        {"BOT_MAX_BOOK_AGE_MS", "1500"},
        {"BOT_ALPHA_PORT", "9911"},
        {"BOT_PIN_CPU", "1"},
        {"BOT_COLD_CPU", "2"},
        {"BOT_LEDGER_FSYNC", "0"},
        {"BOT_LEDGER_CHECKPOINT_EVERY", "256"},
        {"BOT_RECON_MAX_PAGES", "3"},
        {"BOT_PREFLIGHT_MAX_AGE_S", "600"},
        {"BOT_MAX_CLOCK_SKEW_S", "5"},
        {"BOT_MIN_COLLATERAL", "2.5"},
        {"BOT_TARGET_ALLOWANCE", "12.25"},
        {"BOT_USER_WS_ENABLED", "1"},
        {"BOT_USER_WS_KEEPALIVE_MS", "7000"},
        {"BOT_USER_WS_IDLE_MS", "25000"},
        {"BOT_USER_WS_PONG_MS", "60000"},
        {"BOT_USER_WS_RECONNECT_MIN_MS", "400"},
        {"BOT_USER_WS_RECONNECT_MAX_MS", "8000"},
        {"BOT_HEARTBEAT_ENABLED", "1"},
        {"BOT_HEARTBEAT_INTERVAL_MS", "4000"},
        {"BOT_HEARTBEAT_WARN_MS", "6000"},
        {"BOT_HEARTBEAT_BLOCK_MS", "8000"},
        {"BOT_HEARTBEAT_ASSUME_CANCELLED_MS", "9000"},
        {"BOT_HEARTBEAT_MAX_FAILURES", "3"},
        {"BOT_METADATA_MAX_AGE_MS", "60000"},
        {"BOT_METADATA_REFRESH_MS", "20000"},
        {"BOT_STRICT_ENV", "1"},
        {"BOT_ALLOW_PROTOCOL_V2", "0"},
        {"BOT_PREFLIGHT_CHECK_L1", "1"},
        {"BOT_PREFLIGHT_CHECK_USER_WS", "1"},
        {"BOT_PREFLIGHT_CHECK_HEARTBEAT", "1"},
        {"BOT_ENABLE_LIVE_TRADING", "0"},
        {"BOT_SESSION_TIMEOUT_MS", "600000"},
    };
    for (const auto& entry : text) setenv(entry[0], entry[1], 1);
    for (const auto& entry : numbers) setenv(entry[0], entry[1], 1);
    setenv("BOT_PRIVATE_KEY_HEX", KEY, 1);

    MarketConfig cfg;
    const char* error = cfg.load(false, false);
    CHECK(error == nullptr, error ? error : "every knob at a valid non-default value loads");
    if (error != nullptr) {
        for (const auto& entry : text) unsetenv(entry[0]);
        for (const auto& entry : numbers) unsetenv(entry[0]);
        unsetenv("BOT_PRIVATE_KEY_HEX");
        return;
    }

    // Identity and market
    CHECK(cfg.bot_mode == BotMode::PAPER, "BOT_MODE");
    CHECK(std::strcmp(cfg.market_slug, "plumbing-market") == 0, "BOT_MARKET_SLUG");
    CHECK(std::strcmp(cfg.condition_id, CONDITION) == 0, "BOT_CONDITION_ID");
    CHECK(std::strcmp(cfg.token_id_dec, TOKEN) == 0, "BOT_TOKEN_ID");
    CHECK(!cfg.token_id_from_metadata, "BOT_TOKEN_ID provenance");
    CHECK(cfg.neg_risk, "BOT_NEG_RISK");
    CHECK(cfg.signature_type == 1, "BOT_SIGNATURE_TYPE");
    CHECK(std::strcmp(cfg.order_type, "GTD") == 0, "BOT_ORDER_TYPE");
    CHECK(cfg.gtd_ttl_seconds == 600, "BOT_GTD_TTL_SECONDS");
    CHECK(cfg.wire_expiration(1000000) == 1000000 + 60 + 600,
          "BOT_GTD_TTL_SECONDS reaches the wire expiration");
    CHECK(cfg.tick_size == 5000, "BOT_TICK_SIZE");
    CHECK(cfg.max_ticks == 2500, "BOT_TICKS");

    // Risk and sizing
    CHECK(cfg.bankroll_usd == 2500.0, "BOT_BANKROLL_USD");
    CHECK(cfg.kelly_fraction == 0.05, "BOT_KELLY_FRACTION");
    CHECK(cfg.min_edge == 0.03, "BOT_MIN_EDGE");
    CHECK(cfg.min_confidence == 0.9, "BOT_MIN_CONFIDENCE");
    CHECK(cfg.max_q_value == 0.04, "BOT_MAX_Q_VALUE");
    CHECK(cfg.taker_fee_rate == 0.04, "BOT_TAKER_FEE_RATE");
    CHECK(cfg.max_taker_fee_rate == 0.05, "BOT_MAX_TAKER_FEE_RATE");
    CHECK(cfg.max_order_usd == 42.5, "BOT_MAX_ORDER_USD");
    CHECK(cfg.max_exposure_usd == 321.0, "BOT_MAX_EXPOSURE_USD");
    CHECK(cfg.max_daily_loss_usd == 77.0, "BOT_MAX_DAILY_LOSS_USD");
    CHECK(cfg.min_size_shares == 7500000, "BOT_MIN_SIZE_SHARES");
    CHECK(cfg.initial_position_shares == 15000000, "BOT_INITIAL_POSITION_SHARES");
    CHECK(cfg.presign_ttl_ms == 2500, "BOT_PRESIGN_TTL_MS");
    CHECK(cfg.signal_ttl_ms == 1200, "BOT_SIGNAL_TTL_MS");
    CHECK(cfg.max_book_age_ms == 1500, "BOT_MAX_BOOK_AGE_MS");

    // Transport and runtime
    CHECK(std::strcmp(cfg.alpha_bind, "127.0.0.1") == 0, "BOT_ALPHA_BIND");
    CHECK(cfg.alpha_port == 9911, "BOT_ALPHA_PORT");
    CHECK(cfg.pin_cpu == 1, "BOT_PIN_CPU");
    CHECK(cfg.cold_cpu == 2, "BOT_COLD_CPU");
    CHECK(std::strcmp(cfg.kill_switch_file, "/tmp/crowdintel-plumbing.kill") == 0,
          "BOT_KILL_SWITCH_FILE");
    CHECK(std::strcmp(cfg.tls_pin, pin.c_str()) == 0, "BOT_TLS_PIN");
    CHECK(std::strcmp(cfg.clob_host, "https://clob.plumbing.test") == 0, "CLOB_HOST");
    CHECK(std::strcmp(cfg.ws_host, "wss://ws.plumbing.test/ws/market") == 0, "WS_HOST");
    CHECK(std::strcmp(cfg.gamma_host, "https://gamma.plumbing.test") == 0, "GAMMA_HOST");
    CHECK(std::strcmp(cfg.user_ws_host, "wss://ws.plumbing.test/ws/user") == 0,
          "BOT_USER_WS_HOST");
    CHECK(std::strcmp(cfg.polygon_rpc_url, "https://rpc.plumbing.test") == 0,
          "POLYGON_RPC_URL");
    CHECK(std::strcmp(cfg.polygon_rpc_backup_url, "https://rpc2.plumbing.test") == 0,
          "POLYGON_RPC_BACKUP_URL");
    CHECK(cfg.session_timeout_ms == 600000, "BOT_SESSION_TIMEOUT_MS");

    // Ledger, reconciliation, preflight
    CHECK(std::strcmp(cfg.ledger_dir, "/tmp/crowdintel-plumbing-ledger") == 0,
          "BOT_LEDGER_DIR");
    CHECK(!cfg.ledger_fsync, "BOT_LEDGER_FSYNC");
    CHECK(cfg.ledger_checkpoint_every == 256, "BOT_LEDGER_CHECKPOINT_EVERY");
    CHECK(cfg.recon_max_pages == 3, "BOT_RECON_MAX_PAGES");
    CHECK(std::strcmp(cfg.preflight_token_file, "/tmp/crowdintel-plumbing.pass") == 0,
          "BOT_PREFLIGHT_TOKEN_FILE");
    CHECK(cfg.preflight_max_age_s == 600, "BOT_PREFLIGHT_MAX_AGE_S");
    CHECK(cfg.max_clock_skew_s == 5, "BOT_MAX_CLOCK_SKEW_S");
    CHECK(cfg.min_collateral_base == 2500000, "BOT_MIN_COLLATERAL");
    CHECK(cfg.target_allowance_base == 12250000, "BOT_TARGET_ALLOWANCE");
    CHECK(cfg.preflight_check_l1 && cfg.preflight_check_user_ws &&
              cfg.preflight_check_heartbeat,
          "BOT_PREFLIGHT_CHECK_*");

    // User channel and heartbeat: the thresholds that decide how fast a lost
    // venue connection blocks trading.
    CHECK(cfg.user_ws_enabled, "BOT_USER_WS_ENABLED");
    CHECK(cfg.user_ws_keepalive_ms == 7000, "BOT_USER_WS_KEEPALIVE_MS");
    CHECK(cfg.user_ws_idle_ms == 25000, "BOT_USER_WS_IDLE_MS");
    CHECK(cfg.user_ws_pong_ms == 60000, "BOT_USER_WS_PONG_MS");
    CHECK(cfg.user_ws_reconnect_min_ms == 400, "BOT_USER_WS_RECONNECT_MIN_MS");
    CHECK(cfg.user_ws_reconnect_max_ms == 8000, "BOT_USER_WS_RECONNECT_MAX_MS");
    CHECK(cfg.heartbeat_enabled, "BOT_HEARTBEAT_ENABLED");
    CHECK(cfg.heartbeat_interval_ms == 4000, "BOT_HEARTBEAT_INTERVAL_MS");
    CHECK(cfg.heartbeat_warn_ms == 6000, "BOT_HEARTBEAT_WARN_MS");
    CHECK(cfg.heartbeat_block_ms == 8000, "BOT_HEARTBEAT_BLOCK_MS");
    CHECK(cfg.heartbeat_assume_cancelled_ms == 9000,
          "BOT_HEARTBEAT_ASSUME_CANCELLED_MS");
    CHECK(cfg.heartbeat_max_failures == 3, "BOT_HEARTBEAT_MAX_FAILURES");
    CHECK(cfg.metadata_max_age_ms == 60000, "BOT_METADATA_MAX_AGE_MS");
    CHECK(cfg.metadata_refresh_ms == 20000, "BOT_METADATA_REFRESH_MS");
    CHECK(cfg.strict_env, "BOT_STRICT_ENV");
    CHECK(!cfg.allow_protocol_v2_positions, "BOT_ALLOW_PROTOCOL_V2");
    CHECK(!cfg.live_armed, "BOT_ENABLE_LIVE_TRADING");

    // BOT_MAKER_ADDRESS and BOT_API_ADDRESS are read by finalize_identity, not by
    // load: they must reach the fields the signer and the L2 headers use.
    uint8_t signer_address[20];
    for (uint8_t& byte : signer_address) byte = 0x44;
    CHECK(cfg.finalize_identity(signer_address) == nullptr, "identity binds");
    CHECK(std::strcmp(cfg.maker_hex,
                      "0x2222222222222222222222222222222222222222") == 0,
          "BOT_MAKER_ADDRESS");
    CHECK(std::strcmp(cfg.api_address_hex,
                      "0x3333333333333333333333333333333333333333") == 0,
          "BOT_API_ADDRESS");
    CHECK(std::strcmp(cfg.signer_hex,
                      "0x4444444444444444444444444444444444444444") == 0,
          "the derived signer address is formatted");

    for (const auto& entry : text) unsetenv(entry[0]);
    for (const auto& entry : numbers) unsetenv(entry[0]);
    unsetenv("BOT_PRIVATE_KEY_HEX");
}

int main() {
    std::printf("== CROWDINTEL core unit tests ==\n");
    test_config_fail_closed();
    test_secret_file_loading();
    test_alpha_http_receiver();
    test_order_gateway();
    test_presigned_pool_concurrency();
    test_signal_deduplication();
    test_fee_in_cost_cap();
#ifdef CROWDINTEL_HAVE_NETWORK
    test_clob_responses();
    test_ws_parsing();
#endif
    test_alpha_parser();
    test_spsc_basic();
    test_spsc_fifo_order();
    test_spsc_multithread();
    test_order_book();
    test_kelly();
    test_amounts();
    test_buy_market_rounding();  // M1: ceil_to_quantum for BUY market orders
    test_parsing();
    test_base64_hmac();
    test_salt_uniqueness();
    test_signer_key_validation();
    test_wire_body();
    test_config_env_plumbing();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
