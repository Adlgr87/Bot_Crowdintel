// Bot CrowdIntel — live topology:
//   market WSS  -> atomic top/depth book
//   alpha HTTP  -> SPSC -> execution engine (risk, freshness, dedupe)
//   presigner   -> consumable ladder
//   execution   -> order recorder (durable ticket) -> async gateway -> CLOB
//   user WSS    -> idempotent event application -> ledger
//   heartbeat   -> cancel-on-disconnect contract + watchdog
//   supervisor  -> metadata refresh, reconciliation, readiness gate
//
// Trading is disabled until every gate has passed, and is disabled again the
// moment any of them breaks.  See docs/CANARY_CHECKLIST.md.

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include "../crypto/eip712_signer.hpp"
#include "../crypto/secure_zero.hpp"
#include "../include/event_ledger.hpp"
#include "../include/order_book.hpp"
#include "../include/order_state.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../include/venue_metadata.hpp"
#include "alpha_parser.hpp"
#include "execution_engine.hpp"
#include "kill_switch.hpp"
#include "ledger_order_observer.hpp"
#include "market_config.hpp"
#include "metadata_pipeline.hpp"
#include "order_recorder.hpp"
#include "preflight.hpp"
#include "presigned_pool.hpp"
#include "reconciliation.hpp"
#include "session_report.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK) && !defined(CROWDINTEL_FORCE_MOCK)
#include "alpha_http_receiver.hpp"
#include "curl_transport.hpp"
#include "lightweight_client.hpp"
#include "order_gateway.hpp"
#include "rpc_client.hpp"
#include "user_ws_client.hpp"
#include "ws_market_listener.hpp"
#define CROWDINTEL_LIVE 1
#else
#include "mock_client.hpp"
#endif

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

namespace {

void idle_wait() {
#if defined(__x86_64__) || defined(__i386__)
    for (int i = 0; i < 256; ++i) __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
    timespec delay{0, 50000};
    nanosleep(&delay, nullptr);
}

uint64_t now_ms_or_zero() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void pin_to_cpu(int cpu) {
#if defined(__linux__)
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        std::fprintf(stderr, "warning: could not pin thread to cpu %d\n", cpu);
#else
    (void)cpu;
#endif
}

#ifdef CROWDINTEL_LIVE
uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
#endif

#if !defined(CROWDINTEL_LIVE)
void mock_feed(SPSC_RingBuffer<AlphaSignal>& queue, OrderBookL2& book,
               const MarketConfig& cfg, std::atomic<bool>& running) {
    const Level2Entry bids[3] = {
        {470000, 40000000}, {460000, 60000000}, {450000, 90000000}};
    const Level2Entry asks[3] = {
        {530000, 30000000}, {540000, 55000000}, {550000, 80000000}};
    book.set_tick_size(cfg.tick_size);
    book.set_book(bids, 3, asks, 3);
    uint64_t id = 1;
    while (running.load(std::memory_order_acquire)) {
        AlphaSignal signal{};
        signal.type = AlphaSignal::Type::WHALE_TRADE;
        signal.direction_hint = 0;  // explicitly exercise BUY-hint semantics
        signal.p_win = (id % 2) ? 0.65 : 0.50;
        signal.confidence = 0.92;
        signal.q_value = 0.01;
        signal.timestamp_ns = AlphaParser::realtime_ns();
        signal.market_hash = cfg.market_hash;
        signal.signal_id = id++;
        (void)queue.try_push(signal);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
#endif

// Paper-mode egress: the order is built, signed and durably recorded, then
// refused.  No venue traffic, no artificial fills, no assumed balance.
class PaperEgressClient {
public:
    explicit PaperEgressClient(const MarketConfig&) {}
    SubmitResult submit(const WireBody&) {
        submissions_.fetch_add(1, std::memory_order_relaxed);
        SubmitResult result{};
        result.ok = false;
        result.final = true;
        result.retryable = false;
        result.ambiguous = false;
        std::snprintf(result.error, sizeof(result.error),
                      "paper mode: order egress is disabled");
        return result;
    }
    uint64_t submissions() const {
        return submissions_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> submissions_{0};
};

template <typename Engine>
long run_engine(Engine& engine, const MarketConfig& cfg,
                uint64_t* counts, size_t count_size) {
    long productive = 0;
    const uint64_t session_start = now_ms_or_zero();
    while (g_running.load(std::memory_order_acquire)) {
        if (cfg.session_timeout_ms &&
            now_ms_or_zero() - session_start >= cfg.session_timeout_ms) {
            std::fprintf(stdout, "session timeout reached (%llu ms)\n",
                         static_cast<unsigned long long>(cfg.session_timeout_ms));
            break;
        }
        const TickResult result = engine.run_tick();
        const size_t index = static_cast<size_t>(result);
        if (index < count_size) ++counts[index];
        if (result == TickResult::NO_SIGNAL) idle_wait();
        else ++productive;
        if (cfg.max_ticks > 0 && productive >= cfg.max_ticks) break;
    }
    return productive;
}

#ifdef CROWDINTEL_LIVE
// Live variant: also parses the POST /order response body kept by the client.
class LiveOrderObserver : public egress::LedgerOrderObserver {
public:
    LiveOrderObserver(ledger::EventLedger* ledger, const MarketConfig& config,
                      std::atomic<bool>& trading_enabled,
                      LightweightCLOBClient& client)
        : egress::LedgerOrderObserver(ledger, config, trading_enabled),
          client_(client) {}

    void on_after_egress(const WireBody& body, const SubmitResult& result) override {
        if (result.order_id[0] && !client_.last_response_truncated()) {
            clob::OrderPostResult detail{};
            if (clob::parse_order_post(client_.last_response_body(),
                                       client_.last_response_len(), detail)) {
                last_detail_ = detail;
                has_detail_ = true;
            }
        }
        egress::LedgerOrderObserver::on_after_egress(body, result);
        has_detail_ = false;
    }

    const clob::OrderPostResult* pending_detail() const noexcept override {
        return has_detail_ ? &last_detail_ : nullptr;
    }

private:
    LightweightCLOBClient& client_;
    clob::OrderPostResult last_detail_{};
    bool has_detail_ = false;
};
#endif

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);  // OpenSSL can ultimately write(2) a dead peer
#endif

    const char* mode_env = std::getenv("BOT_MODE");
    const bool live_requested = mode_env && std::strcmp(mode_env, "live") == 0;
    if (mode_env && std::strcmp(mode_env, "replay") == 0) {
        std::fprintf(stderr,
                     "FATAL: BOT_MODE=replay belongs to the l2_backtester binary "
                     "(tests/replay/l2_backtester.cpp); this process runs paper or "
                     "live only\n");
        return 2;
    }
#if !defined(CROWDINTEL_LIVE)
    if (live_requested) {
        std::fprintf(stderr,
                     "FATAL: this build has no network transport; live mode is "
                     "impossible (rebuild with -DCROWDINTEL_NETWORK=ON)\n");
        return 2;
    }
#endif

#ifdef CROWDINTEL_LIVE
    constexpr bool kForceMock = false;
#else
    constexpr bool kForceMock = true;
#endif

    MarketConfig cfg;
    if (const char* error = cfg.load(live_requested, kForceMock)) {
        std::fprintf(stderr, "FATAL: %s\n", error);
        return 1;
    }
    const bool live = cfg.bot_mode == BotMode::LIVE;
    if (live && !cfg.token_id_dec[0] && !cfg.market_slug[0] && !cfg.condition_id[0]) {
        std::fprintf(stderr,
                     "FATAL: live mode needs BOT_TOKEN_ID, or BOT_MARKET_SLUG / "
                     "BOT_CONDITION_ID to resolve it from venue metadata\n");
        return 1;
    }

    // ── Persistent ledger (Phase 2) ─────────────────────────────────────────
    auto ledger = std::make_unique<ledger::EventLedger>();
    ledger::EventLedger::Options ledger_options{};
    std::snprintf(ledger_options.directory, sizeof(ledger_options.directory), "%s",
                  cfg.ledger_dir);
    ledger_options.fsync_each_append = cfg.ledger_fsync;
    ledger_options.checkpoint_every = cfg.ledger_checkpoint_every;
    char ledger_error[192]{};
    bool ledger_available = ledger->open(ledger_options, ledger_error,
                                         sizeof(ledger_error));
    if (!ledger_available) {
        if (live) {
            std::fprintf(stderr,
                         "FATAL: ledger at %s is unusable (%s); refusing to trade on "
                         "unknown state\n",
                         cfg.ledger_dir, ledger_error);
            return 1;
        }
        std::fprintf(stderr,
                     "WARNING: ledger unavailable (%s); continuing without durable "
                     "state (paper/offline only)\n",
                     ledger_error);
        ledger.reset();
    } else if (ledger->recovered_trailing_record()) {
        std::fprintf(stdout,
                     "ledger: discarded one incomplete trailing journal record "
                     "(crash during append)\n");
    }

    // ── Venue metadata (Phase 1) ────────────────────────────────────────────
    venue::MarketRuntime runtime;
    venue::PipelineResult metadata_result{};
    bool metadata_ok = false;
#ifdef CROWDINTEL_LIVE
    auto transport = std::make_unique<clob::CurlTransport>();
    transport->set_tls_pin(cfg.tls_pin);
    if (!transport->usable()) {
        std::fprintf(stderr, "FATAL: curl initialisation failed\n");
        return 1;
    }
    clob::Credentials credentials{};
    std::snprintf(credentials.address, sizeof(credentials.address), "%s",
                  cfg.api_address_hex[0] ? cfg.api_address_hex : cfg.signer_hex);
    std::snprintf(credentials.api_key, sizeof(credentials.api_key), "%s",
                  cfg.owner_api_key);
    std::snprintf(credentials.api_secret_b64, sizeof(credentials.api_secret_b64),
                  "%s", cfg.api_secret_b64);
    std::snprintf(credentials.api_passphrase, sizeof(credentials.api_passphrase),
                  "%s", cfg.api_passphrase);
    credentials.signature_type = cfg.signature_type;
    auto api_client = std::make_unique<clob::ClobApiClient>(
        *transport, credentials, cfg.clob_host, cfg.gamma_host);
    rpc::JsonRpcClient rpc_client(*transport, cfg.polygon_rpc_url,
                                  cfg.polygon_rpc_backup_url);
    transport->warmup(cfg.clob_host);

    if (live) {
        // Every contract address and chain-scoped constant in this binary is
        // only trusted after the RPC confirms we are on Polygon mainnet.
        clob::CallResult chain_call{};
        uint64_t chain_id = 0;
        if (!rpc_client.chain_id(chain_id, chain_call)) {
            std::fprintf(stderr,
                         "FATAL: cannot verify the chain id over POLYGON_RPC_URL "
                         "(%s); refusing to trade with unverified contract "
                         "constants\n",
                         chain_call.detail);
            return 1;
        }
        if (chain_id != venue::K_POLYGON_CHAIN_ID) {
            std::fprintf(stderr,
                         "FATAL: RPC reports chain id %llu, expected %llu (Polygon "
                         "mainnet)%s\n",
                         static_cast<unsigned long long>(chain_id),
                         static_cast<unsigned long long>(venue::K_POLYGON_CHAIN_ID),
                         chain_id == venue::K_AMOY_CHAIN_ID
                             ? "; that is the Amoy testnet, whose contract addresses "
                               "are different - POLYGON_RPC_URL points at the wrong "
                               "network"
                             : "");
            return 1;
        }
        std::fprintf(stdout, "rpc: chain id %llu verified%s\n",
                     static_cast<unsigned long long>(chain_id),
                     rpc_client.used_backup() ? " (via backup URL)" : "");
    }

    {
        venue::PipelineOptions options{};
        options.require_status = live;
        venue::MetadataPipeline pipeline(*api_client, options);
        venue::MetadataPolicy policy{};
        policy.require_gamma_status = live;
        policy.allow_protocol_v2 = cfg.allow_protocol_v2_positions;
        policy.max_taker_fee_micro =
            static_cast<uint64_t>(cfg.max_taker_fee_rate * 1000000.0);
        metadata_ok = pipeline.run(cfg.market_slug, cfg.condition_id,
                                   cfg.token_id_dec[0] ? cfg.token_id_dec : nullptr,
                                   policy, metadata_result);
        if (metadata_ok) {
            venue::MetadataReport report{};
            metadata_ok = runtime.publish(metadata_result.metadata, policy, report);
            if (!metadata_ok) {
                char reasons[256]{};
                report.format(reasons, sizeof(reasons));
                std::fprintf(stderr, "FATAL: metadata validation failed: %s\n",
                             reasons);
            }
        } else {
            char reasons[256]{};
            metadata_result.report.format(reasons, sizeof(reasons));
            std::fprintf(stderr, "metadata pipeline: %s %s\n", metadata_result.detail,
                         reasons);
        }
    }
    if (!metadata_ok && live) {
        std::fprintf(stderr,
                     "FATAL: live mode refuses to start without validated venue "
                     "metadata\n");
        return 1;
    }
    if (metadata_ok) {
        // Runtime configuration now comes from the venue, not from the
        // environment.  The environment values were used as expectations and
        // have already been cross-checked.
        const venue::MarketMetadata& metadata = metadata_result.metadata;
        cfg.tick_size = metadata.tick_raw;
        cfg.min_size_shares = metadata.min_size_raw;
        cfg.neg_risk = metadata.neg_risk;
        if (metadata.fees_enabled && metadata.fee_rate_micro) {
            const double venue_fee =
                static_cast<double>(metadata.fee_rate_micro) * 1e-6;
            // The venue's own rate is what will actually be charged, so the risk
            // model uses it.  The dangerous direction - a venue rate above what
            // the operator accepted - never reaches this line: validate_metadata
            // fails the whole snapshot with "taker_fee_above_policy" when
            // fee_rate_micro > policy.max_taker_fee_micro (BOT_MAX_TAKER_FEE_RATE),
            // and main refuses to start (or disables egress on a refresh) instead.
            // When the venue reports no fee schedule at all, the configured
            // assumption is kept, which overestimates and is therefore the
            // conservative side for sizing.
            cfg.taker_fee_rate = venue_fee;
        }
        std::snprintf(cfg.condition_id, sizeof(cfg.condition_id), "%s",
                      metadata.condition_id);
        if (!cfg.token_id_dec[0]) {
            std::snprintf(cfg.token_id_dec, sizeof(cfg.token_id_dec), "%s",
                          metadata_result.resolved_token_id);
            uint64_t limbs[4] = {0, 0, 0, 0};
            if (!venue::parse_uint256_limbs(cfg.token_id_dec,
                                            std::strlen(cfg.token_id_dec), limbs)) {
                std::fprintf(stderr, "FATAL: resolved token id is not a uint256\n");
                return 1;
            }
            for (int word = 0; word < 4; ++word)
                for (int byte = 0; byte < 8; ++byte)
                    cfg.token_id_be[word * 8 + byte] = static_cast<uint8_t>(
                        (limbs[3 - word] >> (8 * (7 - byte))) & 0xFFULL);
        }
        cfg.market_hash = alpha_hash_bytes(cfg.market_slug, std::strlen(cfg.market_slug));
        // The token identity is printed with its provenance: an operator reading
        // the startup log must be able to tell a configured token id from one the
        // venue resolved for us (config.token_id_from_metadata records exactly
        // that, and it is decided at load time, before any resolution).
        std::fprintf(stdout,
                     "metadata: condition=%s token=%s (source=%s) tick=%.4f "
                     "min_size=%.2f neg_risk=%d fee=%.4f book=%zu/%zu\n",
                     cfg.condition_id, cfg.token_id_dec, cfg.token_id_source(),
                     static_cast<double>(cfg.tick_size) * 1e-6,
                     static_cast<double>(cfg.min_size_shares) * 1e-6,
                     cfg.neg_risk ? 1 : 0, cfg.taker_fee_rate,
                     metadata_result.book.bid_count, metadata_result.book.ask_count);
        if (ledger) {
            auto lock = ledger->guard();
            ledger::Event event{};
            event.type = ledger::EventType::METADATA_SNAPSHOT;
            event.source = ledger::Source::REST;
            event.wall_ns = ledger::now_wall_ns();
            event.add_str(ledger::F_CONDITION_ID, cfg.condition_id);
            event.add_str(ledger::F_TOKEN_ID, cfg.token_id_dec);
            event.add_u64(ledger::F_TICK_RAW, cfg.tick_size);
            event.add_u64(ledger::F_MIN_SIZE_RAW, cfg.min_size_shares);
            event.add_u64(ledger::F_FEE_RATE_MICRO, metadata.fee_rate_micro);
            event.add_u8(ledger::F_NEG_RISK, cfg.neg_risk ? 1 : 0);
            event.add_u8(ledger::F_ACCEPTING_ORDERS,
                         metadata.accepting_orders ? 1 : 0);
            ledger::compute_event_key(event.type, event.source, "", "", 0,
                                      metadata.fee_rate_micro, event.key);
            char error[128]{};
            if (!ledger->commit_locked(event, error, sizeof(error)))
                std::fprintf(stderr, "LEDGER: metadata snapshot failed: %s\n", error);
        }
    }
#endif

    // Parse the private key exactly once, initialize the only signer (with the
    // venue-confirmed negative-risk domain), then wipe the environment copy.
    uint8_t private_key[32];
    if (!parse_hex_bytes(cfg.private_key_hex, std::strlen(cfg.private_key_hex),
                         private_key, sizeof(private_key))) {
        std::fprintf(stderr, "FATAL: invalid BOT_PRIVATE_KEY_HEX\n");
        return 1;
    }
    EIP712Signer signer;
    if (!signer.init(private_key, cfg.neg_risk)) {
        secure_zero(private_key, sizeof(private_key));
        std::fprintf(stderr, "FATAL: signer initialization failed\n");
        return 1;
    }
    secure_zero(private_key, sizeof(private_key));
    secure_zero(cfg.private_key_hex, sizeof(cfg.private_key_hex));
    if (const char* error = cfg.finalize_identity(signer.signer_address())) {
        std::fprintf(stderr, "FATAL: %s\n", error);
        return 1;
    }

#if defined(__linux__)
    if (live && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr,
                     "FATAL: mlockall failed; refusing live mode with swappable secrets\n");
        return 1;
    }
#endif

    std::fprintf(stdout,
        "CrowdIntel 3.0 mode=%s market=%s tick=%.4f sig=%u order=%s ledger=%s\n",
        bot_mode_name(cfg.bot_mode), cfg.market_slug,
        static_cast<double>(cfg.tick_size) * 1e-6,
        cfg.signature_type, cfg.order_type, ledger ? cfg.ledger_dir : "disabled");
    std::fprintf(stdout, "config_fingerprint=%s\n", cfg.config_fingerprint);
    // Public addresses (not secrets): they let the operator confirm at a glance
    // that this process signs with the wallet crowdintel-preflight validated.
    std::fprintf(stdout, "wallet=%s maker=%s api_address=%s\n", cfg.signer_hex,
                 cfg.maker_hex, cfg.api_address_hex);

    // ── Preflight gate (Phase 6/7) ──────────────────────────────────────────
    if (live && cfg.live_armed) {
        char detail[192]{};
        if (!preflight::Runner::token_is_fresh(cfg.preflight_token_file,
                                               cfg.preflight_max_age_s,
                                               cfg.config_fingerprint, detail,
                                               sizeof(detail))) {
            std::fprintf(stderr,
                         "FATAL: BOT_ENABLE_LIVE_TRADING=1 requires a fresh "
                         "crowdintel-preflight pass for this exact configuration: %s\n",
                         detail);
            return 1;
        }
        std::fprintf(stdout, "preflight: %s\n", detail);
    }

    auto book = std::make_unique<OrderBookL2>();
    auto signals = std::make_unique<SPSC_RingBuffer<AlphaSignal>>();
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    std::atomic<bool> workers_running{true};
    // Fail closed: trading is enabled only after every gate has passed.
    std::atomic<bool> trading_enabled{false};
    std::atomic<uint64_t> reconciled_inventory{0};
    std::atomic<bool> readiness_ready{false};

    if (metadata_ok && metadata_result.book_valid) {
        book->set_tick_size(cfg.tick_size);
        Level2Entry bids[venue::BookSnapshot::K_MAX_LEVELS];
        Level2Entry asks[venue::BookSnapshot::K_MAX_LEVELS];
        for (size_t i = 0; i < metadata_result.book.bid_count; ++i)
            bids[i] = {metadata_result.book.bid_price[i],
                       metadata_result.book.bid_size[i]};
        for (size_t i = 0; i < metadata_result.book.ask_count; ++i)
            asks[i] = {metadata_result.book.ask_price[i],
                       metadata_result.book.ask_size[i]};
        book->set_book(bids, metadata_result.book.bid_count, asks,
                       metadata_result.book.ask_count);
    }

    // market_config.hpp validates BOT_HEARTBEAT_BLOCK_MS and
    // BOT_HEARTBEAT_ASSUME_CANCELLED_MS against a literal 10000 ms; this ties that
    // literal to the venue constant it comes from, so changing one without the
    // other stops the build instead of silently weakening the margin.
    static_assert(heartbeat::K_VENUE_TIMEOUT_MS == 10000,
                  "market_config.hpp caps the heartbeat thresholds at this value");
    static_assert(heartbeat::K_VENUE_WORST_CASE_MS ==
                      heartbeat::K_VENUE_TIMEOUT_MS + heartbeat::K_VENUE_CHECK_MS,
                  "the worst-case cancellation horizon is timeout + check cadence");

    egress::LedgerOrderObserver observer(ledger.get(), cfg, trading_enabled);

    // ── Kill switch: monitored in every mode ───────────────────────────────
    // The operator's emergency stop must not depend on which mode the binary
    // happens to run in, and a check that cannot be answered blocks (see
    // core/src/kill_switch.hpp).
    std::thread kill_switch_thread([&] {
        pin_to_cpu(cfg.cold_cpu);
        while (workers_running.load(std::memory_order_acquire)) {
            const safety::KillSwitchState state =
                safety::poll_kill_switch(cfg.kill_switch_file);
            if (safety::kill_switch_blocks(state)) {
                trading_enabled.store(false, std::memory_order_release);
                g_running.store(false, std::memory_order_release);
                if (state == safety::KillSwitchState::ENGAGED) {
                    std::fprintf(stderr, "KILL SWITCH active: %s\n",
                                 cfg.kill_switch_file);
                } else {
                    char error[160]{};
                    safety::kill_switch_error_text(error, sizeof(error));
                    std::fprintf(stderr,
                                 "KILL SWITCH check failed closed (%s): %s\n",
                                 error, cfg.kill_switch_file);
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::thread mock_thread;
    std::thread supervisor_thread;
#ifdef CROWDINTEL_LIVE
    std::unique_ptr<PaperEgressClient> paper_client;
    std::unique_ptr<OrderGateway<PaperEgressClient>> paper_gateway;
    std::unique_ptr<WsMarketListener> market_listener;
    std::unique_ptr<AlphaParser> alpha_parser;
    std::unique_ptr<AlphaHttpReceiver> alpha_receiver;
    std::unique_ptr<LightweightCLOBClient> live_client;
    std::unique_ptr<LiveOrderObserver> live_observer;
    std::unique_ptr<OrderGateway<LightweightCLOBClient>> gateway;
    std::unique_ptr<heartbeat::OrderHeartbeat> heartbeat_monitor;
    std::unique_ptr<user_ws::UserWsClient> user_ws;
    std::unique_ptr<recon::Reconciler> reconciler;
#endif

    // ── Cold-path services ──────────────────────────────────────────────────
#ifdef CROWDINTEL_LIVE
    if (!live) {
        // Paper: real market data, no order egress, no artificial fills.
        market_listener = std::make_unique<WsMarketListener>(cfg, *book);
        market_listener->start();
        alpha_parser = std::make_unique<AlphaParser>(
            *signals, cfg.market_slug, cfg.max_q_value, cfg.min_confidence);
        alpha_receiver = std::make_unique<AlphaHttpReceiver>(cfg, *alpha_parser);
        if (!alpha_receiver->start()) {
            std::fprintf(stderr, "FATAL: cannot bind alpha receiver at %s:%u\n",
                         cfg.alpha_bind, cfg.alpha_port);
            market_listener->stop();
            return 1;
        }
        paper_client = std::make_unique<PaperEgressClient>(cfg);
        paper_gateway = std::make_unique<OrderGateway<PaperEgressClient>>(
            *paper_client, &trading_enabled, &observer);
        paper_gateway->start();
        trading_enabled.store(true, std::memory_order_release);
        std::fprintf(stdout,
                     "paper mode: market data live, order egress disabled, no "
                     "simulated fills\n");
    } else {
        // ── Heartbeat (Phase 4) — before any order can be placed ────────────
        if (cfg.heartbeat_enabled) {
            heartbeat::Config hb_config{};
            hb_config.enabled = true;
            hb_config.interval_ms = cfg.heartbeat_interval_ms;
            hb_config.warn_ms = cfg.heartbeat_warn_ms;
            hb_config.block_ms = cfg.heartbeat_block_ms;
            hb_config.assume_cancelled_ms = cfg.heartbeat_assume_cancelled_ms;
            hb_config.max_consecutive_failures = cfg.heartbeat_max_failures;
            char hb_error[192]{};
            if (!hb_config.validate(hb_error, sizeof(hb_error))) {
                std::fprintf(stderr, "FATAL: %s\n", hb_error);
                return 1;
            }
            heartbeat_monitor = std::make_unique<heartbeat::OrderHeartbeat>(
                *api_client, *ledger, hb_config);
            if (!heartbeat_monitor->start(hb_error, sizeof(hb_error))) {
                std::fprintf(stderr, "FATAL: heartbeat start failed: %s\n", hb_error);
                return 1;
            }
            // Establish the chain synchronously: if the contract cannot be
            // started, orders would rest without the protection we asked for.
            if (!heartbeat_monitor->tick(hb_error, sizeof(hb_error))) {
                std::fprintf(stderr,
                             "FATAL: first heartbeat was not acknowledged (%s); "
                             "refusing to place orders without cancel-on-disconnect\n",
                             hb_error);
                heartbeat_monitor->request_stop();
                heartbeat_monitor->join();
                return 1;
            }
            std::fprintf(stdout,
                         "heartbeat: contract active (interval %u ms, block %u ms, "
                         "assume-cancelled %u ms) — these credentials must be "
                         "dedicated to this process\n",
                         cfg.heartbeat_interval_ms, cfg.heartbeat_block_ms,
                         cfg.heartbeat_assume_cancelled_ms);
        }

        // ── User channel (Phase 3) ──────────────────────────────────────────
        if (cfg.user_ws_enabled) {
            user_ws::Config ws_config{};
            ws_config.enabled = true;
            std::snprintf(ws_config.url, sizeof(ws_config.url), "%s", cfg.user_ws_host);
            if (cfg.condition_id[0]) {
                ws_config.market_count = 1;
                std::snprintf(ws_config.markets[0], sizeof(ws_config.markets[0]),
                              "%s", cfg.condition_id);
            }
            ws_config.keepalive_interval_ms = cfg.user_ws_keepalive_ms;
            ws_config.idle_timeout_ms = cfg.user_ws_idle_ms;
            ws_config.pong_timeout_ms = cfg.user_ws_pong_ms;
            ws_config.reconnect_min_ms = cfg.user_ws_reconnect_min_ms;
            ws_config.reconnect_max_ms = cfg.user_ws_reconnect_max_ms;
            // BOT_TLS_PIN holds up to two "sha256//<base64>" pins; the session
            // pins on the first one (rotation is an operator action).
            if (const char* pin = std::strstr(cfg.tls_pin, "sha256//")) {
                const char* body = pin + 8;
                const char* end = std::strchr(body, ';');
                const size_t length = end ? static_cast<size_t>(end - body)
                                          : std::strlen(body);
                if (length > 0 && length < sizeof(ws_config.pin_spki_base64)) {
                    std::memcpy(ws_config.pin_spki_base64, body, length);
                    ws_config.pin_spki_base64[length] = '\0';
                }
            }
            user_ws::ApplierContext context{};
            std::snprintf(context.api_owner, sizeof(context.api_owner), "%s",
                          cfg.owner_api_key);
            std::snprintf(context.maker_address, sizeof(context.maker_address), "%s",
                          cfg.maker_hex);
            std::snprintf(context.condition_id, sizeof(context.condition_id), "%s",
                          cfg.condition_id);
            std::snprintf(context.token_id, sizeof(context.token_id), "%s",
                          cfg.token_id_dec);
            user_ws = std::make_unique<user_ws::UserWsClient>(ws_config, credentials,
                                                              *ledger, context);
            char ws_error[192]{};
            if (!user_ws->start(ws_error, sizeof(ws_error))) {
                std::fprintf(stderr, "FATAL: user channel start failed: %s\n",
                             ws_error);
                return 1;
            }
            // Bounded wait for the subscription before reconciling, so the
            // reconciliation sees a stream that is actually attached.
            const uint64_t deadline = now_ms() + 10000;
            while (now_ms() < deadline &&
                   user_ws->state() != user_ws::StreamState::SUBSCRIBED)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (user_ws->state() != user_ws::StreamState::SUBSCRIBED) {
                std::fprintf(stderr,
                             "FATAL: user channel did not subscribe within 10 s "
                             "(state=%s, last_error=%s)\n",
                             user_ws::stream_state_name(user_ws->state()),
                             ws::session_error_name(user_ws->last_error()));
                user_ws->stop();
                user_ws->join();
                return 1;
            }
            std::fprintf(stdout, "user channel: subscribed to %s\n",
                         cfg.condition_id[0] ? cfg.condition_id : "the whole account");
        }

        // ── Order egress ────────────────────────────────────────────────────
        live_client = std::make_unique<LightweightCLOBClient>(cfg);
        live_observer = std::make_unique<LiveOrderObserver>(
            ledger.get(), cfg, trading_enabled, *live_client);
        gateway = std::make_unique<OrderGateway<LightweightCLOBClient>>(
            *live_client, &trading_enabled, live_observer.get());
        gateway->start();

        // ── Startup reconciliation (Phase 5) ────────────────────────────────
        recon::Context recon_context{};
        std::snprintf(recon_context.condition_id, sizeof(recon_context.condition_id),
                      "%s", cfg.condition_id);
        std::snprintf(recon_context.token_id, sizeof(recon_context.token_id), "%s",
                      cfg.token_id_dec);
        uint8_t exchange_bytes[20]{};
        char exchange_hex[43]{};
        api_client->exchange_for_token(cfg.token_id_dec, cfg.neg_risk, exchange_bytes,
                                       exchange_hex);
        std::snprintf(recon_context.collateral_spender,
                      sizeof(recon_context.collateral_spender), "%s", exchange_hex);
        recon_context.neg_risk = cfg.neg_risk;
        recon_context.min_collateral = cfg.min_collateral_base;
        recon_context.target_allowance = cfg.target_allowance_base;
        recon_context.max_pages = cfg.recon_max_pages;
        recon_context.signature_type = cfg.signature_type;
        reconciler = std::make_unique<recon::Reconciler>(*api_client, *ledger,
                                                         recon_context);
        recon::Report report{};
        const bool ready = reconciler->run_startup(report, heartbeat_monitor.get());
        char reasons[256]{};
        report.format_reasons(reasons, sizeof(reasons));
        std::fprintf(stdout,
                     "reconciliation: %s venue_orders=%zu local_orders=%zu "
                     "adopted=%zu fills_missing=%zu collateral=%llu allowance=%llu "
                     "target=%llu inventory=%llu reasons=%s\n",
                     recon::readiness_name(report.readiness), report.venue_open_orders,
                     report.local_open_orders, report.adopted_orders,
                     report.fills_unregistered,
                     static_cast<unsigned long long>(report.collateral_balance),
                     static_cast<unsigned long long>(report.collateral_allowance),
                     static_cast<unsigned long long>(cfg.target_allowance_base),
                     static_cast<unsigned long long>(report.ledger_inventory),
                     reasons);
        if (!ready) {
            std::fprintf(stderr,
                         "FATAL: startup reconciliation is BLOCKED (%s); refusing to "
                         "trade on divergent state\n",
                         reasons);
            if (user_ws) { user_ws->stop(); user_ws->join(); }
            if (heartbeat_monitor) {
                heartbeat_monitor->request_stop();
                heartbeat_monitor->join();
            }
            gateway->stop(false);
            return 1;
        }
        reconciled_inventory.store(report.ledger_inventory, std::memory_order_release);
        readiness_ready.store(true, std::memory_order_release);
        if (user_ws) user_ws->clear_stale();
        trading_enabled.store(true, std::memory_order_release);

        // ── Market data and alpha ingress ───────────────────────────────────
        market_listener = std::make_unique<WsMarketListener>(cfg, *book);
        market_listener->start();
        alpha_parser = std::make_unique<AlphaParser>(
            *signals, cfg.market_slug, cfg.max_q_value, cfg.min_confidence);
        alpha_receiver = std::make_unique<AlphaHttpReceiver>(cfg, *alpha_parser);
        if (!alpha_receiver->start()) {
            std::fprintf(stderr, "FATAL: cannot bind alpha receiver at %s:%u\n",
                         cfg.alpha_bind, cfg.alpha_port);
            trading_enabled.store(false, std::memory_order_release);
            market_listener->stop();
            return 1;
        }
        std::fprintf(stdout, "alpha ingress: http://%s:%u/signal\n", cfg.alpha_bind,
                     cfg.alpha_port);

        // ── Supervisor: readiness, metadata refresh, re-reconciliation ───────
        supervisor_thread = std::thread([&] {
            pin_to_cpu(cfg.cold_cpu);
            uint64_t last_metadata_refresh = now_ms();
            uint64_t last_reconciliation = now_ms();
            uint64_t last_inventory_publish = 0;
            bool fatal_metadata = false;
            bool metadata_stale = false;
            // Venue-derived parameters (tick size, minimum order size, fee rate,
            // negative-risk flag) have a bounded lifetime: BOT_METADATA_MAX_AGE_MS.
            // The published generation carries its own observation timestamp, and
            // invalidate() clears it, so "no published metadata" is stale too.
            const auto metadata_fresh = [&]() noexcept {
                return venue::metadata_is_fresh(runtime.observed_wall_ns(),
                                                cfg.metadata_max_age_ms,
                                                ledger::now_wall_ns());
            };
            while (workers_running.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                const uint64_t now = now_ms();

                // 1. Heartbeat watchdog.
                if (heartbeat_monitor) {
                    const heartbeat::Health health = heartbeat_monitor->health();
                    if (heartbeat::health_blocks_new_orders(health)) {
                        if (trading_enabled.load(std::memory_order_acquire)) {
                            std::fprintf(stderr,
                                         "TRADING DISABLED: heartbeat %s (age %llu ms)\n",
                                         heartbeat::health_name(health),
                                         static_cast<unsigned long long>(
                                             heartbeat_monitor->age_ms()));
                            if (heartbeat::health_implies_cancelled_orders(health))
                                std::fprintf(stderr,
                                             "  resting orders must be assumed "
                                             "cancelled: the venue may kill them as "
                                             "late as %u ms after the last "
                                             "acknowledged beat; reconciliation is "
                                             "the only way back\n",
                                             heartbeat::K_VENUE_WORST_CASE_MS);
                            trading_enabled.store(false, std::memory_order_release);
                            readiness_ready.store(false, std::memory_order_release);
                        }
                    }
                }

                // 2. The published runtime metadata must be valid.
                if (!runtime.valid()) {
                    if (trading_enabled.load(std::memory_order_acquire)) {
                        std::fprintf(stderr,
                                     "TRADING DISABLED: runtime metadata is not "
                                     "published/valid\n");
                        trading_enabled.store(false, std::memory_order_release);
                    }
                }

                // 2b. Metadata age guard (BOT_METADATA_MAX_AGE_MS).
                if (!metadata_fresh()) {
                    if (!metadata_stale) {
                        metadata_stale = true;
                        const uint64_t age = venue::metadata_age_ms(
                            runtime.observed_wall_ns(), ledger::now_wall_ns());
                        const bool age_known = age != UINT64_MAX;
                        std::fprintf(stderr,
                                     "TRADING DISABLED: venue metadata is stale "
                                     "(age %llu ms%s, budget %llu ms); a successful "
                                     "refresh is required before egress resumes\n",
                                     age_known ? static_cast<unsigned long long>(age)
                                               : 0ULL,
                                     age_known ? "" : " (unknown)",
                                     static_cast<unsigned long long>(
                                         cfg.metadata_max_age_ms));
                        trading_enabled.store(false, std::memory_order_release);
                        runtime.invalidate();
                    }
                }

                // 3. Unknown order state blocks egress.
                if (ledger && ledger->blocking_orders() != 0) {
                    if (trading_enabled.load(std::memory_order_acquire)) {
                        std::fprintf(stderr,
                                     "TRADING DISABLED: %zu order(s) in "
                                     "SUBMITTING/UNKNOWN state\n",
                                     ledger->blocking_orders());
                        trading_enabled.store(false, std::memory_order_release);
                    }
                }

                // 4. User-channel staleness → reconcile, then re-enable.
                if (user_ws && user_ws->stale()) {
                    if (trading_enabled.load(std::memory_order_acquire)) {
                        std::fprintf(stderr,
                                     "TRADING DISABLED: user channel is stale "
                                     "(state=%s)\n",
                                     user_ws::stream_state_name(user_ws->state()));
                        trading_enabled.store(false, std::memory_order_release);
                    }
                    const bool attached =
                        user_ws->state() == user_ws::StreamState::SUBSCRIBED;
                    if (attached && reconciler && now - last_reconciliation > 1000) {
                        last_reconciliation = now;
                        recon::Report rerun{};
                        const bool ready_again =
                            reconciler->run_after_disconnect(rerun, heartbeat_monitor.get());
                        char rerun_reasons[256]{};
                        rerun.format_reasons(rerun_reasons, sizeof(rerun_reasons));
                        std::fprintf(stdout,
                                     "reconciliation after disconnect: %s (%s)\n",
                                     recon::readiness_name(rerun.readiness),
                                     rerun_reasons);
                        if (ready_again) {
                            user_ws->clear_stale();
                            readiness_ready.store(true, std::memory_order_release);
                            if (metadata_fresh() &&
                                (!ledger || ledger->blocking_orders() == 0) &&
                                (!heartbeat_monitor ||
                                 !heartbeat::health_blocks_new_orders(
                                     heartbeat_monitor->health()))) {
                                metadata_stale = false;
                                trading_enabled.store(true, std::memory_order_release);
                            }
                        }
                    }
                }

                // 5. Publish reconciled inventory for the hot path (lock-free read).
                if (ledger && now - last_inventory_publish >= 250) {
                    last_inventory_publish = now;
                    reconciled_inventory.store(
                        ledger->available_inventory(cfg.token_id_dec),
                        std::memory_order_release);
                }

                // 6. Metadata refresh: a tick-size change invalidates the book and
                //    the presigned ladder; a negative-risk change is fatal because
                //    the signer's EIP-712 domain would be wrong.
                if (now - last_metadata_refresh >= cfg.metadata_refresh_ms &&
                    !fatal_metadata) {
                    last_metadata_refresh = now;
                    venue::PipelineOptions options{};
                    options.require_status = true;
                    venue::MetadataPipeline pipeline(*api_client, options);
                    venue::MetadataPolicy policy{};
                    policy.require_gamma_status = true;
                    policy.allow_protocol_v2 = cfg.allow_protocol_v2_positions;
                    policy.max_taker_fee_micro =
                        static_cast<uint64_t>(cfg.max_taker_fee_rate * 1000000.0);
                    venue::PipelineResult refreshed{};
                    if (!pipeline.run(cfg.market_slug, cfg.condition_id,
                                      cfg.token_id_dec, policy, refreshed)) {
                        std::fprintf(stderr,
                                     "TRADING DISABLED: metadata refresh failed (%s)\n",
                                     refreshed.detail);
                        trading_enabled.store(false, std::memory_order_release);
                        runtime.invalidate();
                        continue;
                    }
                    if (refreshed.metadata.neg_risk != cfg.neg_risk) {
                        fatal_metadata = true;
                        trading_enabled.store(false, std::memory_order_release);
                        runtime.invalidate();
                        std::fprintf(stderr,
                                     "FATAL: neg_risk changed (%d -> %d); the EIP-712 "
                                     "domain no longer matches the signer. Restart "
                                     "after re-running preflight.\n",
                                     cfg.neg_risk ? 1 : 0,
                                     refreshed.metadata.neg_risk ? 1 : 0);
                        g_running.store(false, std::memory_order_release);
                        continue;
                    }
                    const bool tick_changed =
                        refreshed.metadata.tick_raw != cfg.tick_size;
                    const bool size_changed =
                        refreshed.metadata.min_size_raw != cfg.min_size_shares;
                    venue::MetadataReport refresh_report{};
                    if (!runtime.publish(refreshed.metadata, policy, refresh_report)) {
                        char refresh_reasons[256]{};
                        refresh_report.format(refresh_reasons,
                                              sizeof(refresh_reasons));
                        std::fprintf(stderr,
                                     "TRADING DISABLED: refreshed metadata invalid (%s)\n",
                                     refresh_reasons);
                        trading_enabled.store(false, std::memory_order_release);
                        continue;
                    }
                    if (tick_changed) {
                        // A tick change is survivable: the book, the presigned
                        // ladder and the engine all read the book's atomic tick.
                        // The book is invalidated so no stale level can be traded.
                        std::fprintf(stderr,
                                     "tick size changed in flight: %.4f -> %.4f; "
                                     "book invalidated, presigned ladder will "
                                     "rebuild\n",
                                     static_cast<double>(cfg.tick_size) * 1e-6,
                                     static_cast<double>(refreshed.metadata.tick_raw) *
                                         1e-6);
                        book->set_tick_size(refreshed.metadata.tick_raw);
                        book->invalidate();
                    }
                    if (size_changed) {
                        // cfg is read by the hot path and must stay immutable for
                        // the life of the process: a changed minimum order size
                        // therefore stops trading until a restart re-derives it.
                        std::fprintf(stderr,
                                     "TRADING DISABLED: venue minimum order size "
                                     "changed (%.2f -> %.2f shares); restart after "
                                     "re-running preflight\n",
                                     static_cast<double>(cfg.min_size_shares) * 1e-6,
                                     static_cast<double>(
                                         refreshed.metadata.min_size_raw) * 1e-6);
                        trading_enabled.store(false, std::memory_order_release);
                        readiness_ready.store(false, std::memory_order_release);
                        book->invalidate();
                    }
                    metadata_result = refreshed;
                    if (metadata_stale) {
                        metadata_stale = false;
                        // The age guard was the reason egress was blocked; lift it
                        // only when no other blocking condition is present.
                        if (metadata_fresh() &&
                            !trading_enabled.load(std::memory_order_acquire) &&
                            readiness_ready.load(std::memory_order_acquire) &&
                            (!ledger || ledger->blocking_orders() == 0) &&
                            (!user_ws || !user_ws->stale()) &&
                            (!heartbeat_monitor ||
                             !heartbeat::health_blocks_new_orders(
                                 heartbeat_monitor->health()))) {
                            trading_enabled.store(true, std::memory_order_release);
                            std::fprintf(stdout,
                                         "TRADING ENABLED: venue metadata refreshed, "
                                         "age guard cleared\n");
                        }
                    }
                }
            }
        });
    }
#else
    (void)observer;
    mock_thread = std::thread([&] {
        pin_to_cpu(cfg.cold_cpu);
        mock_feed(*signals, *book, cfg, workers_running);
    });
    trading_enabled.store(true, std::memory_order_release);
#endif

    // Rebuild only when top/tick changes or freshness reaches half-TTL.
    std::thread presign_thread([&] {
        pin_to_cpu(cfg.cold_cpu);
        uint64_t last_bid = 0, last_ask = 0, last_tick = 0, last_build = 0;
        while (workers_running.load(std::memory_order_acquire)) {
            OrderBookL2::Top top{};
            if (book->read_top(top) && top.bid.size && top.ask.size) {
                const uint64_t tick = book->tick_size(cfg.tick_size);
                const uint64_t now = PresignedOrderPool::now_ms();
                if (top.bid.price != last_bid || top.ask.price != last_ask ||
                    tick != last_tick || now - last_build >= cfg.presign_ttl_ms / 2) {
                    const double price =
                        static_cast<double>(top.ask.price) * 1e-6;
                    const uint64_t target = KellyEngine::usd_to_shares_fixed(
                        cfg.max_order_usd, price);
                    if (target >= cfg.min_size_shares &&
                        pool.rebuild(top.bid.price, top.ask.price, target, tick)) {
                        last_bid = top.bid.price;
                        last_ask = top.ask.price;
                        last_tick = tick;
                        last_build = now;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    pin_to_cpu(cfg.pin_cpu);
    constexpr size_t RESULT_COUNT = static_cast<size_t>(TickResult::COUNT);
    uint64_t counts[RESULT_COUNT]{};
    const auto start = std::chrono::steady_clock::now();

#if defined(CROWDINTEL_LIVE)
    if (live) {
        ExecutionEngine<OrderGateway<LightweightCLOBClient>> engine(
            cfg, *book, *signals, signer, pool, *gateway, &trading_enabled,
            &reconciled_inventory);
        run_engine(engine, cfg, counts, RESULT_COUNT);
        std::fprintf(stdout, "  engine confirmed_inventory=%llu reconciled=%llu\n",
                     static_cast<unsigned long long>(engine.confirmed_inventory()),
                     static_cast<unsigned long long>(engine.reconciled_inventory()));
    } else {
        ExecutionEngine<OrderGateway<PaperEgressClient>> engine(
            cfg, *book, *signals, signer, pool, *paper_gateway, &trading_enabled,
            &reconciled_inventory);
        run_engine(engine, cfg, counts, RESULT_COUNT);
    }
#else
    {
        MockCLOBClient mock_client(cfg);
        ExecutionEngine<MockCLOBClient> engine(
            cfg, *book, *signals, signer, pool, mock_client, &trading_enabled);
        run_engine(engine, cfg, counts, RESULT_COUNT);
    }
#endif

    // ── Shutdown: stop egress first, then cancel, then the feeds ────────────
    trading_enabled.store(false, std::memory_order_release);
    workers_running.store(false, std::memory_order_release);
#ifdef CROWDINTEL_LIVE
    if (live && gateway) {
        gateway->stop(false);  // discard unsent work; never drain after a stop
        // Best-effort flatten: if the heartbeat contract is active the venue
        // would cancel these orders within ~10-15 s anyway, but an explicit
        // cancel is deterministic and immediate.
        clob::CallResult cancel{};
        if (api_client->cancel_all(cancel))
            std::fprintf(stdout, "shutdown: cancel-all accepted\n");
        else
            std::fprintf(stderr, "shutdown: cancel-all failed (%s)%s\n",
                         cancel.detail,
                         cfg.heartbeat_enabled
                             ? "; the heartbeat contract will cancel them when the "
                               "beats stop"
                             : "");
    }
    if (paper_gateway) paper_gateway->stop(false);
    if (supervisor_thread.joinable()) supervisor_thread.join();
    if (user_ws) { user_ws->stop(); user_ws->join(); }
    if (heartbeat_monitor) {
        heartbeat_monitor->request_stop();
        heartbeat_monitor->join();
    }
    if (alpha_receiver) alpha_receiver->stop();
    if (market_listener) market_listener->stop();
#endif
    if (mock_thread.joinable()) mock_thread.join();
    if (kill_switch_thread.joinable()) kill_switch_thread.join();
    presign_thread.join();

    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::fprintf(stdout, "\nSession %.2fs\n", seconds);
    for (size_t i = 1; i < RESULT_COUNT; ++i) {
        if (counts[i]) std::fprintf(stdout, "  %-20s %llu\n",
            tick_result_name(static_cast<TickResult>(i)),
            static_cast<unsigned long long>(counts[i]));
    }
#ifdef CROWDINTEL_LIVE
    if (gateway) std::fprintf(stdout,
        "  gateway enqueued=%llu accepted=%llu rejected=%llu dropped=%llu "
        "retries=%llu cancelled=%llu ambiguous=%llu unrecorded=%llu\n",
        static_cast<unsigned long long>(gateway->enqueued()),
        static_cast<unsigned long long>(gateway->accepted()),
        static_cast<unsigned long long>(gateway->rejected()),
        static_cast<unsigned long long>(gateway->dropped()),
        static_cast<unsigned long long>(gateway->retried()),
        static_cast<unsigned long long>(gateway->cancelled()),
        static_cast<unsigned long long>(gateway->ambiguous()),
        static_cast<unsigned long long>(gateway->unrecorded()));
    if (market_listener) std::fprintf(stdout, "  market ws events=%llu reconnects=%llu\n",
        static_cast<unsigned long long>(market_listener->events_seen()),
        static_cast<unsigned long long>(market_listener->reconnects()));
    if (user_ws) {
        const user_ws::FrameProcessor& processor = user_ws->processor();
        std::fprintf(stdout,
                     "  user ws state=%s frames=%llu applied=%llu duplicates=%llu "
                     "parse_failures=%llu divergences=%llu keepalives=%llu "
                     "connect_failures=%llu session_failures=%llu stale=%d\n",
                     user_ws::stream_state_name(user_ws->state()),
                     static_cast<unsigned long long>(processor.frames()),
                     static_cast<unsigned long long>(processor.events_applied()),
                     static_cast<unsigned long long>(processor.duplicates()),
                     static_cast<unsigned long long>(processor.parse_failures()),
                     static_cast<unsigned long long>(processor.divergences()),
                     static_cast<unsigned long long>(processor.keepalive_acks()),
                     static_cast<unsigned long long>(user_ws->connect_failures()),
                     static_cast<unsigned long long>(user_ws->session_failures()),
                     user_ws->stale() ? 1 : 0);
    }
    if (heartbeat_monitor) std::fprintf(stdout,
        "  heartbeat health=%s accepted=%llu resyncs=%llu ambiguous_failures=%llu\n",
        heartbeat::health_name(heartbeat_monitor->health()),
        static_cast<unsigned long long>(heartbeat_monitor->accepted_count()),
        static_cast<unsigned long long>(heartbeat_monitor->resync_count()),
        static_cast<unsigned long long>(heartbeat_monitor->ambiguous_failures()));
    if (alpha_receiver) std::fprintf(stdout, "  alpha accepted=%llu rejected=%llu\n",
        static_cast<unsigned long long>(alpha_receiver->accepted()),
        static_cast<unsigned long long>(alpha_receiver->rejected()));
#endif
    if (ledger) {
        char error[128]{};
        if (!ledger->checkpoint(error, sizeof(error)))
            std::fprintf(stderr, "ledger checkpoint failed: %s\n", error);
        std::fprintf(stdout,
                     "  ledger events=%llu replayed=%llu duplicates=%llu "
                     "illegal_transitions=%llu open_orders=%zu blocking=%zu "
                     "checkpoints=%llu\n",
                     static_cast<unsigned long long>(ledger->applied_events()),
                     static_cast<unsigned long long>(ledger->replayed_events()),
                     static_cast<unsigned long long>(ledger->duplicates_ignored()),
                     static_cast<unsigned long long>(ledger->illegal_transitions()),
                     ledger->open_orders(), ledger->blocking_orders(),
                     static_cast<unsigned long long>(ledger->checkpoints()));

        // Post-mortem in the last lines of the log: why the session ended up
        // blocked and what the heartbeat contract was doing.  Both are read
        // through the ledger's locked copy accessors and formatted by the pure
        // helpers in session_report.hpp (unit-tested without a running bot).
        const uint64_t shutdown_wall_ns = ledger::now_wall_ns();
        ledger::ReconciliationRun last_run{};
        if (ledger->last_run_copy(last_run)) {
            char line[512]{};
            if (report::format_last_reconciliation(last_run, shutdown_wall_ns, line,
                                                   sizeof(line)))
                std::fprintf(stdout, "  last_reconciliation %s\n", line);
        } else {
            std::fprintf(stdout, "  last_reconciliation none recorded\n");
        }
        ledger::HeartbeatState heartbeat_state{};
        if (ledger->heartbeat_copy(heartbeat_state)) {
            char line[256]{};
            if (report::format_heartbeat_contract(heartbeat_state, shutdown_wall_ns,
                                                  line, sizeof(line)))
                std::fprintf(stdout, "  heartbeat_contract %s\n", line);
        }
    }
    std::fprintf(stdout, "  readiness=%s\n",
                 readiness_ready.load(std::memory_order_acquire) ? "READY" : "BLOCKED");
    return 0;
}
