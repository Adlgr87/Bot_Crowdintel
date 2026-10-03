// Phase 2 unit tests: order state machine + crash-safe order ledger.
//
// The ledger is exercised against real files in a temporary directory,
// including torn tails (interrupted write), mid-file corruption, compaction,
// and a forked process that dies without flushing.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../alpha/crowdintel/alpha_parser.hpp"
#include "../../core/src/execution_engine.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/order_ledger.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__);      \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

std::string temp_dir() {
    char templ[] = "/tmp/crowdintel-ledger-XXXXXX";
    const char* dir = ::mkdtemp(templ);
    return dir ? std::string(dir) : std::string();
}

long file_mode(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return -1;
    return static_cast<long>(info.st_mode & 07777);
}

uint64_t file_size(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return 0;
    return static_cast<uint64_t>(info.st_size);
}

bool append_bytes(const std::string& path, const void* data, size_t length) {
    FILE* file = std::fopen(path.c_str(), "ab");
    if (!file) return false;
    const bool ok = std::fwrite(data, 1, length, file) == length;
    std::fclose(file);
    return ok;
}

bool flip_byte(const std::string& path, long offset) {
    FILE* file = std::fopen(path.c_str(), "r+b");
    if (!file) return false;
    if (std::fseek(file, offset, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    int byte = std::fgetc(file);
    if (byte == EOF) {
        std::fclose(file);
        return false;
    }
    const int flipped = byte ^ 0x40;
    std::fseek(file, offset, SEEK_SET);
    const bool ok = std::fputc(flipped, file) != EOF;
    std::fclose(file);
    return ok;
}

// ── state machine ────────────────────────────────────────────────────────────

void test_state_machine_table() {
    std::printf("state_machine_table\n");
    using namespace cledger;

    // Local intent reaches PENDING_SUBMIT but nothing further.
    CHECK(transition(OrderState::kUnknown, LedgerEvent::kIntentRecorded,
                     Evidence::kLocalIntent).next == OrderState::kPendingSubmit,
          "intent -> pending submit");
    CHECK(transition(OrderState::kPendingSubmit, LedgerEvent::kSubmitAck,
                     Evidence::kLocalIntent)
              .result == TransitionResult::kNeedsReconcile,
          "ack without venue evidence stays unproven");
    CHECK(transition(OrderState::kPendingSubmit, LedgerEvent::kSubmitAck,
                     Evidence::kVenueAck).next == OrderState::kSubmitted,
          "ack with venue evidence -> submitted");

    // Ambiguity always lands in UNKNOWN, from any non-terminal state.
    CHECK(transition(OrderState::kPendingSubmit, LedgerEvent::kSubmitAmbiguous,
                     Evidence::kNone).next == OrderState::kUnknown,
          "ambiguous submit -> UNKNOWN");
    CHECK(transition(OrderState::kUnknown, LedgerEvent::kSubmitAmbiguous,
                     Evidence::kNone).result == TransitionResult::kUnchanged,
          "ambiguous submit is idempotent in UNKNOWN");

    // Channel evidence can pull an order out of UNKNOWN (the case that makes
    // the journal necessary: our POST was lost but the venue took the order).
    const Transition rescued =
        transition(OrderState::kUnknown, LedgerEvent::kOrderLive,
                   Evidence::kVenueChannel);
    CHECK(rescued.result == TransitionResult::kApplied &&
              rescued.next == OrderState::kLive,
          "channel evidence rescues an UNKNOWN order");
    CHECK(transition(OrderState::kUnknown, LedgerEvent::kOrderLive,
                     Evidence::kLocalIntent)
              .result == TransitionResult::kNeedsReconcile,
          "no channel evidence keeps UNKNOWN");

    // Fill accounting.
    const Transition partial = transition(OrderState::kLive,
                                          LedgerEvent::kTradeUpdate,
                                          Evidence::kVenueChannel, 4000000,
                                          10000000);
    CHECK(partial.next == OrderState::kPartiallyFilled, "partial fill");
    const Transition full = transition(OrderState::kPartiallyFilled,
                                       LedgerEvent::kTradeUpdate,
                                       Evidence::kVenueChannel, 10000000,
                                       10000000);
    CHECK(full.next == OrderState::kFilled, "full fill");
    CHECK(transition(OrderState::kLive, LedgerEvent::kTradeUpdate,
                     Evidence::kVenueChannel, 0, 0)
              .result == TransitionResult::kNeedsReconcile,
          "fill without size needs reconciliation");
    CHECK(transition(OrderState::kLive, LedgerEvent::kTradeFailed,
                     Evidence::kVenueChannel).next == OrderState::kUnknown,
          "failed trade returns to UNKNOWN");

    // Terminal states are immutable; a contradiction must be reported.
    CHECK(transition(OrderState::kFilled, LedgerEvent::kTradeUpdate,
                     Evidence::kVenueChannel, 10000000, 10000000)
              .result == TransitionResult::kUnchanged,
          "replayed fill is idempotent");
    CHECK(transition(OrderState::kCanceled, LedgerEvent::kTradeUpdate,
                     Evidence::kVenueChannel, 10000000, 10000000)
              .result == TransitionResult::kIllegal,
          "canceled then filled is illegal");
    CHECK(transition(OrderState::kRejected, LedgerEvent::kSubmitRejected,
                     Evidence::kVenueAck)
              .result == TransitionResult::kUnchanged,
          "replayed rejection is idempotent");
    CHECK(transition(OrderState::kRejected, LedgerEvent::kSubmitAck,
                     Evidence::kVenueAck)
              .result == TransitionResult::kIllegal,
          "rejection contradicted by an ack is illegal");

    // Reconciliation evidence can also close a lost order.
    CHECK(transition(OrderState::kUnknown, LedgerEvent::kReconcilePresent,
                     Evidence::kVenueRest, 10000000, 10000000)
              .next == OrderState::kFilled,
          "REST reconciliation fills an UNKNOWN order");
    CHECK(transition(OrderState::kUnknown, LedgerEvent::kReconcileAbsent,
                     Evidence::kVenueRest)
              .next == OrderState::kRejected,
          "confirmed absence rejects");
    CHECK(transition(OrderState::kUnknown, LedgerEvent::kReconcileAbsent,
                     Evidence::kVenueChannel)
              .result == TransitionResult::kNeedsReconcile,
          "absence without REST evidence is not proof");
    CHECK(is_terminal(OrderState::kFilled) && is_terminal(OrderState::kRejected) &&
              !is_terminal(OrderState::kUnknown) &&
              !is_terminal(OrderState::kSubmitted),
          "terminal classification");
}

// ── ledger persistence ───────────────────────────────────────────────────────

cledger::IntentRecord intent(const char* id, uint64_t signal_id,
                             uint64_t shares = 10000000) {
    cledger::IntentRecord record{};
    std::snprintf(record.client_order_id, sizeof(record.client_order_id), "%s",
                  id);
    record.signal_id = signal_id;
    record.market_hash = 0xABCDEF;
    record.price_fixed6 = 470000;
    record.shares_fixed6 = shares;
    record.notional_fixed6 = 4700000;
    record.side = K_SIDE_BUY;
    record.order_type = 1;
    return record;
}

void test_ledger_roundtrip_and_views() {
    std::printf("ledger_roundtrip\n");
    const std::string dir = temp_dir();
    CHECK(!dir.empty(), "temp dir");
    const std::string path = dir + "/orders.journal";

    char error[192];
    {
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        options.fsync_records = true;
        CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
              "ledger opens");
        CHECK(ledger.gate_open(), "empty ledger has an open gate");
        CHECK(ledger.record_intent(intent("aa-1", 1), error, sizeof(error)),
              "intent recorded");
        CHECK(!ledger.record_intent(intent("aa-1", 1), error, sizeof(error)),
              "duplicate client order id rejected");
        CHECK(!ledger.gate_open(), "pending submit blocks trading");

        CHECK(ledger.record_transition("aa-1", cledger::LedgerEvent::kSubmitAck,
                                       cledger::Evidence::kVenueAck,
                                       "venue-1", 0, error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "ack applied");
        CHECK(ledger.record_intent(intent("aa-2", 2), error, sizeof(error)),
              "second intent");
        CHECK(ledger.record_transition("aa-2", cledger::LedgerEvent::kSubmitAck,
                                       cledger::Evidence::kVenueAck, "venue-2",
                                       0, error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "second ack");
        CHECK(ledger.record_transition("aa-2",
                                       cledger::LedgerEvent::kSubmitAmbiguous,
                                       cledger::Evidence::kNone, "", 0, error,
                                       sizeof(error))
                  .next == cledger::OrderState::kUnknown,
              "second order goes UNKNOWN");
        CHECK(ledger.unknown_orders() == 1 && ledger.unreconciled_orders() == 2,
              "views count UNKNOWN and unreconciled");
        CHECK(std::strcmp(ledger.find("aa-1")->venue_order_id, "venue-1") == 0,
              "venue id journaled");
        CHECK(ledger.record_transition("nope",
                                       cledger::LedgerEvent::kSubmitAck,
                                       cledger::Evidence::kVenueAck, "", 0,
                                       error, sizeof(error))
                  .result == cledger::TransitionResult::kIllegal,
              "unknown client id rejected");
        CHECK(file_mode(path) == 0640, "journal mode is 0640");
        CHECK(ledger.bytes_on_disk() == file_size(path),
              "byte accounting matches disk");
    }

    // Reopen: recovery must reconstruct both orders and the gate.
    {
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
              "ledger reopens");
        CHECK(ledger.order_count() == 2, "two orders recovered");
        CHECK(ledger.unknown_orders() == 1, "UNKNOWN recovered");
        CHECK(ledger.unreconciled_orders() == 2, "gate stays closed after crash");
        const cledger::OrderSummary* first = ledger.find("aa-1");
        CHECK(first && first->state == cledger::OrderState::kSubmitted &&
                  first->signal_id == 1,
              "first order state recovered");

        // Reconciliation closes both orders.
        CHECK(ledger.record_transition("aa-1",
                                       cledger::LedgerEvent::kOrderCanceled,
                                       cledger::Evidence::kVenueChannel, "",
                                       0, error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "cancel applied");
        CHECK(ledger.record_transition("aa-2",
                                       cledger::LedgerEvent::kReconcilePresent,
                                       cledger::Evidence::kVenueRest, "venue-2",
                                       0, error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "REST reconciliation applied");
        CHECK(!ledger.gate_open(),
              "an order REST confirms as resting keeps the gate closed");
        CHECK(ledger.record_transition("aa-2",
                                       cledger::LedgerEvent::kOrderCanceled,
                                       cledger::Evidence::kVenueRest, "venue-2",
                                       0, error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "REST cancellation applied");
        CHECK(ledger.gate_open(), "gate opens once every order is terminal");
        CHECK(ledger.record_transition("aa-1",
                                       cledger::LedgerEvent::kOrderLive,
                                       cledger::Evidence::kVenueChannel, "", 0,
                                       error, sizeof(error))
                  .result == cledger::TransitionResult::kIllegal,
              "illegal transition refuses to write");
    }
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

void test_torn_tail_and_corruption() {
    std::printf("ledger_torn_tail_and_corruption\n");
    const std::string dir = temp_dir();
    CHECK(!dir.empty(), "temp dir");
    char error[192];

    // (a) Sub-frame garbage after the last good frame: truncate + report.
    {
        const std::string path = dir + "/torn.journal";
        {
            cledger::OrderLedger ledger;
            cledger::OpenOptions options;
            options.fsync_records = false;
            CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
                  "torn fixture opens");
            CHECK(ledger.record_intent(intent("torn-1", 1), error,
                                       sizeof(error)),
                  "torn fixture intent");
        }
        const char garbage[100] = "this is an interrupted write";
        CHECK(append_bytes(path, garbage, sizeof(garbage)), "garbage appended");
        {
            cledger::OrderLedger ledger;
            cledger::OpenOptions options;
            options.fsync_records = false;
            CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
                  "ledger tolerates a torn tail");
            CHECK(ledger.torn_tail_bytes() == sizeof(garbage),
                  "torn tail reported");
            CHECK(ledger.order_count() == 1, "good records survive");
            CHECK(ledger.unreconciled_orders() == 1, "gate stays closed");
        }
        // The rewrite must have removed the garbage from the file.
        CHECK(file_size(path) == 2 * cledger::kLedgerFrame,
              "torn tail physically truncated");
    }

    // (b) A full frame of garbage at the tail is still a torn tail.
    {
        const std::string path = dir + "/torn2.journal";
        {
            cledger::OrderLedger ledger;
            cledger::OpenOptions options;
            options.fsync_records = false;
            CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
                  "torn2 opens");
            CHECK(ledger.record_intent(intent("torn-2", 2), error,
                                       sizeof(error)),
                  "torn2 intent");
        }
        std::vector<char> one_frame(cledger::kLedgerFrame, 0x5A);
        CHECK(append_bytes(path, one_frame.data(), one_frame.size()),
              "one full garbage frame appended");
        {
            cledger::OrderLedger ledger;
            cledger::OpenOptions options;
            options.fsync_records = false;
            CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
                  "a single garbage frame at the tail is torn, not corruption");
            CHECK(ledger.order_count() == 1, "torn2 recovers");
        }
        // Two garbage frames cannot come from one interrupted write: refuse.
        std::vector<char> two_frames(2 * cledger::kLedgerFrame, 0x5A);
        const std::string path3 = dir + "/torn3.journal";
        CHECK(append_bytes(path3, two_frames.data(), two_frames.size()),
              "two garbage frames written");
        {
            cledger::OrderLedger ledger;
            cledger::OpenOptions options;
            options.fsync_records = false;
            CHECK(!ledger.open(path3.c_str(), options, error, sizeof(error)),
                  "two garbage frames are corruption");
        }
    }

    // (c) Corruption in the middle must refuse to open.
    {
        const std::string path = dir + "/corrupt.journal";
        {
            cledger::OrderLedger ledger;
            cledger::OpenOptions options;
            options.fsync_records = false;
            CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
                  "corrupt fixture opens");
            CHECK(ledger.record_intent(intent("c-1", 1), error, sizeof(error)),
                  "c-1");
            CHECK(ledger.record_intent(intent("c-2", 2), error, sizeof(error)),
                  "c-2");
            CHECK(ledger.record_intent(intent("c-3", 3), error, sizeof(error)),
                  "c-3");
        }
        CHECK(flip_byte(path, static_cast<long>(cledger::kLedgerFrame + 20)),
              "middle frame corrupted");
        {
            cledger::OrderLedger ledger;
            cledger::OpenOptions options;
            options.fsync_records = false;
            CHECK(!ledger.open(path.c_str(), options, error, sizeof(error)),
                  "mid-file corruption refuses to open");
            CHECK(std::strstr(error, "corrupt") != nullptr,
                  "corruption reported as such");
            CHECK(!ledger.gate_open(), "failed ledger keeps the gate closed");
        }
    }

    // (d) Foreign file is never treated as an empty ledger.
    {
        const std::string path = dir + "/foreign.journal";
        const char junk[256] = "not a ledger at all";
        CHECK(append_bytes(path, junk, sizeof(junk)), "junk written");
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        CHECK(!ledger.open(path.c_str(), options, error, sizeof(error)),
              "foreign file rejected");
    }
}

void test_compaction() {
    std::printf("ledger_compaction\n");
    const std::string dir = temp_dir();
    CHECK(!dir.empty(), "temp dir");
    const std::string path = dir + "/compact.journal";
    char error[192];

    cledger::OrderLedger ledger;
    cledger::OpenOptions options;
    options.fsync_records = false;
    options.max_bytes = 4096;  // forces compaction after ~22 frames
    CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
          "compact fixture opens");

    for (uint64_t i = 0; i < 40; ++i) {
        char id[cledger::kClientOrderIdChars];
        std::snprintf(id, sizeof(id), "cmp-%02llu",
                      static_cast<unsigned long long>(i));
        CHECK(ledger.record_intent(intent(id, i), error, sizeof(error)),
              "compact intent");
        CHECK(ledger.record_transition(id, cledger::LedgerEvent::kSubmitAck,
                                       cledger::Evidence::kVenueAck, "venue",
                                       0, error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "compact ack");
        // Keep two orders live; everything else reaches a terminal state.
        if (i < 2) continue;
        CHECK(ledger.record_transition(id, cledger::LedgerEvent::kOrderCanceled,
                                       cledger::Evidence::kVenueChannel, "", 0,
                                       error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "compact cancel");
    }
    CHECK(file_size(path) < 4096 + 2 * cledger::kLedgerFrame,
          "compaction keeps the file under the cap");
    CHECK(ledger.unreconciled_orders() == 2, "live orders retained in memory");
    const uint64_t size_before = file_size(path);
    ledger.close();

    cledger::OrderLedger reopened;
    CHECK(reopened.open(path.c_str(), options, error, sizeof(error)),
          "compacted ledger reopens");
    CHECK(reopened.unreconciled_orders() == 2,
          "compaction preserved the live orders");
    CHECK(reopened.find("cmp-00") != nullptr && reopened.find("cmp-01") != nullptr,
          "live orders identifiable after compaction");
    CHECK(size_before > 0, "compaction wrote bytes");
    CHECK(std::strstr(reopened.path(), "compact.journal") != nullptr,
          "path exposed");
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

void test_fork_crash_recovery() {
    std::printf("ledger_fork_crash\n");
    const std::string dir = temp_dir();
    CHECK(!dir.empty(), "temp dir");
    const std::string path = dir + "/crash.journal";

    const pid_t child = ::fork();
    CHECK(child >= 0, "fork");
    if (child == 0) {
        // Child: journal an intent, then die without closing (SIGKILL).
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        options.fsync_records = true;
        char error[192];
        if (!ledger.open(path.c_str(), options, error, sizeof(error)))
            _exit(2);
        if (!ledger.record_intent(intent("crash-1", 99), error, sizeof(error)))
            _exit(3);
        ::raise(SIGKILL);
        _exit(4);
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child, "waitpid");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "child died by SIGKILL");

    cledger::OrderLedger ledger;
    cledger::OpenOptions options;
    options.fsync_records = true;
    char error[192];
    CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
          "ledger recovers after SIGKILL");
    const cledger::OrderSummary* recovered = ledger.find("crash-1");
    CHECK(recovered && recovered->signal_id == 99,
          "fsynced intent survived SIGKILL");
    CHECK(ledger.unreconciled_orders() == 1,
          "unreconciled order still blocks trading");
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

// ── engine integration ───────────────────────────────────────────────────────

enum class ScriptMode { kAck, kTimeout, kRejected };

struct ScriptedClient {
    ScriptMode mode = ScriptMode::kAck;
    int calls = 0;

    SubmitResult submit(const WireBody&) {
        ++calls;
        SubmitResult result{};
        if (mode == ScriptMode::kAck) {
            result.ok = true;
            result.final = true;
            result.http_code = 200;
            std::snprintf(result.status, sizeof(result.status), "live");
            std::snprintf(result.order_id, sizeof(result.order_id), "venue-%d",
                          calls);
        } else if (mode == ScriptMode::kTimeout) {
            result.http_code = 0;  // transport failure: no venue evidence
            std::snprintf(result.error, sizeof(result.error), "timeout");
        } else {
            result.http_code = 400;
            std::snprintf(result.error, sizeof(result.error), "invalid order");
        }
        return result;
    }
};

struct EngineFixture {
    MarketConfig cfg;
    EIP712Signer signer;
    OrderBookL2 book;
    SPSC_RingBuffer<AlphaSignal> signals;
    std::unique_ptr<PresignedOrderPool> pool;
    uint64_t next_signal_id = 1;

    bool init() {
        const char* key_text =
            "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
        uint8_t key[32];
        if (!parse_hex_bytes(key_text, 64, key, sizeof(key))) return false;
        const bool signer_ok = signer.init(key, false);
        secure_zero(key, sizeof(key));
        if (!signer_ok) return false;
        cfg.mode = MarketConfig::Mode::kReplay;
        const char* token =
            "71321045679252212594626395510336467040167069592778062791519851593659551227755";
        std::snprintf(cfg.token_id_dec, sizeof(cfg.token_id_dec), "%s", token);
        if (!parse_uint256_dec(token, std::strlen(token), cfg.token_id_be))
            return false;
        if (cfg.finalize_identity(signer.signer_address()) != nullptr)
            return false;
        std::snprintf(cfg.owner_api_key, sizeof(cfg.owner_api_key), "test-owner");
        std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "ledger-market");
        cfg.market_hash =
            alpha_hash_bytes(cfg.market_slug, std::strlen(cfg.market_slug));
        cfg.max_order_usd = 10.0;
        cfg.max_exposure_usd = 100.0;
        cfg.max_daily_loss_usd = 100.0;
        book.set_tick_size(10000);
        const Level2Entry bids[] = {{470000, 50000000}};
        const Level2Entry asks[] = {{530000, 50000000}};
        book.set_book(bids, 1, asks, 1);
        pool = std::make_unique<PresignedOrderPool>(cfg, signer,
                                                    cfg.presign_ttl_ms);
        return pool->rebuild(470000, 530000, 20000000, 10000);
    }

    AlphaSignal signal() {
        AlphaSignal signal{};
        signal.direction_hint = K_SIDE_BUY;
        signal.p_win = 0.75;
        signal.confidence = 0.95;
        signal.q_value = 0.01;
        signal.timestamp_ns = AlphaParser::realtime_ns();
        signal.market_hash = cfg.market_hash;
        signal.signal_id = next_signal_id++;
        return signal;
    }

    bool push(const AlphaSignal& signal) { return signals.try_push(signal); }
};

void test_engine_journals_outcomes() {
    std::printf("engine_journals_outcomes\n");
    const std::string dir = temp_dir();
    CHECK(!dir.empty(), "temp dir");
    char error[192];

    // (a) Venue acknowledgement -> SUBMITTED, pending confirmation.
    {
        EngineFixture fixture;
        CHECK(fixture.init(), "fixture");
        const std::string path = dir + "/ack.journal";
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        options.fsync_records = true;
        CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
              "ledger opens");

        ScriptedClient client;
        client.mode = ScriptMode::kAck;
        ExecutionEngine<ScriptedClient> engine(fixture.cfg, fixture.book,
                                               fixture.signals, fixture.signer,
                                               *fixture.pool, client);
        engine.attach_ledger(&ledger);

        const AlphaSignal first = fixture.signal();
        CHECK(fixture.push(first) &&
                  engine.run_tick() == TickResult::SUBMITTED,
              "acknowledged submit reports SUBMITTED");
        char client_id[cledger::kClientOrderIdChars];
        cledger::make_client_order_id(fixture.cfg.market_hash, first.signal_id,
                                      client_id);
        const cledger::OrderSummary* summary = ledger.find(client_id);
        CHECK(summary && summary->state == cledger::OrderState::kSubmitted,
              "ledger holds SUBMITTED");
        CHECK(std::strcmp(summary->venue_order_id, "venue-1") == 0,
              "venue order id recorded");
        CHECK(summary->shares_fixed6 > 0 && summary->notional_fixed6 > 0,
              "intent amounts recorded");

        // A journaled order without venue confirmation blocks the next tick.
        CHECK(!ledger.gate_open(), "gate closed while unconfirmed");
        CHECK(fixture.push(fixture.signal()) &&
                  engine.run_tick() == TickResult::RECONCILE_REQUIRED,
              "engine refuses to trade with unconfirmed orders");
        CHECK(client.calls == 1, "no second order was sent");
        CHECK(engine.ledger_blocks() == 1, "block counted");
        ::unlink(path.c_str());
    }

    // (b) Transport timeout -> UNKNOWN (never "failed").
    {
        EngineFixture fixture;
        CHECK(fixture.init(), "fixture");
        const std::string path = dir + "/timeout.journal";
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
              "ledger opens");
        ScriptedClient client;
        client.mode = ScriptMode::kTimeout;
        ExecutionEngine<ScriptedClient> engine(fixture.cfg, fixture.book,
                                               fixture.signals, fixture.signer,
                                               *fixture.pool, client);
        engine.attach_ledger(&ledger);

        const AlphaSignal first = fixture.signal();
        CHECK(fixture.push(first) &&
                  engine.run_tick() == TickResult::SUBMIT_FAILED,
              "timeout reports SUBMIT_FAILED to the caller");
        CHECK(ledger.unknown_orders() == 1, "timeout produces UNKNOWN");
        CHECK(!ledger.gate_open(), "UNKNOWN blocks trading");
        CHECK(engine.submitted() == 0, "nothing counted as submitted");
        ::unlink(path.c_str());
    }

    // (c) Definitive venue rejection -> terminal, gate stays open.
    {
        EngineFixture fixture;
        CHECK(fixture.init(), "fixture");
        const std::string path = dir + "/rejected.journal";
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
              "ledger opens");
        ScriptedClient client;
        client.mode = ScriptMode::kRejected;
        ExecutionEngine<ScriptedClient> engine(fixture.cfg, fixture.book,
                                               fixture.signals, fixture.signer,
                                               *fixture.pool, client);
        engine.attach_ledger(&ledger);

        CHECK(fixture.push(fixture.signal()) &&
                  engine.run_tick() == TickResult::SUBMIT_FAILED,
              "rejection reports SUBMIT_FAILED");
        CHECK(ledger.unknown_orders() == 0 && ledger.gate_open(),
              "venue rejection is terminal, not UNKNOWN");
        ::unlink(path.c_str());
    }

    // (d) A durable intent is a precondition for egress: an id collision
    // (reused signal id) must stop the order before it reaches the venue.
    {
        EngineFixture fixture;
        CHECK(fixture.init(), "fixture");
        const std::string path = dir + "/collision.journal";
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
              "ledger opens");
        const AlphaSignal reused = fixture.signal();
        char collision_id[cledger::kClientOrderIdChars];
        cledger::make_client_order_id(fixture.cfg.market_hash, reused.signal_id,
                                      collision_id);
        // A terminal order with the same client id leaves the gate open but
        // makes the next intent for that id impossible to journal.
        CHECK(ledger.record_intent(intent(collision_id, reused.signal_id), error,
                                   sizeof(error)),
              "collision intent");
        CHECK(ledger.record_transition(collision_id,
                                       cledger::LedgerEvent::kSubmitRejected,
                                       cledger::Evidence::kVenueAck, "", 0,
                                       error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "collision order terminal");
        CHECK(ledger.gate_open(), "gate open before the collision");
        fixture.next_signal_id = reused.signal_id + 1;  // push() uses signal_id
        ScriptedClient client;
        client.mode = ScriptMode::kAck;
        ExecutionEngine<ScriptedClient> engine(fixture.cfg, fixture.book,
                                               fixture.signals, fixture.signer,
                                               *fixture.pool, client);
        engine.attach_ledger(&ledger);
        AlphaSignal duplicate = reused;
        duplicate.timestamp_ns = AlphaParser::realtime_ns();
        CHECK(fixture.push(duplicate) &&
                  engine.run_tick() == TickResult::RECONCILE_REQUIRED,
              "unwritable intent fails closed");
        CHECK(client.calls == 0, "no egress without a durable intent");
        ::unlink(path.c_str());
    }

    ::rmdir(dir.c_str());
}

void test_ledger_has_no_secrets() {
    std::printf("ledger_no_secrets\n");
    const std::string dir = temp_dir();
    CHECK(!dir.empty(), "temp dir");
    const std::string path = dir + "/secrets.journal";
    char error[192];
    {
        cledger::OrderLedger ledger;
        cledger::OpenOptions options;
        options.fsync_records = true;
        CHECK(ledger.open(path.c_str(), options, error, sizeof(error)),
              "ledger opens");
        CHECK(ledger.record_intent(intent("sec-1", 1), error, sizeof(error)),
              "intent recorded");
        CHECK(ledger.record_transition("sec-1", cledger::LedgerEvent::kSubmitAck,
                                       cledger::Evidence::kVenueAck, "venue-1",
                                       0, error, sizeof(error))
                  .result == cledger::TransitionResult::kApplied,
              "ack recorded");
    }
    FILE* file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr, "journal readable");
    if (file) {
        std::vector<char> bytes(static_cast<size_t>(file_size(path)));
        const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
        CHECK(read == bytes.size(), "journal read");
        // The fixture key from other tests must never appear in the journal.
        const char* key =
            "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
        const std::string haystack(bytes.begin(), bytes.end());
        CHECK(haystack.find(key) == std::string::npos,
              "no key material in the journal");
    }
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
}

}  // namespace

int main() {
    std::printf("== CROWDINTEL ledger tests ==\n");
    test_state_machine_table();
    test_ledger_roundtrip_and_views();
    test_torn_tail_and_corruption();
    test_compaction();
    test_fork_crash_recovery();
    test_engine_journals_outcomes();
    test_ledger_has_no_secrets();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
