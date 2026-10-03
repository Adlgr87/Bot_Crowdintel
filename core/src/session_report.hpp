// ═══════════════════════════════════════════════════════════════════════════
// Shutdown post-mortem: the two answers an operator needs from the last lines
// of a session log.
//
//   * why the session ended up BLOCKED — the most recent reconciliation run, its
//     readiness and its reasons (the ledger keeps a ring of runs);
//   * what the heartbeat contract was doing — whether the venue ever
//     acknowledged a beat, whether it invalidated our id, and how long ago the
//     last acknowledged beat was.
//
// Both formatters are pure: bounded output, no I/O, no allocation, no locks.  The
// caller reads the ledger through its locked copy accessors and hands the copies
// over, which keeps this testable without a running bot.
//
// The heartbeat id itself is never formatted: it is the token that chains our
// beats to the venue, and logs leave the machine.  Only its presence is reported.
// ═══════════════════════════════════════════════════════════════════════════
#ifndef CROWDINTEL_SESSION_REPORT_HPP
#define CROWDINTEL_SESSION_REPORT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../include/event_ledger.hpp"

namespace report {

// Formats one reconciliation run.  `now_wall_ns` is the instant of the report, so
// the age is "how long ago the run finished".  A run that never finished, or a
// wall clock that moved backwards, reports age 0 rather than a wrapped huge
// number.  Returns the number of characters written (excluding the terminator),
// or 0 when the buffer is too small - the caller must not print a half line.
inline size_t format_last_reconciliation(const ledger::ReconciliationRun& run,
                                         uint64_t now_wall_ns, char* out,
                                         size_t cap) noexcept {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';

    char reasons[256]{};
    size_t reasons_len = 0;
    for (size_t i = 0; i < run.reason_count &&
                        i < sizeof(run.reasons) / sizeof(run.reasons[0]);
         ++i) {
        const int n = std::snprintf(reasons + reasons_len,
                                    sizeof(reasons) - reasons_len, "%s%s",
                                    reasons_len ? ";" : "", run.reasons[i]);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(reasons) - reasons_len) {
            // Out of room: mark the list as truncated instead of dropping the
            // last reason silently.
            const int t = std::snprintf(reasons + reasons_len,
                                        sizeof(reasons) - reasons_len, "%s",
                                        reasons_len ? ";..." : "...");
            if (t > 0 && static_cast<size_t>(t) < sizeof(reasons) - reasons_len)
                reasons_len += static_cast<size_t>(t);
            break;
        }
        reasons_len += static_cast<size_t>(n);
    }

    const uint64_t age_s =
        run.finished_wall_ns != 0 && now_wall_ns > run.finished_wall_ns
            ? (now_wall_ns - run.finished_wall_ns) / 1000000000ULL
            : 0;
    const int written = std::snprintf(
        out, cap, "run=%llu ready=%s reason_count=%u age_s=%llu%s%s%s",
        static_cast<unsigned long long>(run.run_id), run.ready ? "READY" : "BLOCKED",
        static_cast<unsigned>(run.reason_count),
        static_cast<unsigned long long>(age_s), reasons_len ? " reasons=[" : "",
        reasons_len ? reasons : "", reasons_len ? "]" : "");
    if (written <= 0 || static_cast<size_t>(written) >= cap) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

// Formats the persisted heartbeat contract state.  Same age rule as above.
inline size_t format_heartbeat_contract(const ledger::HeartbeatState& state,
                                        uint64_t now_wall_ns, char* out,
                                        size_t cap) noexcept {
    if (out == nullptr || cap == 0) return 0;
    out[0] = '\0';
    const uint64_t accepted_age_s =
        state.last_accepted_wall_ns != 0 && now_wall_ns > state.last_accepted_wall_ns
            ? (now_wall_ns - state.last_accepted_wall_ns) / 1000000000ULL
            : 0;
    const int written = std::snprintf(
        out, cap,
        "chain_active=%d invalidated=%d consecutive_failures=%llu id_present=%d "
        "last_accepted_age_s=%llu",
        state.chain_active ? 1 : 0, state.invalidated ? 1 : 0,
        static_cast<unsigned long long>(state.consecutive_failures),
        state.heartbeat_id[0] ? 1 : 0,
        static_cast<unsigned long long>(accepted_age_s));
    if (written <= 0 || static_cast<size_t>(written) >= cap) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

}  // namespace report

#endif  // CROWDINTEL_SESSION_REPORT_HPP
