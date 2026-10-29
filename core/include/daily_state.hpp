#ifndef CROWDINTEL_DAILY_STATE_HPP
#define CROWDINTEL_DAILY_STATE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// daily_state.hpp — durable risk state across process restarts (Fase 2).
//
// THE DEFECT THIS CLOSES
// `daily_buy_volume_` was a plain atomic<double> in RAM and the kill latch was
// a plain atomic<bool>.  Neither was ever written anywhere, so a restart
// silently re-armed the whole daily loss budget and cleared an operator kill.
// With max_daily_loss_usd = 50 and a crash loop, every restart authorised
// another 50 USD of loss while the kill switch worked perfectly inside each
// individual cycle.
//
// DESIGN
//   * one fixed-size POD record, CRC-protected, versioned;
//   * write = tmpfile -> write -> fsync -> rename -> fsync(dir): a crash either
//     leaves the previous record intact or the new one complete, never a torn
//     file;
//   * O_NOFOLLOW on read and write, mode 0600: a symlink cannot redirect the
//     state file (same threat model as the kill switch);
//   * read is fail-closed by policy at the call site, not here: this layer
//     reports Corrupt and lets main decide (LIVE refuses to start).
//
// Everything here is cold path. No allocation, no locale, no std::string.
// ─────────────────────────────────────────────────────────────────────────────

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace crowdintel {

inline constexpr uint32_t DAILY_STATE_MAGIC = 0x54534443u;  // "CDST" little-endian
inline constexpr uint16_t DAILY_STATE_VERSION = 1;

struct DailyStateRecord {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint64_t utc_day;              // UTC day number the volume belongs to
    double daily_buy_volume_usd;   // gross BUY notional accumulated that day
    uint8_t kill_latched;          // operator or risk kill is active
    uint8_t day_kill_latched;      // kill was triggered by the daily loss cap
    uint8_t reserved1[6];
    uint64_t seq;                  // monotonic write counter (diagnostics)
    uint32_t crc;                  // CRC32 over every preceding byte
    uint32_t reserved2;
};

enum class DailyStateLoad : uint8_t {
    Loaded = 0,   // a valid record was read
    Missing = 1,  // no file yet (first run)
    Corrupt = 2,  // wrong size, bad magic/version, or CRC mismatch
    IoError = 3   // open/read failed for a reason other than ENOENT
};

// Bitwise CRC32 (IEEE 802.3). Table-free on purpose: this runs once per
// restart and once per persistence interval, never per tick.
inline uint32_t daily_state_crc32(const void* data, size_t len) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

inline uint32_t daily_state_record_crc(const DailyStateRecord& rec) noexcept {
    return daily_state_crc32(&rec, offsetof(DailyStateRecord, crc));
}

// Splits "a/b/c.bin" into "a/b". Falls back to "." when there is no separator.
inline void daily_state_parent_dir(const char* path, char* out, size_t cap) noexcept {
    if (!path || !out || cap == 0) return;
    const char* last = nullptr;
    for (const char* p = path; *p; ++p)
        if (*p == '/') last = p;
    if (!last) {
        std::snprintf(out, cap, ".");
        return;
    }
    size_t len = static_cast<size_t>(last - path);
    if (len == 0) len = 1;  // "/file" -> "/"
    if (len >= cap) len = cap - 1;
    std::memcpy(out, path, len);
    out[len] = '\0';
}

// Atomic, durable write. Returns false on any failure, leaving `path` either
// at its previous content or fully replaced — never partially written.
inline bool save_daily_state(const char* path,
                             DailyStateRecord& rec) noexcept {
    if (!path || !*path) return false;

    rec.magic = DAILY_STATE_MAGIC;
    rec.version = DAILY_STATE_VERSION;
    rec.reserved0 = 0;
    rec.reserved1[0] = rec.reserved1[1] = rec.reserved1[2] = 0;
    rec.reserved1[3] = rec.reserved1[4] = rec.reserved1[5] = 0;
    rec.reserved2 = 0;
    ++rec.seq;
    rec.crc = daily_state_record_crc(rec);

    char tmp[256];
    const int n = std::snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path,
                                static_cast<long>(::getpid()));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    const int fd = ::open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;

    bool ok = ::write(fd, &rec, sizeof(rec)) ==
              static_cast<ssize_t>(sizeof(rec));
    if (ok) ok = ::fsync(fd) == 0;
    ::close(fd);

    if (!ok) {
        ::unlink(tmp);
        return false;
    }
    if (::rename(tmp, path) != 0) {
        ::unlink(tmp);
        return false;
    }

    // Durability of the rename itself. Without this a power loss can undo the
    // link update even though the data blocks were flushed.
    char dir[256];
    daily_state_parent_dir(path, dir, sizeof(dir));
    const int dfd = ::open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dfd >= 0) {
        (void)::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

inline DailyStateLoad load_daily_state(const char* path,
                                       DailyStateRecord& out) noexcept {
    if (!path || !*path) return DailyStateLoad::IoError;

    const int fd = ::open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return (errno == ENOENT) ? DailyStateLoad::Missing
                                 : DailyStateLoad::IoError;

    DailyStateRecord rec{};
    const ssize_t got = ::read(fd, &rec, sizeof(rec));
    const int read_errno = errno;
    ::close(fd);

    // A read() failure (EISDIR, EIO, ...) is an environment problem, not a
    // damaged record; report it distinctly so the operator sees the real cause.
    if (got < 0) return DailyStateLoad::IoError;
    if (got != static_cast<ssize_t>(sizeof(rec))) return DailyStateLoad::Corrupt;
    if (rec.magic != DAILY_STATE_MAGIC) return DailyStateLoad::Corrupt;
    if (rec.version != DAILY_STATE_VERSION) return DailyStateLoad::Corrupt;
    if (rec.crc != daily_state_record_crc(rec)) return DailyStateLoad::Corrupt;

    (void)read_errno;
    out = rec;
    return DailyStateLoad::Loaded;
}

}  // namespace crowdintel

#endif  // CROWDINTEL_DAILY_STATE_HPP
