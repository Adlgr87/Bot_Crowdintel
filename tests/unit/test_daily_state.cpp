// ─────────────────────────────────────────────────────────────────────────────
// test_daily_state: Fase 2 durable daily risk state
//
// Verifies:
//   1. CRC32 matches the canonical IEEE 802.3 check vector
//   2. save -> load round-trips every persisted field
//   3. a corrupt magic, a corrupt CRC, and a truncated file are all reported
//      as Corrupt rather than silently accepted
//   4. a missing file is reported as Missing (legitimate first run), and an
//      unreadable path as IoError
//   5. the write is atomic: no temporary file survives a successful save
//   6. a symlink at the state path is refused (O_NOFOLLOW)
//   7. `seq` advances on every write
//
// This is the layer that stops a restart from re-arming the daily loss budget
// and from clearing an operator kill.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#include "../../core/include/daily_state.hpp"
#include "../../core/src/execution_engine.hpp"

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

static std::string tmp_path(const char* tag) {
    return std::string("/tmp/cdt_state_") + tag + "_" +
           std::to_string(static_cast<long>(::getpid())) + ".bin";
}

static bool file_exists(const std::string& p) {
    return ::access(p.c_str(), F_OK) == 0;
}

// ── 1. CRC ───────────────────────────────────────────────────────────────────

static void test_crc() {
    std::printf("crc\n");
    // Canonical check value for CRC-32/ISO-HDLC.
    const char* check = "123456789";
    CHECK(daily_state_crc32(check, 9) == 0xCBF43926u,
          "CRC32(\"123456789\") == 0xCBF43926");
    CHECK(daily_state_crc32("", 0) == 0u, "CRC32 of empty input is 0");
    const char a[] = "abcd";
    const char b[] = "abce";
    CHECK(daily_state_crc32(a, 4) != daily_state_crc32(b, 4),
          "one-bit change alters the CRC");
}

// ── 2. Round trip ────────────────────────────────────────────────────────────

static void test_roundtrip() {
    std::printf("roundtrip\n");
    const std::string path = tmp_path("rt");
    ::unlink(path.c_str());

    DailyStateRecord out{};
    CHECK(load_daily_state(path.c_str(), out) == DailyStateLoad::Missing,
          "absent file reports Missing (first run)");

    DailyStateRecord rec{};
    rec.utc_day = 20767;
    rec.daily_buy_volume_usd = 1234.56;
    rec.kill_latched = 0;
    rec.day_kill_latched = 0;
    CHECK(save_daily_state(path.c_str(), rec), "save succeeds");

    DailyStateRecord in{};
    CHECK(load_daily_state(path.c_str(), in) == DailyStateLoad::Loaded,
          "load reports Loaded");
    CHECK(in.utc_day == 20767, "utc_day round-trips");
    CHECK(in.daily_buy_volume_usd == 1234.56, "buy volume round-trips");
    CHECK(in.kill_latched == 0 && in.day_kill_latched == 0, "latches round-trip");
    CHECK(in.magic == DAILY_STATE_MAGIC, "magic is stamped on write");
    CHECK(in.version == DAILY_STATE_VERSION, "version is stamped on write");
    CHECK(in.crc == daily_state_record_crc(in), "written CRC verifies");

    // A second save must announce a higher sequence number.
    const uint64_t first_seq = in.seq;
    CHECK(save_daily_state(path.c_str(), rec), "second save succeeds");
    CHECK(load_daily_state(path.c_str(), in) == DailyStateLoad::Loaded,
          "second load succeeds");
    CHECK(in.seq == first_seq + 1, "seq advances on every write");

    // Latches survive.
    rec.kill_latched = 1;
    rec.day_kill_latched = 1;
    CHECK(save_daily_state(path.c_str(), rec), "save with latches succeeds");
    CHECK(load_daily_state(path.c_str(), in) == DailyStateLoad::Loaded,
          "load with latches succeeds");
    CHECK(in.kill_latched == 1, "kill latch persists");
    CHECK(in.day_kill_latched == 1, "day-kill latch persists");

    ::unlink(path.c_str());
}

// ── 3. Corruption detection ──────────────────────────────────────────────────

static void test_corruption() {
    std::printf("corruption\n");
    const std::string path = tmp_path("corrupt");

    // (a) Wrong magic.
    {
        DailyStateRecord rec{};
        rec.utc_day = 1;
        CHECK(save_daily_state(path.c_str(), rec), "baseline save");
        int fd = ::open(path.c_str(), O_WRONLY);
        CHECK(fd >= 0, "open raw for tampering");
        const uint32_t junk = 0xDEADBEEFu;
        (void)::pwrite(fd, &junk, sizeof(junk), 0);
        ::close(fd);
        DailyStateRecord in{};
        CHECK(load_daily_state(path.c_str(), in) == DailyStateLoad::Corrupt,
              "bad magic is Corrupt");
    }

    // (b) CRC mismatch: flip a byte inside the payload, magic left intact.
    {
        ::unlink(path.c_str());
        DailyStateRecord rec{};
        rec.utc_day = 1;
        rec.daily_buy_volume_usd = 10.0;
        CHECK(save_daily_state(path.c_str(), rec), "baseline save");
        int fd = ::open(path.c_str(), O_RDWR);
        CHECK(fd >= 0, "open raw for tampering");
        const off_t vol_off = static_cast<off_t>(offsetof(DailyStateRecord,
                                                         daily_buy_volume_usd));
        const double tampered = 99999.0;
        (void)::pwrite(fd, &tampered, sizeof(tampered), vol_off);
        ::close(fd);
        DailyStateRecord in{};
        CHECK(load_daily_state(path.c_str(), in) == DailyStateLoad::Corrupt,
              "tampered payload fails the CRC");
    }

    // (c) Truncated file.
    {
        ::unlink(path.c_str());
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(fd >= 0, "create truncated file");
        const char partial[8] = {'C', 'D', 'S', 'T', 1, 0, 0, 0};
        (void)::write(fd, partial, sizeof(partial));
        ::close(fd);
        DailyStateRecord in{};
        CHECK(load_daily_state(path.c_str(), in) == DailyStateLoad::Corrupt,
              "short file is Corrupt");
    }

    // (d) A directory path is an I/O error, not a silent success.
    {
        DailyStateRecord in{};
        CHECK(load_daily_state("/tmp", in) == DailyStateLoad::IoError,
              "directory path reports IoError");
    }

    ::unlink(path.c_str());
}

// ── 4. Atomicity and symlink refusal ────────────────────────────────────────

static void test_atomicity() {
    std::printf("atomicity\n");
    const std::string path = tmp_path("atomic");
    ::unlink(path.c_str());

    DailyStateRecord rec{};
    rec.utc_day = 42;
    CHECK(save_daily_state(path.c_str(), rec), "save succeeds");

    // The tmpfile must be renamed away, never left behind.
    const std::string tmp = path + ".tmp." +
                            std::to_string(static_cast<long>(::getpid()));
    CHECK(!file_exists(tmp), "no temporary file survives a successful save");
    CHECK(file_exists(path), "state file exists after save");

    // A symlink at the destination must be refused by O_NOFOLLOW: this is the
    // same threat model as the kill switch (an attacker redirects the file).
    const std::string link = tmp_path("link");
    const std::string victim = tmp_path("victim");
    ::unlink(link.c_str());
    ::unlink(victim.c_str());
    int vfd = ::open(victim.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (vfd >= 0) ::close(vfd);
    if (::symlink(victim.c_str(), link.c_str()) == 0) {
        DailyStateRecord in{};
        CHECK(load_daily_state(link.c_str(), in) == DailyStateLoad::IoError,
              "symlinked state path is refused on read");
        ::unlink(link.c_str());
    } else {
        std::printf("  SKIP symlink check (cannot create symlink here)\n");
    }
    ::unlink(victim.c_str());
    ::unlink(path.c_str());
}

// ── 5. Parent directory helper ──────────────────────────────────────────────

static void test_parent_dir() {
    std::printf("parent_dir\n");
    char buf[256];
    daily_state_parent_dir("/var/lib/crowdintel/daily_state.bin", buf, sizeof(buf));
    CHECK(std::strcmp(buf, "/var/lib/crowdintel") == 0, "nested path");
    daily_state_parent_dir("state.bin", buf, sizeof(buf));
    CHECK(std::strcmp(buf, ".") == 0, "bare filename -> current dir");
    daily_state_parent_dir("/state.bin", buf, sizeof(buf));
    CHECK(std::strcmp(buf, "/") == 0, "root-level file -> /");
}

// ── 6. utc_day_number ──────────────────────────────────────────────────────
// The value gates the daily loss-cap rollover, so it must be the real day
// count since the epoch: monotonic and collision-free.  The previous revision
// returned era*36524 + doe (192358 on 2026-10-29 instead of ~20755), whose
// ranges overlap between eras.
static void test_utc_day_number() {
    std::printf("utc_day_number\n");
    const uint64_t got = utc_day_number();
    const uint64_t expected =
        static_cast<uint64_t>(std::time(nullptr)) / 86400ULL;
    const uint64_t delta = got > expected ? got - expected : expected - got;
    CHECK(delta <= 1,
          "utc_day_number equals days-since-epoch (allows a midnight race)");
    CHECK(got > 20000ULL && got < 100000ULL,
          "utc_day_number is in the plausible range for this century");
}

int main() {
    std::printf("=== test_daily_state (Fase 2) ===\n");

    test_crc();
    test_roundtrip();
    test_corruption();
    test_atomicity();
    test_parent_dir();
    test_utc_day_number();

    std::printf("\n--- Results: %d/%d passed, %d failed ---\n",
                g_tests - g_failures, g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
