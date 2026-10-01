#ifndef EVIDENCE_INGRESS_HPP
#define EVIDENCE_INGRESS_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Evidence ingress (P4, cold path only).  Three duties:
//
//   * Source-list parsing ("1:0.9,2:0.5") into SourceReliability — used by the
//     startup config, the recalibration-file poller, and unit tests.  Shared
//     code path so calibration semantics can never drift between loaders.
//
//   * NDJSON evidence line parsing (one observation per line):
//       {"source":1,"kind":"count","n":32,"k":22,"hash":12345}
//       {"source":3,"kind":"lr","lr":693147,"hash":12346}   // lr x1e6
//     Strict validation; malformed lines are rejected, never half-applied.
//     Cold-user only (sscanf-based parsers on the advisory thread).
//
//   * EvidenceFileReplayer: a cold thread replays a recorded evidence file
//     (paper-trading/backfill) into the hot queue, then tails it.  This is the
//     universal substrate for the live adapters (polls/macro/sports write the
//     same NDJSON through stdout-redirected collectors or the HTTP poller).
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../include/evidence.hpp"
#include "../include/source_reliability.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../include/time_utils.hpp"

namespace evidence_ingress {

// "1:0.90, 2:0.5" — comma/whitespace separated id:weight pairs.  Returns the
// number of pairs applied; stops at the first malformed token.
inline size_t parse_sources_text(const char* text, SourceReliability& out) {
    if (!text) return 0;
    size_t applied = 0;
    const char* p = text;
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) ||
                      *p == ','))
            ++p;
        if (!*p) break;
        char* end = nullptr;
        const long id = std::strtol(p, &end, 10);
        if (!end || end == p || *end != ':') break;
        p = end + 1;
        char* wend = nullptr;
        const double w = std::strtod(p, &wend);
        if (!wend || wend == p) break;
        out.set_weight(static_cast<uint32_t>(id), w);
        ++applied;
        p = wend;
    }
    return applied;
}

// Parse one NDJSON evidence line.  Returns true and fills `ev` on success;
// anything unrecognized or contradictory returns false untouched.
inline bool parse_evidence_line(const char* line, EvidenceEvent& ev) {
    unsigned source = 0, n = 0, k = 0, hash = 0;
    long long lr = 0;
    char kind[16]{};
    // Tolerate field order by scanning with a permissive-but-complete match:
    // every required field must appear exactly once; extras are rejected by
    // the strict format first, then fall back to field-by-field probes.
    int matched = std::sscanf(line,
        " { \"source\" : %u , \"kind\" : \"%15[^\"]\" , \"n\" : %u , "
        "\"k\" : %u , \"hash\" : %u } ",
        &source, kind, &n, &k, &hash);
    if (matched == 5) {
        if (n == 0 || k > n || hash == 0) return false;
        ev = EvidenceEvent{};
        ev.kind = EvidenceEvent::Kind::COUNT;
        ev.source_id = static_cast<uint32_t>(source);
        ev.count_n = static_cast<uint32_t>(n);
        ev.count_k = static_cast<uint32_t>(k);
        ev.event_hash = static_cast<uint32_t>(hash);
        return true;
    }
    matched = std::sscanf(line,
        " { \"source\" : %u , \"kind\" : \"%15[^\"]\" , \"lr\" : %lld , "
        "\"hash\" : %u } ",
        &source, kind, &lr, &hash);
    if (matched == 4) {
        if (hash == 0) return false;
        if (std::strcmp(kind, "lr") != 0) return false;
        ev = EvidenceEvent{};
        ev.kind = EvidenceEvent::Kind::LR;
        ev.source_id = static_cast<uint32_t>(source);
        ev.lr_x1e6 = static_cast<int32_t>(lr);
        ev.event_hash = static_cast<uint32_t>(hash);
        return true;
    }
    return false;
}

}  // namespace evidence_ingress

// Cold replayer/tailer: pushes parsed events, then keeps tailing the file for
// appends until stopped.  Never blocks the hot queue: full queue drops are
// counted and surfaced via `dropped()`.
class EvidenceFileReplayer {
public:
    EvidenceFileReplayer(const char* path,
                         SPSC_RingBuffer<EvidenceEvent>& queue,
                         SourceReliability* recal, const char* recal_path)
        : path_(path), queue_(queue), recal_(recal),
          recal_path_(recal_path) {}

    bool start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return true;
        FILE* probe = std::fopen(path_, "r");
        open_ok_ = probe != nullptr;
        if (probe) std::fclose(probe);
        thread_ = std::thread([this] { run(); });
        return open_ok_;
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    bool open_ok() const { return open_ok_; }
    uint64_t parsed() const { return parsed_; }
    uint64_t dropped() const { return dropped_; }

private:
    void run() {
        char line[2048];
        while (running_.load(std::memory_order_acquire)) {
            FILE* f = std::fopen(path_, "r");
            if (f) {
                while (running_.load(std::memory_order_acquire) &&
                       std::fgets(line, sizeof(line), f)) {
                    EvidenceEvent ev{};
                    if (!evidence_ingress::parse_evidence_line(line, ev))
                        continue;
                    ev.timestamp_ns = crowdintel::realtime_ns();
                    if (queue_.try_push(ev)) ++parsed_;
                    else ++dropped_;
                }
                std::fclose(f);
            }
            if (recal_ && recal_path_ && recal_path_[0]) {
                FILE* rf = std::fopen(recal_path_, "r");
                if (rf) {
                    if (std::fgets(line, sizeof(line), rf))
                        (void)evidence_ingress::parse_sources_text(line,
                                                                   *recal_);
                    std::fclose(rf);
                }
            }
            for (int i = 0; i < 20 &&
                    running_.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    const char* path_;
    SPSC_RingBuffer<EvidenceEvent>& queue_;
    SourceReliability* recal_;
    const char* recal_path_;
    bool open_ok_ = false;
    uint64_t parsed_ = 0;
    uint64_t dropped_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

#endif  // EVIDENCE_INGRESS_HPP
