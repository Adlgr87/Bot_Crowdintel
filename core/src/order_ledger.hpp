#ifndef ORDER_LEDGER_HPP
#define ORDER_LEDGER_HPP

// Phase 2 — crash-safe, append-only order ledger (stdlib + POSIX only).
//
// Why a hand-rolled journal instead of SQLite: adding a new dependency needs an
// explicit decision plus a pinned version (see the repo's dependency policy),
// and this file's requirements are narrow — bounded records, strict CRC,
// torn-tail tolerance, no queries beyond "state of order X".
//
// On-disk format (little-endian):
//   frame := u32 length | u32 crc32(payload) | payload[length]
//   payload[208] := type|state|event|evidence|side|order_type|reserved2 |
//                   seq, timestamp_ns, signal_id, market_hash, price_fixed6,
//                   shares_fixed6, notional_fixed6, filled_fixed6 |
//                   client_order_id[40] | venue_order_id[72] | reserved[24]
//
// Recovery rules (fail closed):
//   * a foreign or version-mismatched file is an error, never "empty";
//   * a torn tail (incomplete final frame) is truncated and reported;
//   * a CRC mismatch before EOF is corruption: the ledger refuses to open so a
//     human decides, instead of silently forgetting orders.
//
// The ledger never receives secrets: it stores venue order ids, prices, sizes
// and state, nothing else.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(__linux__) || defined(__unix__)
#define CLEDGER_POSIX 1
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "order_state_machine.hpp"

namespace cledger {

inline constexpr size_t kClientOrderIdChars = 40;  // 16 + '-' + 16 hex + NUL
// Venue order ids are hashes: 0x + 64 hex = 66 characters, so the field must
// be able to hold one in full. Truncating it would break identity matching.
inline constexpr size_t kVenueOrderIdChars = 72;
inline constexpr size_t kLedgerPayload = 208;
inline constexpr size_t kLedgerFrameHeader = 8;
inline constexpr size_t kLedgerFrame = kLedgerPayload + kLedgerFrameHeader;
inline constexpr size_t kMaxTrackedOrders = 256;
inline constexpr char kLedgerMagic[8] = {'C', 'I', 'L', 'G', '0', '0', '0', '4'};

enum class RecordType : uint8_t {
    kFileHeader = 1,
    kIntent = 2,
    kStateChange = 3,
    kRecovery = 4,  // compaction: re-materializes one live order
    kCounters = 5,  // compaction footer
};

struct OpenOptions {
    bool fsync_records = true;
    uint64_t max_bytes = 64ull * 1024 * 1024;
};

struct IntentRecord {
    char client_order_id[kClientOrderIdChars]{};
    uint64_t signal_id = 0;
    uint64_t market_hash = 0;
    uint64_t price_fixed6 = 0;
    uint64_t shares_fixed6 = 0;   // requested, cumulative base
    uint64_t notional_fixed6 = 0; // worst-case USD micro
    uint8_t side = 0;
    uint8_t order_type = 0;
};

struct OrderSummary {
    char client_order_id[kClientOrderIdChars]{};
    char venue_order_id[kVenueOrderIdChars]{};
    OrderState state = OrderState::kUnknown;
    uint64_t signal_id = 0;
    uint64_t market_hash = 0;
    uint64_t price_fixed6 = 0;
    uint64_t shares_fixed6 = 0;
    uint64_t notional_fixed6 = 0;
    uint64_t filled_fixed6 = 0;
    uint64_t last_seq = 0;
    uint64_t last_timestamp_ns = 0;
};

// crc32 (IEEE 802.3) over the payload; bitwise to keep the table out of .rodata.
inline uint32_t crc32_ieee(const uint8_t* data, size_t length) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline void put_u32(uint8_t* out, uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline void put_u64(uint8_t* out, uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline uint32_t get_u32(const uint8_t* in) noexcept {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(in[i]) << (8 * i);
    return value;
}

inline uint64_t get_u64(const uint8_t* in) noexcept {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(in[i]) << (8 * i);
    return value;
}

inline uint64_t monotonic_ns() noexcept {
#if CLEDGER_POSIX
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(now.tv_nsec);
#else
    return 0;
#endif
}

// Deterministic, bounded client order id: market hash + signal id. It must fit
// kClientOrderIdChars with a NUL; longer inputs are rejected by the callers.
inline void make_client_order_id(uint64_t market_hash, uint64_t signal_id,
                                 char out[kClientOrderIdChars]) noexcept {
    std::snprintf(out, kClientOrderIdChars, "%016llx-%016llx",
                  static_cast<unsigned long long>(market_hash),
                  static_cast<unsigned long long>(signal_id));
}

class OrderLedger {
public:
    OrderLedger() = default;
    ~OrderLedger() { close(); }
    OrderLedger(const OrderLedger&) = delete;
    OrderLedger& operator=(const OrderLedger&) = delete;

    bool open(const char* path, const OpenOptions& options, char* error,
              size_t error_cap) noexcept {
        close();
        if (!path || !*path) return fail(error, error_cap, "empty ledger path");
        options_ = options;
        if (std::strlen(path) >= sizeof(path_))
            return fail(error, error_cap, "ledger path too long");
        std::memcpy(path_, path, std::strlen(path) + 1);
        seq_ = 0;
        records_ = 0;
        bytes_ = 0;
        torn_tail_bytes_ = 0;
        slot_count_ = 0;
        counters_ = Counters{};

#if !CLEDGER_POSIX
        (void)options_;
        return fail(error, error_cap, "ledger requires a POSIX host");
#else
        struct stat info {};
        if (::lstat(path_, &info) == 0) {
            if (S_ISDIR(info.st_mode))
                return fail(error, error_cap, "ledger path is a directory");
            if (!recover(error, error_cap)) return false;
        } else if (errno != ENOENT) {
            return fail(error, error_cap, "ledger stat failed");
        }

        fd_ = ::open(path_, O_RDWR | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC,
                     0640);
        if (fd_ < 0) {
            // O_NOFOLLOW: a symlinked ledger would hide concurrent writers.
            return fail(error, error_cap, errno == ELOOP
                                             ? "ledger path is a symlink"
                                             : "ledger open failed");
        }
        if (::fchmod(fd_, 0640) != 0)
            return fail(error, error_cap, "ledger chmod failed");
        struct stat opened {};
        if (::fstat(fd_, &opened) != 0 || !S_ISREG(opened.st_mode))
            return fail(error, error_cap, "ledger is not a regular file");
        if (static_cast<uint64_t>(opened.st_size) == 0) {
            if (!write_header(error, error_cap)) return false;
        } else {
            if (static_cast<uint64_t>(opened.st_size) != bytes_) {
                // Torn tail (or a fully written file we just scanned): align.
                if (::ftruncate(fd_, static_cast<off_t>(bytes_)) != 0)
                    return fail(error, error_cap, "ledger truncate failed");
                if (!durable())
                    return fail(error, error_cap, "ledger fsync failed");
            }
            file_size_ = bytes_;
        }
        return true;
#endif
    }

    void close() noexcept {
#if CLEDGER_POSIX
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

    // Durable intent, written before any network egress.
    bool record_intent(const IntentRecord& intent, char* error,
                       size_t error_cap) noexcept {
        if (!ready()) return fail(error, error_cap, "ledger not open");
        if (!valid_id(intent.client_order_id))
            return fail(error, error_cap, "invalid client order id");
        if (find(intent.client_order_id))
            return fail(error, error_cap, "duplicate client order id");
        if (slot_count_ >= kMaxTrackedOrders)
            return fail(error, error_cap, "ledger order table full");
        if (!ensure_capacity(error, error_cap)) return false;
        const Transition step = transition(OrderState::kUnknown,
                                           LedgerEvent::kIntentRecorded,
                                           Evidence::kLocalIntent);
        if (step.result != TransitionResult::kApplied)
            return fail(error, error_cap, "intent transition rejected");

        OrderSlot& slot = slots_[slot_count_++];
        slot.summary = OrderSummary{};
        std::memcpy(slot.summary.client_order_id, intent.client_order_id,
                    std::strlen(intent.client_order_id) + 1);
        slot.summary.state = step.next;
        slot.summary.signal_id = intent.signal_id;
        slot.summary.market_hash = intent.market_hash;
        slot.summary.price_fixed6 = intent.price_fixed6;
        slot.summary.shares_fixed6 = intent.shares_fixed6;
        slot.summary.notional_fixed6 = intent.notional_fixed6;
        slot.side = intent.side;
        slot.order_type = intent.order_type;

        uint8_t payload[kLedgerPayload];
        encode_payload(payload, RecordType::kIntent, slot.summary, slot.side,
                       slot.order_type, LedgerEvent::kIntentRecorded,
                       Evidence::kLocalIntent);
        return commit(payload, error, error_cap);
    }

    // Applies an event with its evidence and journals the resulting state.
    // Returns the transition result so the caller can block trading on
    // kNeedsReconcile / kIllegal.
    Transition record_transition(const char* client_order_id, LedgerEvent event,
                                 Evidence evidence,
                                 const char* venue_order_id,
                                 uint64_t filled_fixed6, char* error,
                                 size_t error_cap) noexcept {
        Transition failed{TransitionResult::kIllegal, OrderState::kUnknown};
        if (!ready()) {
            fail(error, error_cap, "ledger not open");
            return failed;
        }
        OrderSlot* slot = mutable_slot(client_order_id);
        if (!slot) {
            fail(error, error_cap, "unknown client order id");
            return failed;
        }
        const Transition step = transition(slot->summary.state, event, evidence,
                                           filled_fixed6,
                                           slot->summary.shares_fixed6);
        if (step.result == TransitionResult::kIllegal) {
            fail(error, error_cap, "illegal transition; quarantine required");
            return step;
        }
        if (step.result == TransitionResult::kUnchanged) return step;
        // Capacity check before mutating memory: compacting after the slot is
        // updated would prune the very order this transition belongs to.
        if (!ensure_capacity(error, error_cap)) return failed;

        if (venue_order_id && *venue_order_id) {
            const size_t length = std::strlen(venue_order_id);
            if (length >= kVenueOrderIdChars) {
                fail(error, error_cap, "venue order id too long");
                return failed;
            }
            std::memcpy(slot->summary.venue_order_id, venue_order_id, length + 1);
        }
        if (filled_fixed6 > slot->summary.filled_fixed6)
            slot->summary.filled_fixed6 = filled_fixed6;
        slot->summary.state = step.next;

        uint8_t payload[kLedgerPayload];
        encode_payload(payload, RecordType::kStateChange, slot->summary,
                       slot->side, slot->order_type, event, evidence);
        if (!commit(payload, error, error_cap)) return failed;
        return step;
    }

    // Venue ids are assigned by the exchange; the private channel identifies
    // orders only by that id, so the ledger must resolve both directions.
    // Journaled transition for every order whose outcome stopped being
    // provable (heartbeat loss implies the venue canceled the account's open
    // orders). Terminal orders are untouched. Returns the number of orders
    // moved to UNKNOWN, or SIZE_MAX on a write failure.
    size_t mark_transport_lost(char* error, size_t error_cap) noexcept {
        if (!ready()) {
            fail(error, error_cap, "ledger not open");
            return SIZE_MAX;
        }
        size_t moved = 0;
        for (size_t i = 0; i < slot_count_; ++i) {
            if (!blocks_trading(slots_[i].summary.state)) continue;
            if (slots_[i].summary.state == OrderState::kUnknown) continue;
            const Transition step = transition(slots_[i].summary.state,
                                               LedgerEvent::kHeartbeatLost,
                                               Evidence::kNone);
            if (step.result != TransitionResult::kApplied) {
                fail(error, error_cap, "transport-lost transition rejected");
                return SIZE_MAX;
            }
            slots_[i].summary.state = step.next;
            uint8_t payload[kLedgerPayload];
            encode_payload(payload, RecordType::kStateChange, slots_[i].summary,
                           slots_[i].side, slots_[i].order_type,
                           LedgerEvent::kHeartbeatLost, Evidence::kNone);
            if (!commit(payload, error, error_cap)) return SIZE_MAX;
            ++moved;
        }
        return moved;
    }

    // Read-only iteration for the Phase 5 reconciler and for reporting. Slots
    // are dense while the ledger is open (0 .. order_count()); compaction only
    // rewrites the file, it never removes an in-memory slot.
    const OrderSummary* summary_at(size_t index) const noexcept {
        return index < slot_count_ ? &slots_[index].summary : nullptr;
    }
    // Index of the order holding `venue_order_id`, or SIZE_MAX when unknown.
    size_t index_of_venue(const char* venue_order_id) const noexcept {
        if (!venue_order_id || !*venue_order_id) return SIZE_MAX;
        for (size_t i = 0; i < slot_count_; ++i) {
            if (std::strcmp(slots_[i].summary.venue_order_id,
                            venue_order_id) == 0)
                return i;
        }
        return SIZE_MAX;
    }

    const OrderSummary* find_by_venue(const char* venue_order_id) const noexcept {
        if (!venue_order_id || !*venue_order_id) return nullptr;
        for (size_t i = 0; i < slot_count_; ++i) {
            if (std::strcmp(slots_[i].summary.venue_order_id,
                            venue_order_id) == 0)
                return &slots_[i].summary;
        }
        return nullptr;
    }

    const OrderSummary* find(const char* client_order_id) const noexcept {
        if (!client_order_id) return nullptr;
        for (size_t i = 0; i < slot_count_; ++i) {
            if (std::strcmp(slots_[i].summary.client_order_id,
                            client_order_id) == 0)
                return &slots_[i].summary;
        }
        return nullptr;
    }

    size_t order_count() const noexcept { return slot_count_; }
    size_t unreconciled_orders() const noexcept {
        size_t count = 0;
        for (size_t i = 0; i < slot_count_; ++i)
            if (blocks_trading(slots_[i].summary.state)) ++count;
        return count;
    }
    size_t unknown_orders() const noexcept {
        size_t count = 0;
        for (size_t i = 0; i < slot_count_; ++i)
            if (slots_[i].summary.state == OrderState::kUnknown) ++count;
        return count;
    }
    // Trading gate: the journal must be usable and every order terminal.
    bool gate_open() const noexcept {
        return ready() && unreconciled_orders() == 0;
    }

    uint64_t records_written() const noexcept { return records_; }
    uint64_t bytes_on_disk() const noexcept { return file_size_; }
    uint64_t torn_tail_bytes() const noexcept { return torn_tail_bytes_; }
    uint64_t journaled_intents() const noexcept { return counters_.intents; }
    uint64_t journaled_fills() const noexcept { return counters_.fills; }
    const char* path() const noexcept { return path_; }

    // Compaction prunes terminal orders and re-materializes live ones. It is
    // triggered automatically when the size cap is reached; callers may also
    // invoke it explicitly (tests, operations).
    bool compact(char* error, size_t error_cap) noexcept {
        if (!ready()) return fail(error, error_cap, "ledger not open");
        char tmp_path[sizeof(path_) + 16];
        std::snprintf(tmp_path, sizeof(tmp_path), "%s.compact.tmp", path_);
        ::unlink(tmp_path);
        const int tmp_fd =
            ::open(tmp_path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                   0640);
        if (tmp_fd < 0) return fail(error, error_cap, "compaction open failed");

        uint8_t payload[kLedgerPayload];
        encode_file_header(payload);
        if (!write_frame(tmp_fd, payload) ||
            !write_counters(tmp_fd, payload, error, error_cap)) {
            ::close(tmp_fd);
            ::unlink(tmp_path);
            return false;
        }
        uint64_t seq = 2;  // header + counters are the first two frames
        for (size_t i = 0; i < slot_count_; ++i) {
            if (is_terminal(slots_[i].summary.state)) continue;
            encode_payload(payload, RecordType::kRecovery, slots_[i].summary,
                           slots_[i].side, slots_[i].order_type,
                           LedgerEvent::kReconcilePresent, Evidence::kVenueRest);
            put_u64(payload + 8, ++seq);
            if (!write_frame(tmp_fd, payload)) {
                ::close(tmp_fd);
                ::unlink(tmp_path);
                return fail(error, error_cap, "compaction write failed");
            }
        }
        if (::fsync(tmp_fd) != 0 || ::close(tmp_fd) != 0) {
            ::unlink(tmp_path);
            return fail(error, error_cap, "compaction fsync failed");
        }
        if (::rename(tmp_path, path_) != 0)
            return fail(error, error_cap, "compaction rename failed");
#if CLEDGER_POSIX
        if (fd_ >= 0) ::close(fd_);
        fd_ = ::open(path_, O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0640);
        if (fd_ < 0) return fail(error, error_cap, "ledger reopen failed");
        sync_directory();
#endif
        file_size_ = static_cast<uint64_t>(seq) * kLedgerFrame;
        ++records_;  // the header of the compacted file
        return true;
    }

private:
    struct Counters {
        uint64_t intents = 0;
        uint64_t fills = 0;
        uint64_t terminal = 0;
    };

    struct OrderSlot {
        OrderSummary summary{};
        uint8_t side = 0;
        uint8_t order_type = 0;
    };

    static bool fail(char* error, size_t cap, const char* text) noexcept {
        if (error && cap) std::snprintf(error, cap, "%s", text);
        return false;
    }

    bool ready() const noexcept {
#if CLEDGER_POSIX
        return fd_ >= 0;
#else
        return false;
#endif
    }

    static bool valid_id(const char* id) noexcept {
        return id && *id && std::strlen(id) < kClientOrderIdChars;
    }

    OrderSlot* mutable_slot(const char* client_order_id) noexcept {
        if (!client_order_id) return nullptr;
        for (size_t i = 0; i < slot_count_; ++i) {
            if (std::strcmp(slots_[i].summary.client_order_id,
                            client_order_id) == 0)
                return &slots_[i];
        }
        return nullptr;
    }

    void encode_file_header(uint8_t* payload) noexcept {
        std::memset(payload, 0, kLedgerPayload);
        payload[0] = static_cast<uint8_t>(RecordType::kFileHeader);
        std::memcpy(payload + 8, kLedgerMagic, sizeof(kLedgerMagic));
        put_u64(payload + 16, static_cast<uint64_t>(std::time(nullptr)));
    }

    void encode_payload(uint8_t* payload, RecordType type,
                        const OrderSummary& summary, uint8_t side,
                        uint8_t order_type, LedgerEvent event,
                        Evidence evidence) noexcept {
        std::memset(payload, 0, kLedgerPayload);
        payload[0] = static_cast<uint8_t>(type);
        payload[1] = static_cast<uint8_t>(summary.state);
        payload[2] = static_cast<uint8_t>(event);
        payload[3] = static_cast<uint8_t>(evidence);
        payload[4] = side;
        payload[5] = order_type;
        put_u64(payload + 8, ++seq_);
        put_u64(payload + 16, monotonic_ns());
        put_u64(payload + 24, summary.signal_id);
        put_u64(payload + 32, summary.market_hash);
        put_u64(payload + 40, summary.price_fixed6);
        put_u64(payload + 48, summary.shares_fixed6);
        put_u64(payload + 56, summary.notional_fixed6);
        put_u64(payload + 64, summary.filled_fixed6);
        std::memcpy(payload + 72, summary.client_order_id,
                    std::strlen(summary.client_order_id) + 1);
        std::memcpy(payload + 112, summary.venue_order_id,
                    std::strlen(summary.venue_order_id) + 1);
    }

    static bool decode_payload(const uint8_t* payload, RecordType& type,
                               OrderSummary& summary, uint8_t& side,
                               uint8_t& order_type, LedgerEvent& event,
                               Evidence& evidence) noexcept {
        type = static_cast<RecordType>(payload[0]);
        if (type == RecordType::kFileHeader || type == RecordType::kCounters)
            return true;
        summary.state = static_cast<OrderState>(payload[1]);
        if (summary.state >= OrderState::kCount) return false;
        event = static_cast<LedgerEvent>(payload[2]);
        if (event >= LedgerEvent::kCount) return false;
        evidence = static_cast<Evidence>(payload[3]);
        if (evidence > Evidence::kVenueRest) return false;
        if (payload[4] > 1) return false;
        side = payload[4];
        order_type = payload[5];
        summary.last_seq = get_u64(payload + 8);
        summary.last_timestamp_ns = get_u64(payload + 16);
        summary.signal_id = get_u64(payload + 24);
        summary.market_hash = get_u64(payload + 32);
        summary.price_fixed6 = get_u64(payload + 40);
        summary.shares_fixed6 = get_u64(payload + 48);
        summary.notional_fixed6 = get_u64(payload + 56);
        summary.filled_fixed6 = get_u64(payload + 64);
        if (!copy_string(payload + 72, kClientOrderIdChars,
                         summary.client_order_id,
                         sizeof(summary.client_order_id)))
            return false;
        if (!copy_string(payload + 112, kVenueOrderIdChars,
                         summary.venue_order_id,
                         sizeof(summary.venue_order_id)))
            return false;
        if (type != RecordType::kRecovery && !valid_id(summary.client_order_id))
            return false;
        return true;
    }

    static bool copy_string(const uint8_t* source, size_t field_size,
                            char* out, size_t out_cap) noexcept {
        const void* terminator = std::memchr(source, '\0', field_size);
        if (!terminator) return false;
        const size_t length =
            static_cast<const uint8_t*>(terminator) - source;
        if (length + 1 > out_cap) return false;
        std::memcpy(out, source, length);
        out[length] = '\0';
        return true;
    }

    // ── durability plumbing ─────────────────────────────────────────────────

    bool write_all(int fd, const void* data, size_t length) noexcept {
        const uint8_t* cursor = static_cast<const uint8_t*>(data);
        while (length > 0) {
            const ssize_t written = ::write(fd, cursor, length);
            if (written <= 0) {
                if (written < 0 && errno == EINTR) continue;
                return false;
            }
            cursor += written;
            length -= static_cast<size_t>(written);
        }
        return true;
    }

    bool write_frame(int fd, const uint8_t* payload) noexcept {
        uint8_t frame[kLedgerFrame];
        put_u32(frame, static_cast<uint32_t>(kLedgerPayload));
        put_u32(frame + 4, crc32_ieee(payload, kLedgerPayload));
        std::memcpy(frame + kLedgerFrameHeader, payload, kLedgerPayload);
        return write_all(fd, frame, sizeof(frame));
    }

    bool durable() noexcept {
        if (!options_.fsync_records) return true;
#if CLEDGER_POSIX
        return fsync(fd_) == 0;
#else
        return true;
#endif
    }

    bool ensure_capacity(char* error, size_t error_cap) noexcept {
        if (file_size_ + kLedgerFrame <= options_.max_bytes) return true;
        return compact(error, error_cap);
    }

    bool commit(const uint8_t* payload, char* error, size_t error_cap) noexcept {
        if (file_size_ + kLedgerFrame > options_.max_bytes)
            return fail(error, error_cap, "ledger full: compaction did not free space");
        if (!write_frame(fd_, payload)) {
            file_size_ += 0;
            return fail(error, error_cap, "ledger write failed");
        }
        if (!durable()) return fail(error, error_cap, "ledger fsync failed");
        file_size_ += kLedgerFrame;
        ++records_;
        if (static_cast<RecordType>(payload[0]) == RecordType::kIntent)
            ++counters_.intents;
        return true;
    }

    bool write_header(char* error, size_t error_cap) noexcept {
        uint8_t payload[kLedgerPayload];
        encode_file_header(payload);
        if (!write_frame(fd_, payload))
            return fail(error, error_cap, "ledger header write failed");
        if (!durable()) return fail(error, error_cap, "ledger header fsync failed");
        file_size_ += kLedgerFrame;
        ++records_;
        return true;
    }

    bool write_counters(int fd, uint8_t* payload, char* error,
                        size_t error_cap) noexcept {
        std::memset(payload, 0, kLedgerPayload);
        payload[0] = static_cast<uint8_t>(RecordType::kCounters);
        put_u64(payload + 8, 2);
        put_u64(payload + 24, counters_.intents);
        put_u64(payload + 32, counters_.fills);
        put_u64(payload + 40, counters_.terminal);
        if (!write_frame(fd, payload))
            return fail(error, error_cap, "counter write failed");
        return true;
    }

#if CLEDGER_POSIX
    void sync_directory() noexcept {
        char dir[sizeof(path_) + 1];
        std::snprintf(dir, sizeof(dir), "%s", path_);
        char* slash = std::strrchr(dir, '/');
        if (!slash) return;
        *slash = '\0';
        const int dir_fd = ::open(dir[0] ? dir : "/", O_RDONLY | O_CLOEXEC);
        if (dir_fd >= 0) {
            ::fsync(dir_fd);
            ::close(dir_fd);
        }
    }

    // Scans the existing file, applies every good frame to the in-memory table,
    // and records the offset up to which the file is self-consistent.
    bool recover(char* error, size_t error_cap) noexcept {
        const int fd = ::open(path_, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return fail(error, error_cap, "ledger read open failed");
        struct stat info {};
        if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
            ::close(fd);
            return fail(error, error_cap, "ledger is not a regular file");
        }
        const uint64_t size = static_cast<uint64_t>(info.st_size);
        uint64_t offset = 0;
        uint64_t last_good = 0;
        bool header_seen = false;
        bool corrupt = false;
        bool torn = false;
        uint8_t frame[kLedgerFrame];
        while (offset + kLedgerFrame <= size) {
            // A frame that fails validation is only tolerable when nothing
            // follows it: that is a torn tail from an interrupted write. A bad
            // frame *between* good ones is corruption and must not be hidden.
            const bool has_following_bytes = offset + kLedgerFrame < size;
            if (!read_exact(fd, offset, frame, sizeof(frame))) {
                corrupt = has_following_bytes;
                torn = !has_following_bytes;
                break;
            }
            const uint32_t length = get_u32(frame);
            const uint32_t crc = get_u32(frame + 4);
            const uint8_t* payload = frame + kLedgerFrameHeader;
            if (length != kLedgerPayload ||
                crc != crc32_ieee(payload, kLedgerPayload)) {
                corrupt = has_following_bytes;
                torn = !has_following_bytes;
                break;
            }
            RecordType type{};
            OrderSummary summary{};
            uint8_t side = 0, order_type = 0;
            LedgerEvent event{};
            Evidence evidence{};
            if (!decode_payload(payload, type, summary, side, order_type, event,
                                evidence)) {
                corrupt = has_following_bytes;
                torn = !has_following_bytes;
                break;
            }
            if (type == RecordType::kFileHeader) {
                if (header_seen ||
                    std::memcmp(payload + 8, kLedgerMagic, sizeof(kLedgerMagic)) !=
                        0) {
                    corrupt = true;
                    break;
                }
                header_seen = true;
            } else if (!header_seen) {
                corrupt = true;
                break;
            } else if (type == RecordType::kCounters) {
                // Compaction footer: informational for audits, never a source
                // of truth for recovery.
            } else {
                if (!apply_recovered(type, summary, side, order_type)) {
                    corrupt = true;
                    break;
                }
            }
            offset += kLedgerFrame;
            last_good = offset;
            if (summary.last_seq > seq_) seq_ = summary.last_seq;
            ++records_;
        }
        if (!corrupt && offset < size) torn = true;
        const uint64_t torn_bytes = torn ? size - last_good : 0;
        ::close(fd);
        if (corrupt) {
            seq_ = 0;
            records_ = 0;
            slot_count_ = 0;
            counters_ = Counters{};
            return fail(error, error_cap,
                        "ledger corrupt: CRC or layout mismatch before EOF");
        }
        torn_tail_bytes_ = torn_bytes;
        bytes_ = last_good;
        return true;
    }

    bool apply_recovered(RecordType type, const OrderSummary& summary,
                         uint8_t side, uint8_t order_type) noexcept {
        if (type == RecordType::kStateChange && summary.state >= OrderState::kCount)
            return false;
        if (type == RecordType::kIntent) {
            if (slot_count_ >= kMaxTrackedOrders) return false;
            if (find(summary.client_order_id)) return false;
            OrderSlot& slot = slots_[slot_count_++];
            slot.summary = summary;
            slot.side = side;
            slot.order_type = order_type;
            ++counters_.intents;
            return true;
        }
        if (type == RecordType::kRecovery) {
            if (summary.client_order_id[0] == '\0') return true;  // counters-only
            if (slot_count_ >= kMaxTrackedOrders) return false;
            if (find(summary.client_order_id)) return false;
            OrderSlot& slot = slots_[slot_count_++];
            slot.summary = summary;
            slot.side = side;
            slot.order_type = order_type;
            if (summary.state == OrderState::kFilled ||
                summary.state == OrderState::kPartiallyFilled)
                ++counters_.fills;
            return true;
        }
        if (type == RecordType::kStateChange) {
            OrderSlot* slot = mutable_slot(summary.client_order_id);
            if (!slot) return false;
            if (summary.filled_fixed6 > slot->summary.filled_fixed6 &&
                summary.filled_fixed6 > 0)
                ++counters_.fills;
            slot->summary.state = summary.state;
            slot->summary.filled_fixed6 = summary.filled_fixed6;
            slot->summary.last_seq = summary.last_seq;
            slot->summary.last_timestamp_ns = summary.last_timestamp_ns;
            if (summary.venue_order_id[0])
                std::memcpy(slot->summary.venue_order_id,
                            summary.venue_order_id, kVenueOrderIdChars);
            if (is_terminal(summary.state)) ++counters_.terminal;
            return true;
        }
        return false;  // unknown record type in a good-CRC frame
    }

    static bool read_exact(int fd, uint64_t offset, void* buffer,
                           size_t length) noexcept {
        uint8_t* cursor = static_cast<uint8_t*>(buffer);
        while (length > 0) {
            const ssize_t got = ::pread(fd, cursor, length,
                                        static_cast<off_t>(offset));
            if (got <= 0) {
                if (got < 0 && errno == EINTR) continue;
                return false;
            }
            cursor += got;
            offset += static_cast<uint64_t>(got);
            length -= static_cast<size_t>(got);
        }
        return true;
    }
#endif  // CLEDGER_POSIX

    char path_[192]{};
    OpenOptions options_{};
    int fd_ = -1;
    uint64_t seq_ = 0;
    uint64_t records_ = 0;
    uint64_t bytes_ = 0;
    uint64_t file_size_ = 0;
    uint64_t torn_tail_bytes_ = 0;
    size_t slot_count_ = 0;
    Counters counters_{};
    OrderSlot slots_[kMaxTrackedOrders]{};
};

}  // namespace cledger

#endif  // ORDER_LEDGER_HPP
