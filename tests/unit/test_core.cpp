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
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/crypto/sha256_engine.hpp"
#include "../../core/crypto/fast_random.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/polymarket_order.hpp"
#include "../../core/src/presigned_pool.hpp"
#include "../../core/src/ws_market_listener.hpp"
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
    Level2Entry bids[3] = {{470000, 1000000}, {460000, 2000000}, {450000, 3000000}};
    Level2Entry asks[3] = {{530000, 1000000}, {540000, 2000000}};
    book.set_bids(bids, 3);
    book.set_asks(asks, 2);

    OrderBookL2::Top top{};
    CHECK(book.read_top(top), "seqlock read succeeds");
    CHECK(top.bid.price == 470000 && top.ask.price == 530000, "best levels correct");
    CHECK(top.ask.size == 1000000, "ask size correct");
    CHECK(book.get_bid(999).size == 0, "out-of-range returns sentinel (no UB)");
    CHECK(book.sequence() >= 4, "sequence advances on writes");

    // Torn-read detection under concurrent writes: read_top must never return
    // an inconsistent (bid>ask) pair without reporting failure... it reports
    // failure instead. Run writer + reader and count failures.
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> false_reads{0}, torn{0};
    std::thread writer([&] {
        uint64_t i = 0;
        while (!stop.load()) {
            Level2Entry b[1] = {{470000 + (i % 10), 1000000}};
            Level2Entry a[1] = {{530000 + (i % 10), 1000000}};
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
    CHECK(KellyEngine::usd_to_shares_fixed(500.0, 0.5) == 1000000000ULL, "usd→shares fixed-point");
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
    CHECK(ma == 5300000 && ta == 10000000, "BUY: maker=USDC 5.50, taker=10 shares");
    CHECK(compute_amounts(K_SIDE_SELL, 530000, 10000000, ma, ta), "sell amounts ok");
    CHECK(ma == 10000000 && ta == 5300000, "SELL: maker=shares, taker=USDC");
    CHECK(!compute_amounts(K_SIDE_BUY, 0, 1000000, ma, ta), "zero price rejected");
    CHECK(!compute_amounts(K_SIDE_BUY, 1000000, 1000000, ma, ta), "price≥1 rejected");
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
    CHECK(base64url_decode("-_-_", 4, dec) == 3 && dec[0] == 0xfb, "b64url decode round trip");
    CHECK(base64url_decode("!!!", 3, dec) == SIZE_MAX, "invalid char rejected");

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
}

// ── Salt uniqueness ──────────────────────────────────────────────────────────
static void test_salt_uniqueness() {
    std::printf("salt_uniqueness\n");
    FastRandom r;
    std::set<uint64_t> seen;
    bool unique = true;
    for (int i = 0; i < 200000; ++i) {
        const uint64_t s = r.next_salt();
        if (!seen.insert(s).second) { unique = false; break; }
    }
    CHECK(unique, "200k salts, zero collisions");
}

// ── Wire body golden build ───────────────────────────────────────────────────
static void test_wire_body() {
    std::printf("wire_body\n");
    // Reproduce the Python golden vector exactly (fixed salt/timestamp/sizes).
    uint8_t maker[20];
    parse_hex_bytes("26972a79b73e93a0374afabd80302d19638051c9", 42, maker, 20);
    OrderV2 o{};
    o.salt = 123456789012345678ULL;
    std::memcpy(o.maker, maker, 20);
    std::memcpy(o.signer, maker, 20);
    const char* tok = "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    parse_uint256_dec(tok, std::strlen(tok), o.token_id);
    o.maker_amount = 5500000;
    o.taker_amount = 10000000;
    o.side = K_SIDE_BUY;
    o.signature_type = 0;
    o.timestamp_ms = 1758528000000ULL;

    uint8_t sig[65];
    parse_hex_bytes("2d1a7edd096f2073b55765c38cf5f575273f8163a3e16eb2099524c03a6261df"
                    "4ddc6f54a6e8e39df1c6bdd311bf916a7c20b3afdc7c8dbd01a40a836baded821c",
                    130, sig, 65);

    WireBody body;
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

// ── WSS market-message parsing (against real doc examples) ───────────────────
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
        "{\"event_type\":\"price_change\",\"asset_id\":\"65818...\","
        "\"changes\":[{\"price\":\".49\",\"side\":\"BUY\",\"size\":\"0\"},"
                     "{\"price\":\".47\",\"side\":\"BUY\",\"size\":\"10\"},"
                     "{\"price\":\".54\",\"side\":\"SELL\",\"size\":\"5\"}]}";
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
}

int main() {
    std::printf("== CROWDINTEL core unit tests ==\n");
    test_ws_parsing();
    test_spsc_basic();
    test_spsc_fifo_order();
    test_spsc_multithread();
    test_order_book();
    test_kelly();
    test_amounts();
    test_parsing();
    test_base64_hmac();
    test_salt_uniqueness();
    test_wire_body();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
