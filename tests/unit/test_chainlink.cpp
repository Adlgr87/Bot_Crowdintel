// ─────────────────────────────────────────────────────────────────────────────
// test_chainlink: Chainlink Data Streams ingestion + TWAP-60s replica
//
// Verifies:
//   1. scaled-price parsing accepts both live fixed-point strings (18 decimals)
//      and plain decimals, and rejects anything exotic
//   2. the report parser is strict: a missing clock or a missing price is an
//      error, never a silent zero (a zero price would look like a real quote)
//   3. the TWAP is TIME-WEIGHTED, not an arithmetic mean — the DON's cadence is
//      uneven, so a plain mean would misprice the settlement value
//   4. the window clips the first partial interval and excludes stale samples
//   5. out-of-order samples are refused rather than silently clipped
//   6. the replay source handles comments, blank lines and end of file
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "../../core/src/chainlink_stream.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
                ++g_failures; }                                                \
    } while (0)

using namespace crowdintel;

static bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

static constexpr uint64_t NS = 1000000000ULL;  // one second in nanoseconds

// ── 1. Scaled prices ─────────────────────────────────────────────────────────

static void test_scaled_price() {
    std::printf("scaled_price\n");
    double v = 0.0;

    // Live Data Streams transport: fixed-point integer, 18 decimals.
    CHECK(parse_scaled_price("631234500000000000000", 21, 1e18, v) &&
              approx(v, 631.2345),
          "18-decimal fixed point divides by 1e18");

    // Fixtures and human-written replays use plain decimals.
    CHECK(parse_scaled_price("631.2345", 8, 1e18, v) && approx(v, 631.2345),
          "a token containing '.' is read as a decimal regardless of scale");

    CHECK(parse_scaled_price("0.5", 3, 1.0, v) && approx(v, 0.5),
          "fractional decimal");
    CHECK(!parse_scaled_price("", 0, 1e18, v), "empty is rejected");
    CHECK(!parse_scaled_price("1e18", 4, 1e18, v), "exponent notation is rejected");
    CHECK(!parse_scaled_price("  12", 4, 1e18, v), "leading space is rejected");
    CHECK(!parse_scaled_price("12,5", 4, 1e18, v), "comma decimal is rejected");
    CHECK(!parse_scaled_price("abc", 3, 1e18, v), "letters are rejected");
    CHECK(!parse_scaled_price("12", 2, 0.0, v), "scale 0 with an integer is rejected");
}

// ── 2. Report parser ─────────────────────────────────────────────────────────

static void test_parse_report() {
    std::printf("parse_report\n");
    ChainlinkReport r{};

    // Full live-shaped report (report prices are fixed-point strings).
    const char* live =
        R"({"feedID":"0x00039d9e45394f473ab1f050a1b963e6b05351e52d71e5e177b4ea4bbf0dc6d3",)"
        R"("validFromTimestamp":1793271600,"observationsTimestamp":1793271590,)"
        R"("fullReport":"0xabc","report":{"feedID":"0x0003",)"
        R"("observationsTimestamp":1793271590,"benchmarkPrice":"631234500000000000000",)"
        R"("bid":"631200000000000000000","ask":"631300000000000000000"}})";
    CHECK(parse_chainlink_report(live, std::strlen(live), 1e18, r) ==
              ChainlinkParse::Ok,
          "live-shaped report parses");
    CHECK(r.observations_timestamp == 1793271590, "observation timestamp read");
    CHECK(approx(r.benchmark_price, 631.2345), "benchmark price normalised to USD");
    CHECK(approx(r.bid, 631.2), "bid read");
    CHECK(approx(r.ask, 631.3), "ask read");
    CHECK(r.feed_hash != 0, "feed identity hashed");

    // Minimal fixture: plain decimals, "price" alias, no optional fields.
    const char* minimal =
        R"({"observationsTimestamp":1793271590,"price":"631.50"})";
    CHECK(parse_chainlink_report(minimal, std::strlen(minimal), 1e18, r) ==
              ChainlinkParse::Ok,
          "minimal fixture with the 'price' alias parses");
    CHECK(approx(r.benchmark_price, 631.50), "alias price read as decimal");

    // Strictness: without a clock the sample has no place on the time axis.
    const char* no_clock = R"({"benchmarkPrice":"631.5"})";
    CHECK(parse_chainlink_report(no_clock, std::strlen(no_clock), 1e18, r) ==
              ChainlinkParse::MissingField,
          "missing observationsTimestamp is an error");

    // Strictness: without a price there is nothing to model. It must NOT
    // default to zero, which would look like a valid quote.
    const char* no_price = R"({"observationsTimestamp":1793271590})";
    CHECK(parse_chainlink_report(no_price, std::strlen(no_price), 1e18, r) ==
              ChainlinkParse::MissingField,
          "missing price is an error, not a zero");

    const char* zero_price =
        R"({"observationsTimestamp":1793271590,"benchmarkPrice":"0"})";
    CHECK(parse_chainlink_report(zero_price, std::strlen(zero_price), 1e18, r) ==
              ChainlinkParse::BadNumber,
          "a non-positive price is rejected");

    const char* broken = R"({"observationsTimestamp":1793271590,)";
    CHECK(parse_chainlink_report(broken, std::strlen(broken), 1e18, r) ==
              ChainlinkParse::NotJson,
          "malformed JSON is rejected");

    CHECK(parse_chainlink_report(nullptr, 0, 1e18, r) == ChainlinkParse::NotJson,
          "null payload is rejected");
}

// ── 3. TWAP is time-weighted ─────────────────────────────────────────────────

static void test_twap() {
    std::printf("twap\n");
    TwapWindow w;
    w.configure(60 * NS);

    // Constant price: any correct mean returns the price.
    for (uint64_t t = 0; t <= 60; t += 10) (void)w.push(t * NS, 100.0);
    CHECK(w.ready(60 * NS), "window is ready after 60 s of history");
    CHECK(approx(w.twap(60 * NS), 100.0), "constant series -> TWAP == price");

    // A step halfway through. Arithmetic mean of the 7 samples would be
    // (4*100 + 3*200)/7 = 142.86; the TIME-weighted answer is 133.33.
    TwapWindow step;
    step.configure(60 * NS);
    for (uint64_t t = 0; t <= 30; t += 10) (void)step.push(t * NS, 100.0);
    for (uint64_t t = 40; t <= 60; t += 10) (void)step.push(t * NS, 200.0);
    const double step_twap = step.twap(60 * NS);
    CHECK(approx(step_twap, 8000.0 / 60.0),
          "step series -> time-weighted mean 133.33");
    CHECK(!approx(step_twap, 1000.0 / 7.0), "not the arithmetic mean");

    // Stale samples outside the lookback contribute nothing.
    TwapWindow clip_stale;
    clip_stale.configure(60 * NS);
    for (uint64_t t = 0; t < 60; t += 10) (void)clip_stale.push(t * NS, 100.0);
    for (uint64_t t = 60; t <= 120; t += 10) (void)clip_stale.push(t * NS, 200.0);
    CHECK(approx(clip_stale.twap(120 * NS), 200.0),
          "samples older than the lookback are excluded");
}

// ── 4. Window clipping of the first partial interval ─────────────────────────

static void test_twap_clipping() {
    std::printf("twap_clipping\n");
    TwapWindow w;
    w.configure(30 * NS);
    (void)w.push(0, 1000.0);
    (void)w.push(20 * NS, 100.0);
    (void)w.push(40 * NS, 100.0);

    // Window at t=40 is [10,40]: sample@0 covers [0,20] clipped to [10,20] = 10 s
    // at 1000, then 20 s at 100 -> (10000 + 2000) / 30 = 400.
    CHECK(approx(w.twap(40 * NS), 400.0),
          "first in-window interval is clipped to the lookback edge");
    // Without clipping it would be (1000*20 + 100*20) / 40 = 550.
    CHECK(!approx(w.twap(40 * NS), 550.0), "clipping actually happened");
}

// ── 5. Rejections ────────────────────────────────────────────────────────────

static void test_rejections() {
    std::printf("rejections\n");
    TwapWindow w;
    w.configure(60 * NS);
    CHECK(!w.ready(0), "empty window is not ready");
    CHECK(approx(w.twap(0), 0.0), "empty window has no TWAP");

    CHECK(w.push(10 * NS, 100.0), "first sample accepted");
    CHECK(!w.push(10 * NS, 101.0), "duplicate timestamp refused");
    CHECK(!w.push(5 * NS, 99.0), "out-of-order timestamp refused");
    CHECK(w.push(20 * NS, 100.0), "forward sample accepted");
    CHECK(!w.push(30 * NS, -1.0), "negative price refused");
    CHECK(!w.push(30 * NS, 0.0), "zero price refused");
    CHECK(!w.ready(20 * NS), "only 10 s of history is not a 60 s window");
    CHECK(w.samples() == 2, "exactly two samples stored");
}

// ── 6. Replay source ─────────────────────────────────────────────────────────

static void test_replay() {
    std::printf("replay\n");
    char path[] = "/tmp/cdt_cl_replay_XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0, "replay fixture created");
    if (fd < 0) return;

    const char* body =
        "# comment line\n"
        "\n"
        R"({"observationsTimestamp":1000,"price":"100.5"})" "\n"
        R"({"observationsTimestamp":1010,"price":"101.5"})" "\n";
    CHECK(write(fd, body, std::strlen(body)) ==
              static_cast<ssize_t>(std::strlen(body)),
          "replay fixture written");
    close(fd);

    ChainlinkReplay replay;
    CHECK(replay.open(path), "replay file opens");
    ChainlinkReport r{};
    CHECK(replay.next(1.0, r) == ChainlinkParse::Ok &&
              r.observations_timestamp == 1000 && approx(r.benchmark_price, 100.5),
          "first record read, comment and blank line skipped");
    CHECK(replay.next(1.0, r) == ChainlinkParse::Ok &&
              r.observations_timestamp == 1010,
          "second record read");
    CHECK(replay.next(1.0, r) == ChainlinkParse::NotJson, "end of file signalled");

    ChainlinkReplay missing;
    CHECK(!missing.open("/tmp/definitely-not-a-cdt-file"), "absent file fails to open");
    unlink(path);
}

int main() {
    std::printf("=== test_chainlink ===\n");

    test_scaled_price();
    test_parse_report();
    test_twap();
    test_twap_clipping();
    test_rejections();
    test_replay();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
