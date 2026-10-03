// ═══════════════════════════════════════════════════════════════════════════
// Operator kill switch (emergency stop).
//
// The kill switch is a file: its presence stops the bot.  It is the one lever an
// operator can pull without credentials, without a shell inside the container
// and without knowing the process id, so it must work in EVERY mode the binary
// can run in (live, paper and the offline mock build) and it must fail closed:
// "I could not tell whether the switch is engaged" is treated as engaged.
//
// Single poll, no state, no allocation, no locks: the caller decides the cadence
// (main polls every 100 ms on a cold CPU) and what to do with the answer.
// ═══════════════════════════════════════════════════════════════════════════
#ifndef CROWDINTEL_KILL_SWITCH_HPP
#define CROWDINTEL_KILL_SWITCH_HPP

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace safety {

enum class KillSwitchState : unsigned char {
    DISENGAGED,  // the path is cleanly absent (ENOENT): trading may continue
    ENGAGED,     // the path exists: stop now
    UNKNOWN      // the check itself failed: stop now (fail closed)
};

// One poll of the kill-switch path.
//
// Only a clean ENOENT means "not engaged".  Every other failure of access(2) -
// EACCES on a parent directory, ENOTDIR because a path component is a regular
// file, ENAMETOOLONG, ELOOP, EOVERFLOW, EIO - leaves the operator's intent
// undetermined, and undetermined is reported as UNKNOWN so the caller blocks.
// An empty or null path is a configuration error, also UNKNOWN.
inline KillSwitchState poll_kill_switch(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') return KillSwitchState::UNKNOWN;
    errno = 0;
    if (::access(path, F_OK) == 0) return KillSwitchState::ENGAGED;
    return errno == ENOENT ? KillSwitchState::DISENGAGED : KillSwitchState::UNKNOWN;
}

// True when egress and the decision loop must stop.
inline bool kill_switch_blocks(KillSwitchState state) noexcept {
    return state != KillSwitchState::DISENGAGED;
}

inline const char* kill_switch_state_name(KillSwitchState state) noexcept {
    switch (state) {
        case KillSwitchState::DISENGAGED: return "DISENGAGED";
        case KillSwitchState::ENGAGED:    return "ENGAGED";
        case KillSwitchState::UNKNOWN:    return "UNKNOWN";
    }
    return "UNKNOWN";
}

// Copies the errno text of the last failed check for logging.  Never contains a
// secret: it is the system's description of a filesystem error.
inline void kill_switch_error_text(char* out, size_t out_len) noexcept {
    if (out == nullptr || out_len == 0) return;
    const int code = errno;
    if (code == 0) {
        std::snprintf(out, out_len, "no error");
        return;
    }
    std::snprintf(out, out_len, "%s (errno=%d)", std::strerror(code), code);
}

}  // namespace safety

#endif  // CROWDINTEL_KILL_SWITCH_HPP
