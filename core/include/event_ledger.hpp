#ifndef EVENT_LEDGER_HPP
#define EVENT_LEDGER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Persistent, write-ahead event ledger + derived account/order state.
//
// Design goals (live-safety requirements):
//   1. Every event is durably appended BEFORE derived state is updated
//      (write-ahead).  A crash loses in-memory state, never history.
//   2. Duplicate events do not change the result: each event carries a 32-byte
//      idempotency key derived from (event_type, order_id, trade_id,
//      venue_timestamp, source).  Keys are checked exactly (bounded ring that
//      is persisted in the checkpoint) and every applier is additionally
//      semantically idempotent, so replaying a journal after a checkpoint
//      cannot double-count a fill.
//   3. Derived state is fully reconstructible from the journal alone.
//   4. Startup loads the last checkpoint, then replays the journal.
//   5. A corrupt ledger (bad magic, bad checksum, impossible length, mid-file
//      damage) sets `corrupt()` and refuses to open: the bot must not trade on
//      unknown state.  Only a *trailing* incomplete record — the signature of a
//      crash during append — is discarded, and that is reported.
//
// Storage: one append-only journal plus an atomically replaced checkpoint.
// No external database dependency.  SQLite/PostgreSQL were considered and
// rejected: they would add a third-party dependency to a build that today
// needs only libsecp256k1 (+libcurl/OpenSSL for the network variant), the
// access pattern here is a strictly ordered append-only log with a small
// bounded working set, and durability semantics stay auditable in one file
// instead of depending on a database's internal recovery behaviour.  If the
// working set must grow beyond the bounded maps below, revisit explicitly.
//
// Files (directory configurable, default /var/lib/crowdintel):
//   state.journal      append-only event records
//   state.checkpoint   atomic snapshot of derived state + dedup keys
//   state.checkpoint.tmp  transient image (renamed into place)
// ─────────────────────────────────────────────────────────────────────────────

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"
#include "asset_type.hpp"
#include "order_state.hpp"

namespace ledger {

// ── On-disk identifiers ─────────────────────────────────────────────────────
inline constexpr uint32_t K_JOURNAL_MAGIC = 0x4A4C4443U;   // "CDLJ"
inline constexpr uint64_t K_CHECKPOINT_MAGIC = 0x4B434443ULL;
inline constexpr uint16_t K_FORMAT_VERSION = 1;

inline constexpr size_t K_MAX_ORDER_ID = 80;
inline constexpr size_t K_MAX_TRADE_ID = 80;
inline constexpr size_t K_MAX_TOKEN_ID = 80;      // uint256 decimal (<=78 digits)
inline constexpr size_t K_MAX_CONDITION_ID = 70;  // 0x + 64 hex + NUL
inline constexpr size_t K_MAX_ADDRESS = 43;       // 0x + 40 hex + NUL
inline constexpr size_t K_MAX_REASON = 96;
inline constexpr size_t K_MAX_STATUS = 24;
inline constexpr size_t K_MAX_HEARTBEAT_ID = 80;

enum class EventType : uint16_t {
    NONE = 0,
    ORDER_LOCAL_CREATED = 1,
    ORDER_SIGNED = 2,
    ORDER_SUBMITTING = 3,
    ORDER_SUBMIT_RESULT = 4,
    ORDER_STATE = 5,
    ORDER_UNKNOWN = 6,
    VENUE_ORDER_ADOPTED = 7,
    FILL = 8,
    TRADE_STATUS = 9,
    BALANCE_SNAPSHOT = 10,
    ALLOWANCE_SNAPSHOT = 11,
    POSITION_SNAPSHOT = 12,
    RISK_RESERVE = 13,
    RISK_RELEASE = 14,
    HEARTBEAT = 15,
    RECONCILIATION_RUN = 16,
    STATE_EVENT = 17,
    METADATA_SNAPSHOT = 18,
    ILLEGAL_TRANSITION = 19,
    CHECKPOINT = 20
};

inline const char* event_type_name(EventType type) noexcept {
    switch (type) {
        case EventType::NONE: return "none";
        case EventType::ORDER_LOCAL_CREATED: return "order_local_created";
        case EventType::ORDER_SIGNED: return "order_signed";
        case EventType::ORDER_SUBMITTING: return "order_submitting";
        case EventType::ORDER_SUBMIT_RESULT: return "order_submit_result";
        case EventType::ORDER_STATE: return "order_state";
        case EventType::ORDER_UNKNOWN: return "order_unknown";
        case EventType::VENUE_ORDER_ADOPTED: return "venue_order_adopted";
        case EventType::FILL: return "fill";
        case EventType::TRADE_STATUS: return "trade_status";
        case EventType::BALANCE_SNAPSHOT: return "balance_snapshot";
        case EventType::ALLOWANCE_SNAPSHOT: return "allowance_snapshot";
        case EventType::POSITION_SNAPSHOT: return "position_snapshot";
        case EventType::RISK_RESERVE: return "risk_reserve";
        case EventType::RISK_RELEASE: return "risk_release";
        case EventType::HEARTBEAT: return "heartbeat";
        case EventType::RECONCILIATION_RUN: return "reconciliation_run";
        case EventType::STATE_EVENT: return "state_event";
        case EventType::METADATA_SNAPSHOT: return "metadata_snapshot";
        case EventType::ILLEGAL_TRANSITION: return "illegal_transition";
        case EventType::CHECKPOINT: return "checkpoint";
    }
    return "invalid";
}

// Event field identifiers (TLV payload).  Stable: the journal is long-lived.
enum FieldId : uint8_t {
    F_ORDER_ID = 1,
    F_TRADE_ID = 2,
    F_VENUE_TS_MS = 3,
    F_STATE = 4,
    F_SIDE = 5,
    F_PRICE_RAW = 6,
    F_SIZE_RAW = 7,
    F_MATCHED_RAW = 8,
    F_TOKEN_ID = 9,
    F_CONDITION_ID = 10,
    F_REASON = 11,
    F_MAKER_AMOUNT = 12,
    F_TAKER_AMOUNT = 13,
    F_BALANCE = 14,
    F_ALLOWANCE = 15,
    F_SPENDER = 16,
    F_HEARTBEAT_ID = 17,
    F_RESULT = 18,
    F_SOURCE = 19,
    F_ORDER_TYPE = 20,
    F_SALT = 21,
    F_RUN_ID = 22,
    F_FEE_BPS = 23,
    F_STATUS_TEXT = 24,
    F_TICK_RAW = 25,
    F_MIN_SIZE_RAW = 26,
    F_NEG_RISK = 27,
    F_FEE_RATE_MICRO = 28,
    F_ASSET_TYPE = 29,
    F_HTTP_CODE = 30,
    F_RESERVE_AMOUNT = 31,
    F_ACCEPTING_ORDERS = 32
};

// Observation source.  Recorded on every event so reconciliation can tell a
// stream observation from an authoritative REST read.
enum class Source : uint8_t { LOCAL = 0, REST = 1, USER_WS = 2, MARKET_WS = 3, RPC = 4 };

inline const char* source_name(Source source) noexcept {
    switch (source) {
        case Source::LOCAL: return "local";
        case Source::REST: return "rest";
        case Source::USER_WS: return "user_ws";
        case Source::MARKET_WS: return "market_ws";
        case Source::RPC: return "rpc";
    }
    return "invalid";
}

// ── Derived-state records ───────────────────────────────────────────────────
struct OrderRecord {
    char order_id[K_MAX_ORDER_ID]{};  // map key: must stay first
    char token_id[K_MAX_TOKEN_ID]{};
    char condition_id[K_MAX_CONDITION_ID]{};
    char order_type[8]{};
    char venue_status[K_MAX_STATUS]{};
    char unknown_reason[K_MAX_REASON]{};
    OrderState state = OrderState::NONE;
    uint8_t side = 0;
    uint64_t price_raw = 0;
    uint64_t original_size = 0;
    uint64_t matched_size = 0;
    uint64_t maker_amount = 0;
    uint64_t taker_amount = 0;
    uint64_t salt = 0;
    uint64_t created_wall_ns = 0;
    uint64_t updated_wall_ns = 0;
    uint64_t venue_ts_ms = 0;
    uint64_t reserved_amount = 0;
    bool reservation_active = false;
    bool externally_observed = false;  // seen on the venue, never sent by us
    bool needs_confirmation = false;   // resolvable only by an authoritative read
    uint32_t illegal_transitions = 0;
};

struct FillRecord {
    char trade_id[K_MAX_TRADE_ID]{};  // map key: must stay first
    char order_id[K_MAX_ORDER_ID]{};
    char token_id[K_MAX_TOKEN_ID]{};
    uint8_t trade_status = 0;  // venue_status::TradeKind
    uint8_t side = 0;
    bool counts_as_fill = false;
    bool credited = false;
    uint64_t price_raw = 0;
    uint64_t size_raw = 0;
    uint64_t fee_rate_bps = 0;
    uint64_t venue_ts_ms = 0;
    uint64_t first_seen_wall_ns = 0;
    uint64_t last_seen_wall_ns = 0;
};

struct AssetBalance {
    char key[K_MAX_TOKEN_ID + K_MAX_ADDRESS + 8]{};  // map key: must stay first
    char token_id[K_MAX_TOKEN_ID]{};                 // empty for collateral
    char spender[K_MAX_ADDRESS]{};
    uint8_t asset_type = 0;
    bool balance_valid = false;
    bool allowance_valid = false;
    uint64_t balance = 0;
    uint64_t allowance = 0;
    uint64_t observed_wall_ns = 0;
};

struct PositionRecord {
    char token_id[K_MAX_TOKEN_ID]{};  // map key: must stay first
    uint64_t shares = 0;              // confirmed inventory
    uint64_t reserved_shares = 0;     // committed by open/unknown SELL orders
    uint64_t observed_wall_ns = 0;
};

struct HeartbeatState {
    char heartbeat_id[K_MAX_HEARTBEAT_ID]{};
    uint64_t last_accepted_wall_ns = 0;
    uint64_t last_attempt_wall_ns = 0;
    uint64_t consecutive_failures = 0;
    bool chain_active = false;
    bool invalidated = false;  // venue rejected the id: assume orders were killed
};

struct ReconciliationRun {
    uint64_t run_id = 0;
    uint64_t started_wall_ns = 0;
    uint64_t finished_wall_ns = 0;
    uint8_t ready = 0;  // 1 = READY, 0 = BLOCKED
    uint8_t reason_count = 0;
    char reasons[6][40]{};
};

struct MetadataRecord {
    char condition_id[K_MAX_CONDITION_ID]{};
    char token_id[K_MAX_TOKEN_ID]{};
    uint64_t tick_raw = 0;
    uint64_t min_size_raw = 0;
    uint64_t fee_rate_micro = 0;  // fee coefficient ×1e6 (0.07 → 70000)
    bool neg_risk = false;
    bool accepting_orders = false;
    uint64_t observed_wall_ns = 0;
};

struct StateEvent {
    uint64_t wall_ns = 0;
    char reason[K_MAX_REASON]{};
};

// ── Event ───────────────────────────────────────────────────────────────────
inline constexpr size_t K_MAX_FIELDS = 14;
inline constexpr size_t K_MAX_FIELD_LEN = 128;

struct EventField {
    uint8_t id = 0;
    uint16_t len = 0;
    uint8_t value[K_MAX_FIELD_LEN]{};
};

struct Event {
    EventType type = EventType::NONE;
    Source source = Source::LOCAL;
    uint64_t wall_ns = 0;
    uint64_t venue_ts_ms = 0;
    uint8_t key[32]{};  // idempotency key
    uint8_t fields = 0;
    EventField field[K_MAX_FIELDS]{};

    void reset() noexcept {
        type = EventType::NONE;
        source = Source::LOCAL;
        wall_ns = 0;
        venue_ts_ms = 0;
        std::memset(key, 0, sizeof(key));
        fields = 0;
    }

    bool add_u64(uint8_t id, uint64_t value) noexcept {
        if (fields >= K_MAX_FIELDS) return false;
        EventField& slot = field[fields];
        slot.id = id;
        slot.len = 8;
        for (int i = 0; i < 8; ++i)
            slot.value[i] = static_cast<uint8_t>((value >> (8 * (7 - i))) & 0xFFULL);
        ++fields;
        return true;
    }

    bool add_u8(uint8_t id, uint8_t value) noexcept {
        if (fields >= K_MAX_FIELDS) return false;
        EventField& slot = field[fields];
        slot.id = id;
        slot.len = 1;
        slot.value[0] = value;
        ++fields;
        return true;
    }

    bool add_str(uint8_t id, const char* value) noexcept {
        if (!value) return false;
        const size_t length = std::strlen(value);
        if (length == 0 || length > K_MAX_FIELD_LEN) return false;
        if (fields >= K_MAX_FIELDS) return false;
        EventField& slot = field[fields];
        slot.id = id;
        slot.len = static_cast<uint16_t>(length);
        std::memcpy(slot.value, value, length);
        ++fields;
        return true;
    }

    bool get_u64(uint8_t id, uint64_t& out) const noexcept {
        const EventField* f = find(id);
        if (!f || f->len != 8) return false;
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value = (value << 8) | f->value[i];
        out = value;
        return true;
    }

    bool get_u8(uint8_t id, uint8_t& out) const noexcept {
        const EventField* f = find(id);
        if (!f || f->len != 1) return false;
        out = f->value[0];
        return true;
    }

    bool get_str(uint8_t id, char* out, size_t cap) const noexcept {
        const EventField* f = find(id);
        if (!f || cap == 0 || static_cast<size_t>(f->len) + 1 > cap) return false;
        std::memcpy(out, f->value, f->len);
        out[f->len] = '\0';
        return true;
    }

    const EventField* find(uint8_t id) const noexcept {
        for (uint8_t i = 0; i < fields; ++i)
            if (field[i].id == id) return &field[i];
        return nullptr;
    }
};

// ── Bounded helpers ─────────────────────────────────────────────────────────
inline uint64_t fnv1a(const void* data, size_t len) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

inline uint64_t now_wall_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t now_mono_ms() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

// Idempotency key: SHA-256 over the canonical identity of the observation.
// Arrival order is deliberately NOT part of the key, so duplicated or
// out-of-order deliveries collapse onto the same key.
inline void compute_event_key(EventType type, Source source, const char* order_id,
                              const char* trade_id, uint64_t venue_ts_ms,
                              uint64_t dedup_salt, uint8_t out[32]) noexcept {
    Sha256Ctx ctx;
    sha256_init(ctx);
    uint8_t header[16];
    header[0] = static_cast<uint8_t>(static_cast<uint16_t>(type) & 0xFFU);
    header[1] = static_cast<uint8_t>(static_cast<uint16_t>(type) >> 8);
    header[2] = static_cast<uint8_t>(source);
    for (int i = 0; i < 8; ++i)
        header[3 + i] = static_cast<uint8_t>((venue_ts_ms >> (8 * i)) & 0xFFULL);
    for (int i = 0; i < 5; ++i)
        header[11 + i] = static_cast<uint8_t>((dedup_salt >> (8 * i)) & 0xFFULL);
    sha256_update(ctx, header, sizeof(header));
    if (order_id && *order_id)
        sha256_update(ctx, reinterpret_cast<const uint8_t*>(order_id),
                      std::strlen(order_id));
    const uint8_t separator = 0x1FU;
    sha256_update(ctx, &separator, 1);
    if (trade_id && *trade_id)
        sha256_update(ctx, reinterpret_cast<const uint8_t*>(trade_id),
                      std::strlen(trade_id));
    sha256_final(ctx, out);
}

// ── Bounded string-keyed map (open addressing, no allocation) ───────────────
template <typename Record, size_t Capacity, size_t KeyOffset, size_t KeySize>
class StringMap {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(KeySize > 1, "key field must hold a NUL-terminated string");
public:
    using RecordType = Record;
    static constexpr size_t kCapacity = Capacity;

    static const char* key_of(const Record& record) noexcept {
        return reinterpret_cast<const char*>(&record) + KeyOffset;
    }

    Record* find(const char* key) noexcept {
        if (!key || !*key) return nullptr;
        size_t index = bucket(key);
        for (size_t probe = 0; probe < Capacity; ++probe) {
            Record& record = slots_[index];
            const char* slot_key = key_of(record);
            if (slot_key[0] == '\0') return nullptr;
            if (std::strcmp(slot_key, key) == 0) return &record;
            index = (index + 1) & (Capacity - 1);
        }
        return nullptr;
    }

    const Record* find(const char* key) const noexcept {
        return const_cast<StringMap*>(this)->find(key);
    }

    // Returns the record for `key`, inserting a zeroed one when absent.
    // Returns nullptr when the map is full or the key does not fit — callers
    // must fail closed rather than invent state.
    Record* insert(const char* key, bool& inserted) noexcept {
        inserted = false;
        if (!key || !*key || std::strlen(key) + 1 > KeySize) return nullptr;
        size_t index = bucket(key);
        for (size_t probe = 0; probe < Capacity; ++probe) {
            Record& record = slots_[index];
            char* slot_key = reinterpret_cast<char*>(&record) + KeyOffset;
            if (slot_key[0] == '\0') {
                if (count_ >= Capacity - 1) return nullptr;  // keep one slot free
                secure_zero(&record, sizeof(record));
                std::memcpy(slot_key, key, std::strlen(key) + 1);
                ++count_;
                inserted = true;
                return &record;
            }
            if (std::strcmp(slot_key, key) == 0) return &record;
            index = (index + 1) & (Capacity - 1);
        }
        return nullptr;
    }

    size_t size() const noexcept { return count_; }
    bool full() const noexcept { return count_ >= Capacity - 1; }
    Record* at(size_t index) noexcept { return index < Capacity ? &slots_[index] : nullptr; }
    const Record* at(size_t index) const noexcept {
        return index < Capacity ? &slots_[index] : nullptr;
    }
    void clear() noexcept {
        secure_zero(slots_, sizeof(slots_));
        count_ = 0;
    }

private:
    size_t bucket(const char* key) const noexcept {
        return static_cast<size_t>(fnv1a(key, std::strlen(key)) & (Capacity - 1));
    }

    Record slots_[Capacity]{};
    size_t count_ = 0;
};

// ── Deduplication ring (exact 32-byte keys) ─────────────────────────────────
template <size_t Capacity>
class KeyRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
public:
    static constexpr size_t kCapacity = Capacity;

    bool contains(const uint8_t key[32]) const noexcept {
        if (!populated_) return false;
        size_t index = bucket(key);
        for (size_t probe = 0; probe < 16; ++probe) {
            const size_t slot = (index + probe) & (Capacity - 1);
            if (std::memcmp(keys_[slot], key, 32) == 0) return true;
            if (is_empty(slot)) return false;
        }
        return false;
    }

    // Returns false when the key was already present.
    bool add(const uint8_t key[32]) noexcept {
        if (contains(key)) return false;
        size_t index = bucket(key);
        for (size_t probe = 0; probe < 16; ++probe) {
            const size_t slot = (index + probe) & (Capacity - 1);
            if (is_empty(slot)) {
                std::memcpy(keys_[slot], key, 32);
                ++count_;
                populated_ = true;
                return true;
            }
            index = (index + 1) & (Capacity - 1);
        }
        // Ring saturated: deterministic eviction keeps the ledger usable while
        // the semantic appliers still prevent double counting.
        const size_t slot = cursor_ & (Capacity - 1);
        ++cursor_;
        std::memcpy(keys_[slot], key, 32);
        populated_ = true;
        return true;
    }

    size_t size() const noexcept { return count_; }
    bool saturated() const noexcept { return count_ >= Capacity * 3 / 4; }
    void clear() noexcept {
        secure_zero(keys_, sizeof(keys_));
        count_ = 0;
        cursor_ = 0;
        populated_ = false;
    }
    const uint8_t (*raw() const noexcept)[32] { return keys_; }

private:
    bool is_empty(size_t slot) const noexcept {
        for (int i = 0; i < 32; ++i)
            if (keys_[slot][i] != 0) return false;
        return true;
    }

    static size_t bucket(const uint8_t key[32]) noexcept {
        uint64_t h = 1469598103934665603ULL;
        for (int i = 0; i < 32; ++i) { h ^= key[i]; h *= 1099511628211ULL; }
        return static_cast<size_t>(h & (Capacity - 1));
    }

    uint8_t keys_[Capacity][32]{};
    size_t count_ = 0;
    size_t cursor_ = 0;
    bool populated_ = false;
};

// ── Record framing ──────────────────────────────────────────────────────────
struct RecordHeader {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t type = 0;
    uint8_t source = 0;
    uint8_t field_count = 0;
    uint16_t reserved = 0;
    uint32_t payload_len = 0;
    uint64_t wall_ns = 0;
    uint64_t venue_ts_ms = 0;
    uint8_t key[32]{};
    uint8_t checksum[8]{};
};

inline constexpr size_t K_HEADER_SIZE = sizeof(RecordHeader);
inline constexpr size_t K_MAX_PAYLOAD = K_MAX_FIELDS * (3 + K_MAX_FIELD_LEN);
inline constexpr size_t K_MAX_RECORD = K_HEADER_SIZE + K_MAX_PAYLOAD;

// Checkpoint layout (little-endian u64 fields, then records, then SHA-256).
inline constexpr size_t K_CK_MAGIC = 0;
inline constexpr size_t K_CK_VERSION = 8;
inline constexpr size_t K_CK_APPLIED = 16;
inline constexpr size_t K_CK_LAST_EVENT = 24;
inline constexpr size_t K_CK_ORDERS_OFFSET = 32;
inline constexpr size_t K_CK_FILLS_OFFSET = 40;
inline constexpr size_t K_CK_POSITIONS_OFFSET = 48;
inline constexpr size_t K_CK_BALANCES_OFFSET = 56;
inline constexpr size_t K_CK_BODY_LEN = 64;
inline constexpr size_t K_CK_CHECKPOINTS = 72;
inline constexpr size_t K_CK_DATA = 80;

inline size_t serialize_payload(const Event& event, uint8_t* out, size_t cap) noexcept {
    size_t offset = 0;
    for (uint8_t i = 0; i < event.fields; ++i) {
        const EventField& field = event.field[i];
        if (offset + 3 + field.len > cap) return 0;
        out[offset++] = field.id;
        out[offset++] = static_cast<uint8_t>(field.len & 0xFFU);
        out[offset++] = static_cast<uint8_t>((field.len >> 8) & 0xFFU);
        std::memcpy(out + offset, field.value, field.len);
        offset += field.len;
    }
    return offset;
}

inline bool deserialize_payload(Event& event, const uint8_t* data, size_t len) noexcept {
    size_t offset = 0;
    uint8_t count = 0;
    while (offset < len) {
        if (offset + 3 > len || count >= K_MAX_FIELDS) return false;
        const uint8_t id = data[offset++];
        const uint16_t field_len = static_cast<uint16_t>(
            static_cast<uint16_t>(data[offset]) |
            static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8));
        offset += 2;
        if (field_len == 0 || field_len > K_MAX_FIELD_LEN || offset + field_len > len)
            return false;
        EventField& slot = event.field[count];
        slot.id = id;
        slot.len = field_len;
        std::memcpy(slot.value, data + offset, field_len);
        offset += field_len;
        ++count;
    }
    event.fields = count;
    return true;
}

inline void record_checksum(const RecordHeader& header, const uint8_t* payload,
                            size_t payload_len, uint8_t out[8]) noexcept {
    Sha256Ctx ctx;
    sha256_init(ctx);
    // Covers every header byte except the checksum field itself.
    const size_t checksum_offset = offsetof(RecordHeader, checksum);
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(&header), checksum_offset);
    if (payload_len && payload) sha256_update(ctx, payload, payload_len);
    uint8_t digest[32];
    sha256_final(ctx, digest);
    std::memcpy(out, digest, 8);
}

// ── The ledger ──────────────────────────────────────────────────────────────
class EventLedger {
public:
    static constexpr size_t K_ORDER_SLOTS = 1024;
    static constexpr size_t K_FILL_SLOTS = 2048;
    static constexpr size_t K_POSITION_SLOTS = 64;
    static constexpr size_t K_BALANCE_SLOTS = 64;
    static constexpr size_t K_DEDUP_SLOTS = 8192;
    static constexpr size_t K_MAX_RUNS = 16;
    static constexpr size_t K_STATE_EVENTS = 64;

    using OrderMap = StringMap<OrderRecord, K_ORDER_SLOTS,
                               offsetof(OrderRecord, order_id), K_MAX_ORDER_ID>;
    using FillMap = StringMap<FillRecord, K_FILL_SLOTS,
                              offsetof(FillRecord, trade_id), K_MAX_TRADE_ID>;
    using PositionMap = StringMap<PositionRecord, K_POSITION_SLOTS,
                                  offsetof(PositionRecord, token_id), K_MAX_TOKEN_ID>;
    using BalanceMap =
        StringMap<AssetBalance, K_BALANCE_SLOTS, offsetof(AssetBalance, key),
                  K_MAX_TOKEN_ID + K_MAX_ADDRESS + 8>;

    struct Options {
        char directory[192] = "/var/lib/crowdintel";
        bool fsync_each_append = true;  // WAL durability; relax only in tests
        bool create_directory = true;
        size_t checkpoint_every = 512;  // events between checkpoints
        bool read_only = false;
    };

    EventLedger() = default;
    ~EventLedger() { close(); }
    EventLedger(const EventLedger&) = delete;
    EventLedger& operator=(const EventLedger&) = delete;

    // Opens and recovers the ledger.  Returns false when the store is unusable;
    // `error` receives a stable, secret-free message.  A false return must
    // always disable trading.
    bool open(const Options& options, char* error, size_t error_cap) {
        options_ = options;
        corrupt_ = false;
        recovered_trailing_ = false;
        if (error && error_cap) error[0] = '\0';
        if (!options_.directory[0]) {
            set_error(error, error_cap, "ledger directory is not configured");
            corrupt_ = true;
            return false;
        }
        if (options_.create_directory && !options_.read_only) {
            if (::mkdir(options_.directory, 0700) != 0 && errno != EEXIST) {
                set_error(error, error_cap, "cannot create ledger directory");
                corrupt_ = true;
                return false;
            }
        }
        build_paths();
        if (!load_checkpoint(error, error_cap)) return false;
        if (!replay_journal(error, error_cap)) return false;
        if (options_.read_only) { opened_ = true; return true; }
        journal_fd_ = ::open(journal_path_,
                             O_RDWR | O_CREAT | O_CLOEXEC | O_APPEND, 0600);
        if (journal_fd_ < 0) {
            set_error(error, error_cap, "cannot open ledger journal");
            corrupt_ = true;
            return false;
        }
        opened_ = true;
        return true;
    }

    void close() {
        if (journal_fd_ >= 0) { ::close(journal_fd_); journal_fd_ = -1; }
        opened_ = false;
    }

    bool is_open() const noexcept { return opened_; }
    bool corrupt() const noexcept { return corrupt_; }
    bool recovered_trailing_record() const noexcept { return recovered_trailing_; }
    const char* journal_path() const noexcept { return journal_path_; }
    const char* checkpoint_path() const noexcept { return checkpoint_path_; }
    uint64_t journal_bytes() const noexcept { return journal_bytes_; }

    // Serialises every mutation and every cross-thread read of derived state.
    // The ledger is a cold-path component (order/fill/reconciliation rates), so
    // a single mutex is the right trade: it makes the multi-threaded topology
    // (gateway thread, user-channel thread, supervisor thread) race-free without
    // pushing locking discipline into every caller.  Holders must never perform
    // network I/O while holding it.
    std::unique_lock<std::mutex> guard() const {
        return std::unique_lock<std::mutex>(mutex_);
    }

    // Write-ahead commit: durably append, then apply.  On append failure the
    // derived state is untouched so the caller can fail closed.
    bool commit(Event& event, char* error, size_t error_cap) {
        std::lock_guard<std::mutex> lock(mutex_);
        return commit_locked(event, error, error_cap);
    }

    // Same contract, for callers that already hold guard().
    bool commit_locked(Event& event, char* error, size_t error_cap) {
        if (corrupt_ || !opened_ || journal_fd_ < 0) {
            set_error(error, error_cap, "ledger is not open for writing");
            return false;
        }
        if (event.type == EventType::NONE) {
            set_error(error, error_cap, "event type is required");
            return false;
        }
        if (event.wall_ns == 0) event.wall_ns = now_wall_ns();
        if (is_zero(event.key)) {
            set_error(error, error_cap, "event idempotency key is required");
            return false;
        }
        if (!append_record(event, error, error_cap)) return false;
        apply_locked(event);
        ++events_since_checkpoint_;
        if (events_since_checkpoint_ >= options_.checkpoint_every || dedup_.saturated())
            return checkpoint_locked(error, error_cap);
        return true;
    }

    // Applies an event to derived state without persisting it (replay/tests).
    // Idempotent: repeated application is a no-op.
    bool apply(const Event& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        return apply_locked(event);
    }

    bool apply_locked(const Event& event) {
        if (dedup_.contains(event.key)) {
            ++duplicates_ignored_;
            return false;
        }
        dedup_.add(event.key);
        switch (event.type) {
            case EventType::ORDER_LOCAL_CREATED:
            case EventType::ORDER_SIGNED:
            case EventType::ORDER_SUBMITTING:
            case EventType::ORDER_SUBMIT_RESULT:
            case EventType::ORDER_STATE:
            case EventType::ORDER_UNKNOWN:
            case EventType::VENUE_ORDER_ADOPTED:
                apply_order_event(event);
                break;
            case EventType::FILL:
            case EventType::TRADE_STATUS:
                apply_fill_event(event);
                break;
            case EventType::BALANCE_SNAPSHOT:
            case EventType::ALLOWANCE_SNAPSHOT:
                apply_balance_event(event);
                break;
            case EventType::POSITION_SNAPSHOT:
                apply_position_event(event);
                break;
            case EventType::RISK_RESERVE:
            case EventType::RISK_RELEASE:
                apply_reservation_event(event);
                break;
            case EventType::HEARTBEAT:
                apply_heartbeat_event(event);
                break;
            case EventType::RECONCILIATION_RUN:
                apply_reconciliation_event(event);
                break;
            case EventType::METADATA_SNAPSHOT:
                apply_metadata_event(event);
                break;
            case EventType::ILLEGAL_TRANSITION:
                apply_illegal_transition(event);
                break;
            case EventType::STATE_EVENT:
            case EventType::CHECKPOINT:
            case EventType::NONE:
            default:
                ++generic_state_events_;
                break;
        }
        last_event_wall_ns_ = event.wall_ns;
        ++applied_events_;
        return true;
    }

    // ── Derived-state accessors ─────────────────────────────────────────────
    // The map references below are NOT thread-safe on their own: use them from a
    // single thread (tests, replay, tooling) or while holding guard().  The
    // locked scalar accessors that follow are what other threads should use.
    OrderMap& orders() noexcept { return orders_; }
    const OrderMap& orders() const noexcept { return orders_; }
    FillMap& fills() noexcept { return fills_; }
    const FillMap& fills() const noexcept { return fills_; }
    PositionMap& positions() noexcept { return positions_; }
    const PositionMap& positions() const noexcept { return positions_; }
    BalanceMap& balances() noexcept { return balances_; }
    const BalanceMap& balances() const noexcept { return balances_; }
    HeartbeatState& heartbeat() noexcept { return heartbeat_; }
    const HeartbeatState& heartbeat() const noexcept { return heartbeat_; }
    MetadataRecord& metadata() noexcept { return metadata_; }
    const MetadataRecord& metadata() const noexcept { return metadata_; }

    const ReconciliationRun* last_run() const noexcept {
        return run_count_ ? &runs_[run_index_] : nullptr;
    }
    size_t run_count() const noexcept { return run_count_; }

    const AssetBalance* find_balance(const char* token_id, const char* spender,
                                     venue::AssetType type) const noexcept {
        char key[K_MAX_TOKEN_ID + K_MAX_ADDRESS + 8]{};
        balance_key(key, sizeof(key), token_id ? token_id : "",
                    spender ? spender : "", type);
        return balances_.find(key);
    }

    uint64_t applied_events() const noexcept { return applied_events_; }
    uint64_t replayed_events() const noexcept { return replayed_events_; }
    uint64_t duplicates_ignored() const noexcept { return duplicates_ignored_; }
    uint64_t illegal_transitions() const noexcept { return illegal_transitions_; }
    uint64_t generic_state_events() const noexcept { return generic_state_events_; }
    uint64_t last_event_wall_ns() const noexcept { return last_event_wall_ns_; }
    uint64_t checkpoints() const noexcept { return checkpoints_; }

    size_t state_event_count() const noexcept { return state_event_count_; }
    const StateEvent& state_event(size_t index) const noexcept {
        return state_events_ring_[index % K_STATE_EVENTS];
    }

    // Orders whose state forbids new submissions (UNKNOWN/SUBMITTING).
    size_t blocking_orders() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocking_orders_locked();
    }

    size_t open_orders() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_orders_locked();
    }

    // Confirmed sellable inventory for a token (position minus reservations).
    uint64_t available_inventory_locked(const char* token_id) const noexcept {
        const PositionRecord* position = positions_.find(token_id);
        if (!position) return 0;
        return position->shares > position->reserved_shares
                   ? position->shares - position->reserved_shares : 0;
    }

    uint64_t available_inventory(const char* token_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return available_inventory_locked(token_id);
    }

    // Thread-safe copies for cross-thread readers.
    bool order_copy(const char* order_id, OrderRecord& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const OrderRecord* record = orders_.find(order_id);
        if (!record) return false;
        out = *record;
        return true;
    }

    bool heartbeat_copy(HeartbeatState& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        out = heartbeat_;
        return true;
    }

    bool fill_copy(const char* trade_id, FillRecord& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const FillRecord* record = fills_.find(trade_id);
        if (!record) return false;
        out = *record;
        return true;
    }

    bool position_copy(const char* token_id, PositionRecord& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const PositionRecord* record = positions_.find(token_id);
        if (!record) return false;
        out = *record;
        return true;
    }

    size_t order_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return orders_.size();
    }

    // Copies the most recent reconciliation run (thread-safe).
    bool last_run_copy(ReconciliationRun& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!run_count_) return false;
        out = runs_[run_index_];
        return true;
    }

    size_t blocking_orders_locked() const noexcept {
        size_t count = 0;
        for (size_t i = 0; i < OrderMap::kCapacity; ++i) {
            const OrderRecord* record = orders_.at(i);
            if (record && record->order_id[0] &&
                order_state_blocks_new_orders(record->state))
                ++count;
        }
        return count;
    }

    size_t open_orders_locked() const noexcept {
        size_t count = 0;
        for (size_t i = 0; i < OrderMap::kCapacity; ++i) {
            const OrderRecord* record = orders_.at(i);
            if (record && record->order_id[0] && order_state_is_open(record->state))
                ++count;
        }
        return count;
    }

    // Worst-case collateral committed by open/unknown orders.
    uint64_t reserved_collateral() const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t total = 0;
        for (size_t i = 0; i < OrderMap::kCapacity; ++i) {
            const OrderRecord* record = orders_.at(i);
            if (record && record->order_id[0] && record->reservation_active)
                total += record->reserved_amount;
        }
        return total;
    }

    static void balance_key(char* out, size_t cap, const char* token_id,
                            const char* spender, venue::AssetType type) noexcept {
        std::snprintf(out, cap, "%s|%s|%u", token_id, spender,
                      static_cast<unsigned>(type));
    }

    // ── Checkpointing ───────────────────────────────────────────────────────
    static size_t checkpoint_buffer_size() noexcept {
        return K_CK_DATA + 32 + K_DEDUP_SLOTS * 32 + sizeof(HeartbeatState) +
               sizeof(MetadataRecord) + sizeof(ReconciliationRun) * K_MAX_RUNS +
               64 + K_ORDER_SLOTS * (sizeof(OrderRecord) + 8) +
               K_FILL_SLOTS * (sizeof(FillRecord) + 8) +
               K_POSITION_SLOTS * (sizeof(PositionRecord) + 8) +
               K_BALANCE_SLOTS * (sizeof(AssetBalance) + 8);
    }

    bool checkpoint(char* error, size_t error_cap) {
        std::lock_guard<std::mutex> lock(mutex_);
        return checkpoint_locked(error, error_cap);
    }

    bool checkpoint_locked(char* error, size_t error_cap) {
        if (options_.read_only) return true;
        const size_t capacity = checkpoint_buffer_size();
        uint8_t* buffer = static_cast<uint8_t*>(std::malloc(capacity));
        if (!buffer) {
            set_error(error, error_cap, "checkpoint allocation failed");
            return false;
        }
        size_t length = 0;
        const bool serialized = serialize_checkpoint(buffer, capacity, length);
        if (!serialized) {
            std::free(buffer);
            set_error(error, error_cap, "checkpoint image exceeds its bound");
            return false;
        }
        char tmp_path[384];
        std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", checkpoint_path_);
        const int fd = ::open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) {
            std::free(buffer);
            set_error(error, error_cap, "cannot create checkpoint image");
            corrupt_ = true;
            return false;
        }
        bool io_ok = write_all(fd, buffer, length);
        std::free(buffer);
        if (io_ok && options_.fsync_each_append) io_ok = ::fsync(fd) == 0;
        ::close(fd);
        if (!io_ok) {
            ::unlink(tmp_path);
            set_error(error, error_cap, "checkpoint write failed");
            corrupt_ = true;
            return false;
        }
        if (::rename(tmp_path, checkpoint_path_) != 0) {
            ::unlink(tmp_path);
            set_error(error, error_cap, "checkpoint rename failed");
            corrupt_ = true;
            return false;
        }
        sync_directory();
        // Rotation happens only after the checkpoint is durable.  A crash in
        // between replays already-applied events, which the dedup keys and the
        // semantic appliers make harmless.
        if (journal_fd_ >= 0) {
            if (::ftruncate(journal_fd_, 0) != 0) {
                set_error(error, error_cap, "journal truncation failed");
                corrupt_ = true;
                return false;
            }
            if (options_.fsync_each_append) ::fsync(journal_fd_);
            journal_bytes_ = 0;
        }
        events_since_checkpoint_ = 0;
        ++checkpoints_;
        return true;
    }

private:
    static constexpr uint8_t kZeroKey[32] = {0};

    static bool is_zero(const uint8_t key[32]) noexcept {
        return std::memcmp(key, kZeroKey, 32) == 0;
    }

    static void set_error(char* error, size_t cap, const char* message) {
        if (error && cap) std::snprintf(error, cap, "%s", message);
    }

    void build_paths() {
        std::snprintf(journal_path_, sizeof(journal_path_), "%s/state.journal",
                      options_.directory);
        std::snprintf(checkpoint_path_, sizeof(checkpoint_path_),
                      "%s/state.checkpoint", options_.directory);
    }

    static bool write_all(int fd, const uint8_t* data, size_t len) {
        size_t written = 0;
        while (written < len) {
            const ssize_t n = ::write(fd, data + written, len - written);
            if (n < 0) { if (errno == EINTR) continue; return false; }
            if (n == 0) return false;
            written += static_cast<size_t>(n);
        }
        return true;
    }

    void sync_directory() {
        const int dir_fd = ::open(options_.directory, O_RDONLY | O_CLOEXEC);
        if (dir_fd >= 0) { ::fsync(dir_fd); ::close(dir_fd); }
    }

    bool append_record(const Event& event, char* error, size_t error_cap) {
        RecordHeader header{};
        header.magic = K_JOURNAL_MAGIC;
        header.version = K_FORMAT_VERSION;
        header.type = static_cast<uint16_t>(event.type);
        header.source = static_cast<uint8_t>(event.source);
        header.field_count = event.fields;
        header.reserved = 0;
        header.wall_ns = event.wall_ns;
        header.venue_ts_ms = event.venue_ts_ms;
        std::memcpy(header.key, event.key, 32);

        uint8_t payload[K_MAX_PAYLOAD];
        const size_t payload_len = event.fields
                                       ? serialize_payload(event, payload, sizeof(payload))
                                       : 0;
        if (payload_len == 0 && event.fields != 0) {
            set_error(error, error_cap, "event payload exceeds its bound");
            return false;
        }
        header.payload_len = static_cast<uint32_t>(payload_len);
        record_checksum(header, payload_len ? payload : nullptr, payload_len,
                        header.checksum);

        uint8_t record[K_MAX_RECORD];
        std::memcpy(record, &header, K_HEADER_SIZE);
        if (payload_len) std::memcpy(record + K_HEADER_SIZE, payload, payload_len);
        const size_t total = K_HEADER_SIZE + payload_len;
        if (!write_all(journal_fd_, record, total)) {
            set_error(error, error_cap, "journal append failed");
            corrupt_ = true;
            return false;
        }
        if (options_.fsync_each_append && ::fsync(journal_fd_) != 0) {
            set_error(error, error_cap, "journal fsync failed");
            corrupt_ = true;
            return false;
        }
        journal_bytes_ += total;
        return true;
    }

    // ── Recovery ────────────────────────────────────────────────────────────
    bool load_checkpoint(char* error, size_t error_cap) {
        const int fd = ::open(checkpoint_path_, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) return true;  // first run
            set_error(error, error_cap, "cannot open checkpoint");
            corrupt_ = true;
            return false;
        }
        struct stat metadata{};
        if (::fstat(fd, &metadata) != 0 || metadata.st_size < 0 ||
            static_cast<uint64_t>(metadata.st_size) > checkpoint_buffer_size()) {
            ::close(fd);
            set_error(error, error_cap, "checkpoint size is invalid");
            corrupt_ = true;
            return false;
        }
        const size_t length = static_cast<size_t>(metadata.st_size);
        uint8_t* buffer = static_cast<uint8_t*>(std::malloc(length ? length : 1));
        if (!buffer) {
            ::close(fd);
            set_error(error, error_cap, "checkpoint allocation failed");
            corrupt_ = true;
            return false;
        }
        size_t total = 0;
        bool io_ok = true;
        while (total < length) {
            const ssize_t n = ::read(fd, buffer + total, length - total);
            if (n < 0) { if (errno == EINTR) continue; io_ok = false; break; }
            if (n == 0) break;
            total += static_cast<size_t>(n);
        }
        ::close(fd);
        if (!io_ok || total != length) {
            std::free(buffer);
            set_error(error, error_cap, "checkpoint read failed");
            corrupt_ = true;
            return false;
        }
        const bool parsed = deserialize_checkpoint(buffer, length);
        std::free(buffer);
        if (!parsed) {
            set_error(error, error_cap,
                      "checkpoint is corrupt; refusing to operate on unknown state");
            corrupt_ = true;
            return false;
        }
        return true;
    }

    bool replay_journal(char* error, size_t error_cap) {
        const int fd = ::open(journal_path_, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) return true;
            set_error(error, error_cap, "cannot open journal for replay");
            corrupt_ = true;
            return false;
        }
        struct stat metadata{};
        if (::fstat(fd, &metadata) != 0 || metadata.st_size < 0) {
            ::close(fd);
            set_error(error, error_cap, "journal stat failed");
            corrupt_ = true;
            return false;
        }
        uint64_t remaining = static_cast<uint64_t>(metadata.st_size);
        journal_bytes_ = remaining;
        while (remaining > 0) {
            if (remaining < K_HEADER_SIZE) {
                recovered_trailing_ = true;  // crash during append
                break;
            }
            uint8_t header_bytes[K_HEADER_SIZE];
            if (!read_exact(fd, header_bytes, K_HEADER_SIZE, remaining)) {
                recovered_trailing_ = true;
                break;
            }
            RecordHeader header{};
            std::memcpy(&header, header_bytes, K_HEADER_SIZE);
            if (header.magic != K_JOURNAL_MAGIC ||
                header.version != K_FORMAT_VERSION ||
                header.payload_len > K_MAX_PAYLOAD ||
                header.field_count > K_MAX_FIELDS) {
                ::close(fd);
                set_error(error, error_cap,
                          "journal record header is corrupt; refusing to operate");
                corrupt_ = true;
                return false;
            }
            uint8_t payload[K_MAX_PAYLOAD];
            if (header.payload_len) {
                if (remaining < header.payload_len ||
                    !read_exact(fd, payload, header.payload_len, remaining)) {
                    recovered_trailing_ = true;
                    break;
                }
            }
            uint8_t expected[8];
            record_checksum(header, header.payload_len ? payload : nullptr,
                            header.payload_len, expected);
            if (std::memcmp(expected, header.checksum, 8) != 0) {
                ::close(fd);
                set_error(error, error_cap,
                          "journal record checksum mismatch; refusing to operate");
                corrupt_ = true;
                return false;
            }
            Event event{};
            event.type = static_cast<EventType>(header.type);
            event.source = static_cast<Source>(header.source);
            event.wall_ns = header.wall_ns;
            event.venue_ts_ms = header.venue_ts_ms;
            std::memcpy(event.key, header.key, 32);
            if (header.payload_len &&
                !deserialize_payload(event, payload, header.payload_len)) {
                ::close(fd);
                set_error(error, error_cap,
                          "journal record payload is corrupt; refusing to operate");
                corrupt_ = true;
                return false;
            }
            apply_locked(event);
            ++replayed_events_;
        }
        ::close(fd);
        return true;
    }

    static bool read_exact(int fd, uint8_t* out, size_t len, uint64_t& remaining) {
        size_t total = 0;
        while (total < len) {
            const ssize_t n = ::read(fd, out + total, len - total);
            if (n < 0) { if (errno == EINTR) continue; return false; }
            if (n == 0) return false;
            total += static_cast<size_t>(n);
        }
        remaining -= total;
        return true;
    }

    // ── Appliers (all idempotent) ───────────────────────────────────────────
    void apply_order_event(const Event& event) {
        char order_id[K_MAX_ORDER_ID]{};
        if (!event.get_str(F_ORDER_ID, order_id, sizeof(order_id))) return;
        bool inserted = false;
        OrderRecord* record = orders_.insert(order_id, inserted);
        if (!record) {
            record_state_event(event.wall_ns, "order_map_full");
            return;  // bounded: never invent state
        }
        if (inserted) {
            record->created_wall_ns = event.wall_ns;
            char token[K_MAX_TOKEN_ID]{};
            if (event.get_str(F_TOKEN_ID, token, sizeof(token)))
                std::snprintf(record->token_id, sizeof(record->token_id), "%s", token);
            char condition[K_MAX_CONDITION_ID]{};
            if (event.get_str(F_CONDITION_ID, condition, sizeof(condition)))
                std::snprintf(record->condition_id, sizeof(record->condition_id),
                              "%s", condition);
        }
        if (event.type == EventType::VENUE_ORDER_ADOPTED)
            record->externally_observed = true;

        uint8_t byte_value = 0;
        if (event.get_u8(F_SIDE, byte_value)) record->side = byte_value;
        uint64_t value = 0;
        if (event.get_u64(F_PRICE_RAW, value)) record->price_raw = value;
        if (event.get_u64(F_SIZE_RAW, value)) record->original_size = value;
        if (event.get_u64(F_MAKER_AMOUNT, value)) record->maker_amount = value;
        if (event.get_u64(F_TAKER_AMOUNT, value)) record->taker_amount = value;
        if (event.get_u64(F_SALT, value)) record->salt = value;
        if (event.get_u64(F_MATCHED_RAW, value) && value >= record->matched_size)
            record->matched_size = value;
        char status[K_MAX_STATUS]{};
        if (event.get_str(F_STATUS_TEXT, status, sizeof(status)))
            std::snprintf(record->venue_status, sizeof(record->venue_status),
                          "%s", status);
        char order_type[8]{};
        if (event.get_str(F_ORDER_TYPE, order_type, sizeof(order_type)))
            std::snprintf(record->order_type, sizeof(record->order_type),
                          "%s", order_type);
        if (event.venue_ts_ms >= record->venue_ts_ms)
            record->venue_ts_ms = event.venue_ts_ms;

        uint8_t raw_state = 0;
        if (!event.get_u8(F_STATE, raw_state)) {
            record->updated_wall_ns = event.wall_ns;
            return;
        }
        const auto target = static_cast<OrderState>(raw_state);
        if (target >= OrderState::COUNT || target == OrderState::NONE) {
            record->updated_wall_ns = event.wall_ns;
            return;
        }
        if (!order_state::transition_allowed(record->state, target)) {
            ++record->illegal_transitions;
            ++illegal_transitions_;
            char reason[K_MAX_REASON];
            std::snprintf(reason, sizeof(reason), "illegal_transition %s->%s",
                          order_state_name(record->state), order_state_name(target));
            record_state_event(event.wall_ns, reason);
            record->updated_wall_ns = event.wall_ns;
            return;
        }
        record->state = target;
        record->updated_wall_ns = event.wall_ns;
        if (target == OrderState::UNKNOWN) {
            char reason[K_MAX_REASON]{};
            if (event.get_str(F_REASON, reason, sizeof(reason)))
                std::snprintf(record->unknown_reason, sizeof(record->unknown_reason),
                              "%s", reason);
            record->needs_confirmation = true;
        } else if (order_state_is_terminal(target)) {
            record->needs_confirmation = false;
            record->unknown_reason[0] = '\0';
            release_reservation(*record);
        } else {
            record->needs_confirmation = false;
        }
    }

    void apply_fill_event(const Event& event) {
        char trade_id[K_MAX_TRADE_ID]{};
        if (!event.get_str(F_TRADE_ID, trade_id, sizeof(trade_id))) return;
        bool inserted = false;
        FillRecord* fill = fills_.insert(trade_id, inserted);
        if (!fill) {
            record_state_event(event.wall_ns, "fill_map_full");
            return;
        }
        if (inserted) fill->first_seen_wall_ns = event.wall_ns;
        fill->last_seen_wall_ns = event.wall_ns;

        char order_id[K_MAX_ORDER_ID]{};
        if (event.get_str(F_ORDER_ID, order_id, sizeof(order_id)))
            std::snprintf(fill->order_id, sizeof(fill->order_id), "%s", order_id);
        char token[K_MAX_TOKEN_ID]{};
        if (event.get_str(F_TOKEN_ID, token, sizeof(token)))
            std::snprintf(fill->token_id, sizeof(fill->token_id), "%s", token);
        uint8_t byte_value = 0;
        if (event.get_u8(F_SIDE, byte_value)) fill->side = byte_value;
        if (event.get_u8(F_RESULT, byte_value)) {
            fill->trade_status = byte_value;
            const auto kind = static_cast<venue_status::TradeKind>(byte_value);
            fill->counts_as_fill = venue_status::trade_counts_as_fill(kind);
        }
        uint64_t value = 0;
        if (event.get_u64(F_PRICE_RAW, value)) fill->price_raw = value;
        if (event.get_u64(F_SIZE_RAW, value)) fill->size_raw = value;
        if (event.get_u64(F_FEE_BPS, value)) fill->fee_rate_bps = value;
        if (event.venue_ts_ms) fill->venue_ts_ms = event.venue_ts_ms;

        if (fill->counts_as_fill && !fill->credited) {
            fill->credited = true;
            credit_fill(*fill);
        } else if (!fill->counts_as_fill && fill->credited) {
            fill->credited = false;
            revert_fill(*fill);
        }

        if (fill->order_id[0]) {
            OrderRecord* order = orders_.find(fill->order_id);
            if (order) {
                uint64_t matched = 0;
                if (event.get_u64(F_MATCHED_RAW, matched) && matched > order->matched_size)
                    order->matched_size = matched;
                order->updated_wall_ns = event.wall_ns;
            }
        }
    }

    void credit_fill(const FillRecord& fill) {
        if (fill.side > 1 || !fill.token_id[0]) return;
        bool inserted = false;
        PositionRecord* position = positions_.insert(fill.token_id, inserted);
        if (!position) return;
        if (fill.side == 0) {
            position->shares += fill.size_raw;  // BUY grows inventory
        } else if (position->shares >= fill.size_raw) {
            position->shares -= fill.size_raw;  // SELL shrinks inventory
        } else {
            position->shares = 0;
            record_state_event(fill.last_seen_wall_ns, "sell_exceeded_inventory");
        }
        position->observed_wall_ns = fill.last_seen_wall_ns;
        if (fill.order_id[0]) {
            OrderRecord* order = orders_.find(fill.order_id);
            if (order) release_reservation(*order, fill.size_raw);
        }
    }

    void revert_fill(const FillRecord& fill) {
        if (fill.side > 1 || !fill.token_id[0]) return;
        PositionRecord* position = positions_.find(fill.token_id);
        if (!position) return;
        if (fill.side == 0)
            position->shares = position->shares >= fill.size_raw
                                   ? position->shares - fill.size_raw : 0;
        else
            position->shares += fill.size_raw;
    }

    void apply_balance_event(const Event& event) {
        char token[K_MAX_TOKEN_ID]{};
        (void)event.get_str(F_TOKEN_ID, token, sizeof(token));
        char spender[K_MAX_ADDRESS]{};
        (void)event.get_str(F_SPENDER, spender, sizeof(spender));
        uint8_t raw_asset = 0;
        (void)event.get_u8(F_ASSET_TYPE, raw_asset);
        const auto asset_type = venue::asset_type_from_u8(raw_asset);
        char key[K_MAX_TOKEN_ID + K_MAX_ADDRESS + 8]{};
        balance_key(key, sizeof(key), token, spender, asset_type);
        bool inserted = false;
        AssetBalance* balance = balances_.insert(key, inserted);
        if (!balance) return;
        std::snprintf(balance->token_id, sizeof(balance->token_id), "%s", token);
        std::snprintf(balance->spender, sizeof(balance->spender), "%s", spender);
        balance->asset_type = static_cast<uint8_t>(asset_type);
        uint64_t value = 0;
        if (event.get_u64(F_BALANCE, value)) {
            balance->balance = value;
            balance->balance_valid = true;
        }
        if (event.get_u64(F_ALLOWANCE, value)) {
            balance->allowance = value;
            balance->allowance_valid = true;
        }
        balance->observed_wall_ns = event.wall_ns;
        // Outcome-token balances are the authoritative position source.
        if (asset_type != venue::AssetType::COLLATERAL && token[0] &&
            balance->balance_valid && event.type == EventType::BALANCE_SNAPSHOT) {
            bool position_inserted = false;
            PositionRecord* position = positions_.insert(token, position_inserted);
            if (position) {
                position->shares = balance->balance;
                position->observed_wall_ns = event.wall_ns;
            }
        }
    }

    void apply_position_event(const Event& event) {
        char token[K_MAX_TOKEN_ID]{};
        if (!event.get_str(F_TOKEN_ID, token, sizeof(token))) return;
        bool inserted = false;
        PositionRecord* position = positions_.insert(token, inserted);
        if (!position) return;
        uint64_t value = 0;
        if (event.get_u64(F_SIZE_RAW, value)) position->shares = value;
        if (event.get_u64(F_MATCHED_RAW, value)) position->reserved_shares = value;
        position->observed_wall_ns = event.wall_ns;
    }

    void apply_reservation_event(const Event& event) {
        char order_id[K_MAX_ORDER_ID]{};
        if (!event.get_str(F_ORDER_ID, order_id, sizeof(order_id))) return;
        OrderRecord* order = orders_.find(order_id);
        if (!order) return;
        uint64_t amount = 0;
        (void)event.get_u64(F_RESERVE_AMOUNT, amount);
        if (event.type == EventType::RISK_RESERVE) {
            if (!order->reservation_active && order->side == 1 && order->token_id[0]) {
                PositionRecord* position = positions_.find(order->token_id);
                if (position) position->reserved_shares += order->original_size;
            }
            order->reserved_amount = amount;
            order->reservation_active = amount != 0;
        } else {
            release_reservation(*order);
        }
    }

    void release_reservation(OrderRecord& order, uint64_t filled_size = 0) {
        if (!order.reservation_active) return;
        if (filled_size && order.original_size && filled_size < order.original_size &&
            !order_state_is_terminal(order.state)) {
            // Partial fill: shrink the reservation to what is still working.
            const uint64_t remaining = order.original_size - filled_size;
            const uint64_t per_share = order.reserved_amount / order.original_size;
            order.reserved_amount = per_share * remaining;
            if (order.reserved_amount == 0) order.reservation_active = false;
            return;
        }
        order.reserved_amount = 0;
        order.reservation_active = false;
        if (order.side == 1 && order.token_id[0]) {
            PositionRecord* position = positions_.find(order.token_id);
            if (position)
                position->reserved_shares =
                    position->reserved_shares >= order.original_size
                        ? position->reserved_shares - order.original_size : 0;
        }
    }

    void apply_heartbeat_event(const Event& event) {
        uint8_t result = 0;
        (void)event.get_u8(F_RESULT, result);
        heartbeat_.last_attempt_wall_ns = event.wall_ns;
        char id[K_MAX_HEARTBEAT_ID]{};
        if (event.get_str(F_HEARTBEAT_ID, id, sizeof(id)))
            std::snprintf(heartbeat_.heartbeat_id, sizeof(heartbeat_.heartbeat_id),
                          "%s", id);
        if (result == 1) {  // accepted
            heartbeat_.last_accepted_wall_ns = event.wall_ns;
            heartbeat_.consecutive_failures = 0;
            heartbeat_.chain_active = true;
            heartbeat_.invalidated = false;
        } else if (result == 2) {  // rejected: venue supplied a different id
            ++heartbeat_.consecutive_failures;
            heartbeat_.invalidated = true;
        } else {  // transport failure / unknown
            ++heartbeat_.consecutive_failures;
        }
    }

    void apply_reconciliation_event(const Event& event) {
        run_index_ = run_count_ ? (run_index_ + 1) % K_MAX_RUNS : 0;
        ReconciliationRun& run = runs_[run_index_];
        secure_zero(&run, sizeof(run));
        uint64_t value = 0;
        if (event.get_u64(F_RUN_ID, value)) run.run_id = value;
        if (event.get_u64(F_VENUE_TS_MS, value)) run.started_wall_ns = value * 1000000ULL;
        run.finished_wall_ns = event.wall_ns;
        uint8_t ready = 0;
        if (event.get_u8(F_RESULT, ready)) run.ready = ready;
        char reason[K_MAX_REASON]{};
        if (event.get_str(F_REASON, reason, sizeof(reason))) {
            size_t count = 0;
            const char* cursor = reason;
            while (*cursor && count < 6) {
                const char* separator = std::strchr(cursor, ';');
                const size_t length = separator
                    ? static_cast<size_t>(separator - cursor) : std::strlen(cursor);
                if (length) {
                    const size_t copy = std::min(length, sizeof(run.reasons[0]) - 1);
                    std::memcpy(run.reasons[count], cursor, copy);
                    run.reasons[count][copy] = '\0';
                    ++count;
                }
                if (!separator) break;
                cursor = separator + 1;
            }
            run.reason_count = static_cast<uint8_t>(count);
        }
        if (run_count_ < K_MAX_RUNS) ++run_count_;
    }

    void apply_metadata_event(const Event& event) {
        char condition[K_MAX_CONDITION_ID]{};
        if (event.get_str(F_CONDITION_ID, condition, sizeof(condition)))
            std::snprintf(metadata_.condition_id, sizeof(metadata_.condition_id),
                          "%s", condition);
        char token[K_MAX_TOKEN_ID]{};
        if (event.get_str(F_TOKEN_ID, token, sizeof(token)))
            std::snprintf(metadata_.token_id, sizeof(metadata_.token_id), "%s", token);
        uint64_t value = 0;
        if (event.get_u64(F_TICK_RAW, value)) metadata_.tick_raw = value;
        if (event.get_u64(F_MIN_SIZE_RAW, value)) metadata_.min_size_raw = value;
        if (event.get_u64(F_FEE_RATE_MICRO, value)) metadata_.fee_rate_micro = value;
        uint8_t flag = 0;
        if (event.get_u8(F_NEG_RISK, flag)) metadata_.neg_risk = flag != 0;
        if (event.get_u8(F_ACCEPTING_ORDERS, flag))
            metadata_.accepting_orders = flag != 0;
        metadata_.observed_wall_ns = event.wall_ns;
    }

    void apply_illegal_transition(const Event& event) {
        ++illegal_transitions_;
        char order_id[K_MAX_ORDER_ID]{};
        if (event.get_str(F_ORDER_ID, order_id, sizeof(order_id))) {
            OrderRecord* order = orders_.find(order_id);
            if (order) ++order->illegal_transitions;
        }
        char reason[K_MAX_REASON]{};
        if (event.get_str(F_REASON, reason, sizeof(reason)))
            record_state_event(event.wall_ns, reason);
    }

    void record_state_event(uint64_t wall_ns, const char* reason) {
        if (state_event_index_ >= K_STATE_EVENTS) state_event_index_ = 0;
        StateEvent& slot = state_events_ring_[state_event_index_++];
        slot.wall_ns = wall_ns;
        std::snprintf(slot.reason, sizeof(slot.reason), "%s", reason);
        if (state_event_count_ < K_STATE_EVENTS) ++state_event_count_;
    }

    // ── Checkpoint (de)serialization ────────────────────────────────────────
    static void put_u64(uint8_t* out, uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i)
            out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFULL);
    }

    static uint64_t get_u64(const uint8_t* in) noexcept {
        uint64_t value = 0;
        for (int i = 7; i >= 0; --i) value = (value << 8) | in[i];
        return value;
    }

    bool serialize_checkpoint(uint8_t* buffer, size_t cap, size_t& out_len) const {
        size_t offset = K_CK_DATA;
        if (cap < offset + 32) return false;
        put_u64(buffer + K_CK_MAGIC, K_CHECKPOINT_MAGIC);
        put_u64(buffer + K_CK_VERSION, K_FORMAT_VERSION);
        put_u64(buffer + K_CK_APPLIED, applied_events_);
        put_u64(buffer + K_CK_LAST_EVENT, last_event_wall_ns_);
        // Stored as the generation this image represents (the in-memory counter
        // is incremented after the image is durable).
        put_u64(buffer + K_CK_CHECKPOINTS, checkpoints_ + 1);

        const size_t orders_offset = offset;
        if (!serialize_map(buffer, cap, offset, orders_)) return false;
        const size_t fills_offset = offset;
        if (!serialize_map(buffer, cap, offset, fills_)) return false;
        const size_t positions_offset = offset;
        if (!serialize_map(buffer, cap, offset, positions_)) return false;
        const size_t balances_offset = offset;
        if (!serialize_map(buffer, cap, offset, balances_)) return false;

        if (offset + 8 + K_DEDUP_SLOTS * 32 > cap) return false;
        put_u64(buffer + offset, dedup_.size());
        offset += 8;
        const uint8_t (*keys)[32] = dedup_.raw();
        for (size_t i = 0; i < K_DEDUP_SLOTS; ++i) {
            bool empty = true;
            for (int b = 0; b < 32; ++b) empty &= keys[i][b] == 0;
            if (empty) continue;
            if (offset + 32 > cap) return false;
            std::memcpy(buffer + offset, keys[i], 32);
            offset += 32;
        }
        if (offset + sizeof(HeartbeatState) + sizeof(MetadataRecord) + 16 +
                sizeof(runs_) + 32 > cap)
            return false;
        std::memcpy(buffer + offset, &heartbeat_, sizeof(heartbeat_));
        offset += sizeof(heartbeat_);
        std::memcpy(buffer + offset, &metadata_, sizeof(metadata_));
        offset += sizeof(metadata_);
        put_u64(buffer + offset, run_count_); offset += 8;
        put_u64(buffer + offset, run_index_); offset += 8;
        std::memcpy(buffer + offset, runs_, sizeof(runs_));
        offset += sizeof(runs_);

        put_u64(buffer + K_CK_ORDERS_OFFSET, orders_offset);
        put_u64(buffer + K_CK_FILLS_OFFSET, fills_offset);
        put_u64(buffer + K_CK_POSITIONS_OFFSET, positions_offset);
        put_u64(buffer + K_CK_BALANCES_OFFSET, balances_offset);
        put_u64(buffer + K_CK_BODY_LEN, offset);

        uint8_t digest[32];
        sha256(buffer, offset, digest);
        std::memcpy(buffer + offset, digest, 32);
        offset += 32;
        out_len = offset;
        return true;
    }

    template <typename Map>
    static bool serialize_map(uint8_t* buffer, size_t cap, size_t& offset,
                              const Map& map) {
        if (offset + 8 > cap) return false;
        put_u64(buffer + offset, map.size());
        offset += 8;
        for (size_t i = 0; i < Map::kCapacity; ++i) {
            const typename Map::RecordType* record = map.at(i);
            if (!record) return false;
            if (Map::key_of(*record)[0] == '\0') continue;
            if (offset + sizeof(*record) > cap) return false;
            std::memcpy(buffer + offset, record, sizeof(*record));
            offset += sizeof(*record);
        }
        return true;
    }

    bool deserialize_checkpoint(const uint8_t* buffer, size_t length) {
        if (length < K_CK_DATA + 32) return false;
        if (get_u64(buffer + K_CK_MAGIC) != K_CHECKPOINT_MAGIC) return false;
        if (get_u64(buffer + K_CK_VERSION) != K_FORMAT_VERSION) return false;
        const size_t body_len = static_cast<size_t>(get_u64(buffer + K_CK_BODY_LEN));
        if (body_len < K_CK_DATA || body_len + 32 != length) return false;
        uint8_t digest[32];
        sha256(buffer, body_len, digest);
        if (std::memcmp(digest, buffer + body_len, 32) != 0) return false;

        size_t offset = K_CK_DATA;
        if (static_cast<size_t>(get_u64(buffer + K_CK_ORDERS_OFFSET)) != offset)
            return false;
        if (!deserialize_map(buffer, body_len, offset, orders_)) return false;
        if (static_cast<size_t>(get_u64(buffer + K_CK_FILLS_OFFSET)) != offset)
            return false;
        if (!deserialize_map(buffer, body_len, offset, fills_)) return false;
        if (static_cast<size_t>(get_u64(buffer + K_CK_POSITIONS_OFFSET)) != offset)
            return false;
        if (!deserialize_map(buffer, body_len, offset, positions_)) return false;
        if (static_cast<size_t>(get_u64(buffer + K_CK_BALANCES_OFFSET)) != offset)
            return false;
        if (!deserialize_map(buffer, body_len, offset, balances_)) return false;

        if (offset + 8 > body_len) return false;
        const uint64_t key_count = get_u64(buffer + offset);
        offset += 8;
        if (key_count > K_DEDUP_SLOTS) return false;
        dedup_.clear();
        for (uint64_t i = 0; i < key_count; ++i) {
            if (offset + 32 > body_len) return false;
            dedup_.add(buffer + offset);
            offset += 32;
        }
        if (offset + sizeof(HeartbeatState) + sizeof(MetadataRecord) + 16 +
                sizeof(runs_) > body_len)
            return false;
        std::memcpy(&heartbeat_, buffer + offset, sizeof(heartbeat_));
        offset += sizeof(heartbeat_);
        std::memcpy(&metadata_, buffer + offset, sizeof(metadata_));
        offset += sizeof(metadata_);
        run_count_ = static_cast<size_t>(get_u64(buffer + offset)); offset += 8;
        run_index_ = static_cast<size_t>(get_u64(buffer + offset)); offset += 8;
        if (run_count_ > K_MAX_RUNS || run_index_ >= K_MAX_RUNS) return false;
        std::memcpy(runs_, buffer + offset, sizeof(runs_));
        offset += sizeof(runs_);
        if (offset != body_len) return false;
        applied_events_ = get_u64(buffer + K_CK_APPLIED);
        last_event_wall_ns_ = get_u64(buffer + K_CK_LAST_EVENT);
        checkpoints_ = get_u64(buffer + K_CK_CHECKPOINTS);
        return true;
    }

    template <typename Map>
    static bool deserialize_map(const uint8_t* buffer, size_t limit, size_t& offset,
                                Map& map) {
        using Record = typename Map::RecordType;
        if (offset + 8 > limit) return false;
        const uint64_t count = get_u64(buffer + offset);
        offset += 8;
        if (count > Map::kCapacity) return false;
        map.clear();
        for (uint64_t i = 0; i < count; ++i) {
            if (offset + sizeof(Record) > limit) return false;
            Record record{};
            std::memcpy(&record, buffer + offset, sizeof(record));
            offset += sizeof(record);
            const char* key = Map::key_of(record);
            if (key[0] == '\0') return false;
            bool inserted = false;
            Record* slot = map.insert(key, inserted);
            if (!slot || !inserted) return false;
            std::memcpy(slot, &record, sizeof(record));
        }
        return true;
    }

    mutable std::mutex mutex_;
    Options options_{};
    char journal_path_[320]{};
    char checkpoint_path_[320]{};
    int journal_fd_ = -1;
    bool opened_ = false;
    bool corrupt_ = false;
    bool recovered_trailing_ = false;
    uint64_t journal_bytes_ = 0;
    uint64_t applied_events_ = 0;
    uint64_t replayed_events_ = 0;
    uint64_t duplicates_ignored_ = 0;
    uint64_t illegal_transitions_ = 0;
    uint64_t generic_state_events_ = 0;
    uint64_t events_since_checkpoint_ = 0;
    uint64_t checkpoints_ = 0;
    uint64_t last_event_wall_ns_ = 0;

    OrderMap orders_{};
    FillMap fills_{};
    PositionMap positions_{};
    BalanceMap balances_{};
    KeyRing<K_DEDUP_SLOTS> dedup_{};
    HeartbeatState heartbeat_{};
    MetadataRecord metadata_{};
    ReconciliationRun runs_[K_MAX_RUNS]{};
    size_t run_count_ = 0;
    size_t run_index_ = 0;
    StateEvent state_events_ring_[K_STATE_EVENTS]{};
    size_t state_event_index_ = 0;
    size_t state_event_count_ = 0;
};

}  // namespace ledger

#endif  // EVENT_LEDGER_HPP
