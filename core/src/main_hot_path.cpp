// Bot CrowdIntel — live topology:
// market WSS -> atomic top/depth book; authenticated HTTP alpha -> SPSC;
// presigner -> consumable ladder; execution -> async network gateway.

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
#include "../include/order_book.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "alpha_parser.hpp"
#include "execution_engine.hpp"
#include "market_config.hpp"
#include "mock_client.hpp"
#include "order_ledger.hpp"
#include "presigned_pool.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK) && !defined(CROWDINTEL_FORCE_MOCK)
#include "alpha_http_receiver.hpp"
#include "lightweight_client.hpp"
#include "market_resolver.hpp"
#include "order_gateway.hpp"
#include "order_heartbeat.hpp"
#include "preflight.hpp"
#include "reconciliation.hpp"
#include "user_event_applier.hpp"
#include "user_ws_client.hpp"
#include "ws_market_listener.hpp"
#define CROWDINTEL_LIVE 1
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

void mock_feed(SPSC_RingBuffer<AlphaSignal>& queue, OrderBookL2& book,
               const MarketConfig& cfg, std::atomic<bool>& running) {
    Level2Entry bids[3] = {
        {470000, 40000000}, {460000, 60000000}, {450000, 90000000}};
    Level2Entry asks[3] = {
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

#if defined(CROWDINTEL_LIVE)
// Monotonic milliseconds for the heartbeat state machine. steady_clock is
// immune to wall-clock jumps; the service only needs deltas.
uint64_t steady_clock_ms() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
#endif  // CROWDINTEL_LIVE

template <typename Engine, typename Drain>
long run_engine(Engine& engine, const MarketConfig& cfg,
                uint64_t* counts, size_t count_size, Drain drain) {
    long productive = 0;
    while (g_running.load(std::memory_order_acquire)) {
        drain();  // private-channel events become journal transitions here
        const TickResult result = engine.run_tick();
        const size_t index = static_cast<size_t>(result);
        if (index < count_size) ++counts[index];
        if (result == TickResult::NO_SIGNAL) idle_wait();
        else ++productive;
        if (cfg.max_ticks > 0 && productive >= cfg.max_ticks) break;
    }
    return productive;
}

}  // namespace

int main(int argc, char** argv) {
    // Configuration-only mode for operators: validates the deployed
    // EnvironmentFile exactly as startup would and exits without opening a
    // socket, loading no venue state. It is the safe way to check a new file
    // (typos in variable names abort startup) before restarting the service.
    bool check_config_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check-config") == 0) {
            check_config_only = true;
        } else {
            std::fprintf(stderr, "usage: %s [--check-config]\n", argv[0]);
            return 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);  // OpenSSL can ultimately write(2) a dead peer
#endif

    // The mode is always explicit (BOT_MODE=replay|paper|live) and decides
    // which topology starts. An offline build has no network transports at
    // all, so it forces replay and rejects paper/live.
    MarketConfig::LoadOptions load_options;
#if !defined(CROWDINTEL_LIVE)
    load_options.force_replay = true;
#endif

    MarketConfig cfg;
    if (const char* error = cfg.load(load_options)) {
        std::fprintf(stderr, "FATAL: %s\n", error);
        return 1;
    }
    const bool replay = cfg.replay_mode();

    if (check_config_only) {
        std::printf("CONFIG OK mode=%s market=%s armed=%d ledger=%s\n",
                    MarketConfig::mode_name(cfg.mode), cfg.market_slug,
                    cfg.live_armed ? 1 : 0,
                    cfg.ledger_path[0] ? cfg.ledger_path : "(none)");
        return 0;
    }

#if defined(CROWDINTEL_LIVE)
    // Fase 1: every venue-controlled parameter (tick, minimum order size, fee
    // curve, negative-risk flag, outcome token, market status) comes from the
    // public venue metadata and is cross-validated before anything else starts.
    // The signer is initialized *after* this step because its EIP-712 domain
    // depends on the negative-risk flag the venue just told us.
    if (!replay) {
        MarketMetadataResolver resolver(cfg.clob_host, cfg.gamma_host, cfg.tls_pin);
        char resolve_error[192];
        if (!resolver.resolve(cfg, resolve_error, sizeof(resolve_error))) {
            std::fprintf(stderr, "READINESS = BLOCKED reason=%s\n", resolve_error);
            return 1;
        }
        char exchange_hex[43];
        crowdintel::format_address_hex(cfg.runtime.exchange, exchange_hex);
        std::fprintf(stdout,
            "READINESS = READY condition=%s token=%s outcome=%s tick=%.6f "
            "min_size=%.2f fee=%.6f^%.2f neg_risk=%d exchange=%s "
            "clock_offset_ms=%lld fee_source=%s\n",
            cfg.runtime.condition_id, cfg.runtime.token_id_dec,
            cfg.runtime.outcome_label,
            static_cast<double>(cfg.runtime.tick_size) * 1e-6,
            static_cast<double>(cfg.runtime.min_order_size) * 1e-6,
            cfg.runtime.fee_rate, cfg.runtime.fee_exponent,
            cfg.runtime.neg_risk ? 1 : 0, exchange_hex,
            static_cast<long long>(cfg.runtime.clock_offset_ms),
            cfg.runtime.fee_source);
    }
#endif

    // Fase 2: crash-safe order journal. The recovery gate runs before the
    // signer exists, and the file is the only source of truth for orders that
    // outlived a previous process. Live mode requires an explicit path.
    cledger::OrderLedger ledger;
    bool ledger_active = false;
    if (cfg.ledger_path[0]) {
        cledger::OpenOptions ledger_options;
        ledger_options.fsync_records = cfg.ledger_fsync;
        ledger_options.max_bytes = cfg.ledger_max_bytes;
        char ledger_error[192];
        if (!ledger.open(cfg.ledger_path, ledger_options, ledger_error,
                         sizeof(ledger_error))) {
            std::fprintf(stderr, "READINESS = BLOCKED reason=ledger: %s\n",
                         ledger_error);
            return 1;
        }
        ledger_active = true;
        std::fprintf(stdout,
            "ledger path=%s records=%llu orders=%zu unreconciled=%zu "
            "unknown=%zu torn_tail_bytes=%llu\n",
            ledger.path(),
            static_cast<unsigned long long>(ledger.records_written()),
            ledger.order_count(), ledger.unreconciled_orders(),
            ledger.unknown_orders(),
            static_cast<unsigned long long>(ledger.torn_tail_bytes()));
        if (!ledger.gate_open())
            std::fprintf(stdout,
                "ledger gate closed: unreconciled=%zu unknown=%zu "
                "(Phase 5 reconciliation decides)\n",
                ledger.unreconciled_orders(), ledger.unknown_orders());
    } else if (cfg.live_mode()) {
        std::fprintf(stderr,
                     "READINESS = BLOCKED reason=ledger_path_missing "
                     "(BOT_LEDGER_PATH is mandatory in live mode)\n");
        return 1;
    }

#if defined(CROWDINTEL_LIVE)
    // Fase 5: startup reconciliation. The journal records what this process
    // believes; only an authenticated venue query can prove it. This runs after
    // the journal is open and before the signer exists, so no order can be
    // signed — let alone sent — while the account state is unproven. It is
    // deliberately blocking: a live bot that cannot prove its positions must
    // not start at all.
    std::unique_ptr<recon::RestTransport> recon_transport;
    std::unique_ptr<recon::Reconciler<recon::RestTransport>> reconciler;

#if defined(CROWDINTEL_LIVE)
    // Fase 6: account preflight. Reconciliation proves the orders; this proves
    // the account can pay for one. The transport is the same one Phase 5 built,
    // and the requirements come from configuration and the resolved metadata —
    // no venue-controlled value is assumed here.
    preflight::Requirements account_requirements;
    if (cfg.runtime.resolved) {
        crowdintel::format_address_hex(cfg.runtime.exchange,
                                       account_requirements.exchange_hex);
        account_requirements.collateral_required_f6 = static_cast<uint64_t>(
            std::llround(cfg.max_order_usd * 1000000.0));
        account_requirements.conditional_required_f6 =
            cfg.initial_position_shares;
        account_requirements.signature_type = static_cast<int>(cfg.signature_type);
    }
    auto run_account_preflight = [&](const char* trigger) -> bool {
        if (!recon_transport) return false;
        preflight::Preflight<recon::RestTransport> check(
            *recon_transport, cfg, account_requirements);
        char error[256];
        const preflight::Verdict verdict = check.run(error, sizeof(error));
        const preflight::Report& report = check.report();
        std::fprintf(stdout,
            "account preflight trigger=%s verdict=%s reason=%s "
            "collateral=%.6f required=%.6f allowance=%s conditional=%.6f "
            "conditional_approval=%s closed_only=%d requests=%llu detail=%s\n",
            trigger, preflight::verdict_name(verdict),
            preflight::reason_name(check.reason()),
            static_cast<double>(report.collateral_balance_f6) * 1e-6,
            static_cast<double>(report.collateral_required_f6) * 1e-6,
            report.collateral_allowance_unlimited
                ? "unlimited"
                : (report.collateral_allowance_f6 == 0 ? "MISSING" : "present"),
            static_cast<double>(report.conditional_balance_f6) * 1e-6,
            report.conditional_approval_present ? "present" : "MISSING",
            report.closed_only ? 1 : 0,
            static_cast<unsigned long long>(report.requests), error);
        return verdict == preflight::Verdict::kReady;
    };
#endif

    if (cfg.live_mode() && ledger_active) {
        if (cfg.owner_api_key[0] == '\0') {
            std::fprintf(stderr,
                "READINESS = BLOCKED reason=reconcile_configuration "
                "(CLOB_API_KEY is required to prove the journal against the "
                "venue)\n");
            return 1;
        }
        const l2auth::Credentials recon_credentials{
            cfg.owner_api_key, cfg.api_address_hex, cfg.api_secret_b64,
            cfg.api_passphrase};
        recon_transport = std::make_unique<recon::RestTransport>(
            cfg.clob_host, recon_credentials, cfg.tls_pin);
        if (!recon_transport->usable()) {
            std::fprintf(stderr,
                "READINESS = BLOCKED reason=reconcile_configuration detail=%s\n",
                recon_transport->last_error());
            return 1;
        }
        reconciler = std::make_unique<recon::Reconciler<recon::RestTransport>>(
            *recon_transport, cfg);
        char recon_error[192];
        const recon::Result recon_result =
            reconciler->run(ledger, recon_error, sizeof(recon_error));
        const recon::Report& recon_report = reconciler->report();
        std::fprintf(stdout,
            "reconcile trigger=startup result=%s pages=%llu/%llu live=%llu "
            "open=%llu present=%llu canceled=%llu absent=%llu trades=%llu "
            "closed_only=%d detail=%s\n",
            recon::result_name(recon_result),
            static_cast<unsigned long long>(recon_report.order_pages),
            static_cast<unsigned long long>(recon_report.trade_pages),
            static_cast<unsigned long long>(recon_report.live_orders),
            static_cast<unsigned long long>(recon_report.orders_open),
            static_cast<unsigned long long>(recon_report.orders_present),
            static_cast<unsigned long long>(recon_report.orders_canceled),
            static_cast<unsigned long long>(recon_report.orders_absent),
            static_cast<unsigned long long>(recon_report.trades_seen),
            recon_report.closed_only ? 1 : 0, recon_error);
        recon::ReadinessInputs readiness;
        readiness.metadata_resolved = true;
        readiness.ledger_open = true;
        readiness.reconciliation_ok = recon_result == recon::Result::kReady;
        readiness.gate_open = ledger.gate_open();
        readiness.unknown_orders = ledger.unknown_orders();
        readiness.open_orders = static_cast<size_t>(recon_report.orders_open);
        readiness.closed_only = recon_report.closed_only;
        readiness.user_channel_configured = cfg.owner_api_key[0] != '\0';
        readiness.heartbeat_configured = cfg.api_secret_b64[0] != '\0';
        readiness.account_state_ok = run_account_preflight("startup");
        const recon::ReadinessReason reason = recon::evaluate_readiness(readiness);
        if (reason != recon::ReadinessReason::kReady) {
            std::fprintf(stderr,
                "READINESS = BLOCKED reason=%s detail=%s (nothing is sent)\n",
                recon::readiness_reason_name(reason),
                recon_error[0] ? recon_error
                               : "account state not proven by the venue");
            if (reason == recon::ReadinessReason::kOpenOrdersPresent)
                std::fprintf(stderr,
                    "note: %llu order(s) are proven resting at the venue and "
                    "this bot only trades FAK; no heartbeat runs after this "
                    "exit, so the venue cancels them within 10 s. Verify the "
                    "account before restarting.\n",
                    static_cast<unsigned long long>(recon_report.orders_open));
            return 1;
        }
        std::fprintf(stdout,
            "READINESS = READY reconciliation=startup orders=%zu unknown=%zu\n",
            ledger.order_count(), ledger.unknown_orders());
    }
#endif

    // A closed journal gate stops the process. Since Phase 7 only live runs
    // may own a journal, so this is a safety net for a directly constructed
    // configuration rather than a replay/paper path.
    if (ledger_active && !ledger.gate_open()) {
        std::fprintf(stderr,
            "READINESS = BLOCKED reason=unreconciled_orders count=%zu "
            "unknown=%zu ledger=%s (no venue credentials to reconcile with)\n",
            ledger.unreconciled_orders(), ledger.unknown_orders(),
            ledger.path());
        return 1;
    }

    // Fase 3: private user channel. Orders confirmed by the venue move to a
    // terminal state from this feed; a divergence stops new orders until the
    // Phase 5 reconciliation proves the account state again.
#if defined(CROWDINTEL_LIVE)
    std::unique_ptr<UserWsClient> user_listener;
    std::unique_ptr<user_apply::UserEventApplier> user_applier;
    std::unique_ptr<HeartbeatTransport> heartbeat_transport;
    std::unique_ptr<HeartbeatService<HeartbeatTransport>> heartbeat_service;
    if (cfg.live_mode() && ledger_active) {
        if (cfg.owner_api_key[0] == '\0' || cfg.runtime.condition_id[0] == '\0') {
            std::fprintf(stderr,
                "READINESS = BLOCKED reason=user_channel_configuration "
                "(CLOB_API_KEY and a resolved condition id are required)\n");
            return 1;
        }
        user_applier = std::make_unique<user_apply::UserEventApplier>(cfg);
        user_applier->attach_ledger(&ledger);
        user_listener = std::make_unique<UserWsClient>(cfg);
        user_listener->start();
        std::fprintf(stdout, "user channel starting (ws=%s)\n",
                     cfg.user_ws_host[0] ? cfg.user_ws_host : cfg.ws_host);

        // Fase 4: order heartbeat. The venue cancels every open order of these
        // credentials when a valid heartbeat is missing for 10 s, so losing it
        // is treated as a transport failure, never as a cancellation.
        l2auth::Credentials credentials{cfg.owner_api_key, cfg.api_address_hex,
                                        cfg.api_secret_b64, cfg.api_passphrase};
        heartbeat_transport =
            std::make_unique<HeartbeatTransport>(cfg.clob_host, credentials);
        if (!heartbeat_transport->usable()) {
            std::fprintf(stderr,
                "READINESS = BLOCKED reason=heartbeat_configuration "
                "(a decodable CLOB API secret is required)\n");
            user_listener->stop();
            return 1;
        }
        heartbeat_service = std::make_unique<HeartbeatService<HeartbeatTransport>>(
            *heartbeat_transport, heartbeat::Thresholds(), steady_clock_ms);
        heartbeat_service->start();
        std::fprintf(stdout,
                     "order heartbeat starting (interval=5s critical=10s url=%s/v1/heartbeats)\n",
                     cfg.clob_host);
    }
#endif

    // Parse the private key exactly once, initialize the only signer, then
    // remove the original environment entry and wipe the temporary bytes.
    uint8_t private_key[32];
    if (!parse_hex_bytes(cfg.private_key_hex, std::strlen(cfg.private_key_hex),
                         private_key, sizeof(private_key))) {
        std::fprintf(stderr, "FATAL: invalid BOT_PRIVATE_KEY_HEX\n");
        return 1;
    }
    EIP712Signer signer;
    if (!signer.init(private_key, cfg.effective_neg_risk())) {
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
    if (!replay && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr,
                     "FATAL: mlockall failed; refusing paper/live mode with "
                     "swappable key material\n");
        return 1;
    }
#endif

    std::fprintf(stdout,
        "CrowdIntel 2.1 mode=%s market=%s tick=%.4f sig=%u order=%s\n",
        MarketConfig::mode_name(cfg.mode), cfg.market_slug,
        static_cast<double>(cfg.effective_tick()) * 1e-6,
        cfg.signature_type, cfg.order_type);

    auto book = std::make_unique<OrderBookL2>();
    auto signals = std::make_unique<SPSC_RingBuffer<AlphaSignal>>();
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    MockCLOBClient mock_client(cfg, cfg.paper_mode() ? "paper" : "mock");
    std::atomic<bool> workers_running{true};
    std::atomic<bool> trading_enabled{true};

    std::thread mock_thread;
    std::thread kill_switch_thread;
#if defined(CROWDINTEL_LIVE)
    std::unique_ptr<WsMarketListener> market_listener;
    std::unique_ptr<AlphaParser> alpha_parser;
    std::unique_ptr<AlphaHttpReceiver> alpha_receiver;
    std::unique_ptr<LightweightCLOBClient> live_client;
    std::unique_ptr<OrderGateway<LightweightCLOBClient>> gateway;
#endif

#if defined(CROWDINTEL_LIVE)
    auto start_kill_switch = [&] {
        kill_switch_thread = std::thread([&] {
            pin_to_cpu(cfg.cold_cpu);
            while (workers_running.load(std::memory_order_acquire)) {
                errno = 0;
                const int present = ::access(cfg.kill_switch_file, F_OK);
                if (present == 0 || errno != ENOENT) {
                    trading_enabled.store(false, std::memory_order_release);
                    g_running.store(false, std::memory_order_release);
                    std::fprintf(stderr,
                        present == 0 ? "KILL SWITCH active: %s\n"
                                     : "KILL SWITCH check failed closed: %s\n",
                        cfg.kill_switch_file);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    };
#endif  // CROWDINTEL_LIVE

    if (replay) {
        // Deterministic local feed; no socket is opened in this mode.
        mock_thread = std::thread([&] {
            pin_to_cpu(cfg.cold_cpu);
            mock_feed(*signals, *book, cfg, workers_running);
        });
    }
#if defined(CROWDINTEL_LIVE)
    else {
        // Paper and live share the public market topology. The order gateway
        // -- the only thing that can reach the venue with an order -- exists
        // in live mode alone.
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

        if (cfg.live_mode()) {
            live_client = std::make_unique<LightweightCLOBClient>(cfg);
            gateway = std::make_unique<OrderGateway<LightweightCLOBClient>>(
                *live_client, &trading_enabled);
            gateway->start();
        } else {
            std::fprintf(stdout,
                "paper mode: live market data and metadata, local fill "
                "simulation, no order egress and no venue credentials\n");
        }

        start_kill_switch();
        std::fprintf(stdout, "alpha ingress: http://%s:%u/signal\n",
                     cfg.alpha_bind, cfg.alpha_port);
    }
#endif

    // Rebuild only when top/tick changes or freshness reaches half-TTL.
    std::thread presign_thread([&] {
        pin_to_cpu(cfg.cold_cpu);
        uint64_t last_bid = 0, last_ask = 0, last_tick = 0, last_build = 0;
        while (workers_running.load(std::memory_order_acquire)) {
            OrderBookL2::Top top{};
            if (book->read_top(top) && top.bid.size && top.ask.size) {
                const uint64_t tick = book->tick_size(cfg.effective_tick());
                const uint64_t min_size = cfg.effective_min_size();
                // Unknown grid or a venue tick change: sign nothing until the
                // metadata is resolved again for the new grid.
                const bool grid_known =
                    tick != 0 && min_size != 0 &&
                    (!cfg.live_transport() || !cfg.runtime.resolved ||
                     tick == cfg.runtime.tick_size);
                const uint64_t now = PresignedOrderPool::now_ms();
                if (grid_known &&
                    (top.bid.price != last_bid || top.ask.price != last_ask ||
                     tick != last_tick ||
                     now - last_build >= cfg.presign_ttl_ms / 2)) {
                    const double price =
                        static_cast<double>(top.ask.price) * 1e-6;
                    const uint64_t target = KellyEngine::usd_to_shares_fixed(
                        cfg.max_order_usd, price);
                    if (target >= min_size &&
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

    // Drains the private channel into the journal. Any event the journal
    // cannot attribute, or that contradicts it, stops trading: an unreconciled
    // account is not a tradable account.
#if defined(CROWDINTEL_LIVE)
    uint64_t user_divergence_events = 0;
    uint64_t heartbeat_timeouts = 0;
    uint64_t channel_gaps = 0;
    uint64_t reconciliations_run = 0;
    uint64_t trading_resumes = 0;

    // Fase 5 recovery path. Any loss of private-channel evidence (drop,
    // reconnect, heartbeat critical) pauses trading and marks the affected
    // orders unproven; only a successful REST reconciliation arms it again, and
    // arming resets the event dedupe table because the gap may have swallowed
    // events. A retry timer exists so that a transient REST failure cannot
    // leave a paused process with no way back.
    constexpr uint64_t kReconcileRetryMs = 30000;
    bool reconcile_pending = false;
    uint64_t last_reconcile_ms = 0;
    uint64_t user_reconnects_seen = user_listener ? user_listener->reconnects() : 0;
    bool user_was_connected = user_listener && user_listener->connected();

    auto readiness_for = [&](const recon::Report& report,
                             bool reconciliation_ok) {
        recon::ReadinessInputs inputs;
        inputs.metadata_resolved = true;
        inputs.ledger_open = ledger_active;
        inputs.reconciliation_ok = reconciliation_ok;
        inputs.gate_open = ledger_active && ledger.gate_open();
        inputs.unknown_orders = ledger_active ? ledger.unknown_orders() : 0;
        inputs.open_orders = static_cast<size_t>(report.orders_open);
        inputs.closed_only = report.closed_only;
        inputs.user_channel_configured = cfg.owner_api_key[0] != '\0';
        inputs.heartbeat_configured = cfg.api_secret_b64[0] != '\0';
        return inputs;
    };

    // Blocking by design: it runs on the hot thread only while trading is
    // already disabled, and its duration is bounded by kMaxPages request
    // timeouts. The market channel keeps updating the book on its own thread.
    auto reconcile_now = [&](const char* trigger) -> bool {
        if (!reconciler || !ledger_active) return false;
        ++reconciliations_run;
        last_reconcile_ms = steady_clock_ms();
        char error[192];
        const recon::Result result =
            reconciler->run(ledger, error, sizeof(error));
        const recon::Report& report = reconciler->report();
        std::fprintf(stdout,
            "reconcile trigger=%s result=%s pages=%llu/%llu live=%llu open=%llu "
            "untracked=%llu foreign=%llu present=%llu canceled=%llu absent=%llu "
            "trades=%llu unattributed=%llu malformed=%llu closed_only=%d\n",
            trigger, recon::result_name(result),
            static_cast<unsigned long long>(report.order_pages),
            static_cast<unsigned long long>(report.trade_pages),
            static_cast<unsigned long long>(report.live_orders),
            static_cast<unsigned long long>(report.orders_open),
            static_cast<unsigned long long>(report.untracked_live_orders),
            static_cast<unsigned long long>(report.foreign_live_orders),
            static_cast<unsigned long long>(report.orders_present),
            static_cast<unsigned long long>(report.orders_canceled),
            static_cast<unsigned long long>(report.orders_absent),
            static_cast<unsigned long long>(report.trades_seen),
            static_cast<unsigned long long>(report.unattributed_trades),
            static_cast<unsigned long long>(report.malformed_payloads),
            report.closed_only ? 1 : 0);
        const recon::ReadinessReason reason = recon::evaluate_readiness(
            readiness_for(report, result == recon::Result::kReady));
        if (reason != recon::ReadinessReason::kReady) {
            reconcile_pending = true;
            std::fprintf(stderr,
                "READINESS = BLOCKED reason=%s detail=%s (still not armed)\n",
                recon::readiness_reason_name(reason),
                error[0] ? error : "account state not proven by the venue");
            return false;
        }
        // Fase 6: an order state that is proven is not the same as an account
        // that can pay. Re-check the balance/allowances before arming again.
        if (!run_account_preflight(trigger)) {
            reconcile_pending = true;
            std::fprintf(stderr,
                "READINESS = BLOCKED reason=account_state (still not armed)\n");
            return false;
        }
        if (user_applier) user_applier->reset_after_reconciliation();
        reconcile_pending = false;
        if (!trading_enabled.load(std::memory_order_acquire)) {
            trading_enabled.store(true, std::memory_order_release);
            ++trading_resumes;
            std::fprintf(stdout,
                "TRADING RESUMED reason=reconciliation_ready trigger=%s\n",
                trigger);
        }
        return true;
    };

    // Marks every non-terminal order unproven and stops sending. Called for
    // every evidence gap, whether or not trading was already paused: the
    // journal must still record that the channel stopped being authoritative.
    auto pause_for_evidence_gap = [&](const char* reason) {
        char ledger_error[192];
        const size_t moved =
            ledger_active ? ledger.mark_transport_lost(ledger_error,
                                                       sizeof(ledger_error))
                          : 0;
        const bool was_enabled =
            trading_enabled.exchange(false, std::memory_order_acq_rel);
        reconcile_pending = true;
        ++channel_gaps;
        std::fprintf(stderr,
            "%s reason=%s orders_marked_unproven=%zu detail=%s\n",
            was_enabled ? "TRADING PAUSED" : "trading already paused", reason,
            moved, ledger_active ? ledger_error : "ledger inactive");
    };

    auto drain_user_channel = [&] {
        // A critical heartbeat means the venue may already be cancelling every
        // open order: stop sending, mark the orders unproven and let the REST
        // proof decide. Recovery of the heartbeat alone never resumes trading.
        if (heartbeat_service && heartbeat_service->take_critical_event()) {
            ++heartbeat_timeouts;
            pause_for_evidence_gap("heartbeat_lost");
        }
        // Drop -> pause and mark unproven. Reconnect -> immediate re-proof,
        // because everything that lived through the gap is no longer provable
        // by the channel and the dedupe table starts over.
        if (user_listener) {
            const bool connected = user_listener->connected();
            const uint64_t reconnects = user_listener->reconnects();
            if (user_was_connected && !connected)
                pause_for_evidence_gap("user_channel_lost");
            if (reconnects != user_reconnects_seen) {
                reconcile_pending = true;
                if (connected) {
                    std::fprintf(stderr,
                        "user channel reconnected (total=%llu): re-proving the "
                        "account state before arming\n",
                        static_cast<unsigned long long>(reconnects));
                    (void)reconcile_now("user_channel_reconnected");
                }
            }
            user_reconnects_seen = reconnects;
            user_was_connected = connected;
            if (reconcile_pending && connected &&
                user_listener->channel_healthy() &&
                steady_clock_ms() - last_reconcile_ms >= kReconcileRetryMs)
                (void)reconcile_now("retry");
        }
        if (!user_listener || !user_applier) return;
        UserEvent event;
        while (user_listener->poll(event)) {
            char apply_error[192];
            const user_apply::ApplyResult result =
                user_applier->apply(event, apply_error, sizeof(apply_error));
            if (result == user_apply::ApplyResult::kUnattributed ||
                result == user_apply::ApplyResult::kDivergence ||
                result == user_apply::ApplyResult::kIllegal) {
                ++user_divergence_events;
                if (trading_enabled.load(std::memory_order_acquire)) {
                    trading_enabled.store(false, std::memory_order_release);
                    std::fprintf(stderr,
                        "TRADING PAUSED reason=%s detail=%s order=%s\n",
                        user_apply::apply_result_name(result), apply_error,
                        event.venue_order_id);
                }
                // The channel can no longer be trusted for this order, and it
                // may not be able to prove it again (a documented status this
                // build does not map, a fill it cannot attribute): the REST
                // reconciliation is the only path back to arming.
                reconcile_pending = true;
            }
        }
    };
#endif

    if (!cfg.live_mode()) {
        // Replay and paper: the local simulator is the only execution path.
        ExecutionEngine<MockCLOBClient> engine(
            cfg, *book, *signals, signer, pool, mock_client, &trading_enabled);
        if (ledger_active) engine.attach_ledger(&ledger);
        run_engine(engine, cfg, counts, RESULT_COUNT, [] {});
    }
#if defined(CROWDINTEL_LIVE)
    else {
        ExecutionEngine<OrderGateway<LightweightCLOBClient>> engine(
            cfg, *book, *signals, signer, pool, *gateway, &trading_enabled);
        if (ledger_active) engine.attach_ledger(&ledger);
        run_engine(engine, cfg, counts, RESULT_COUNT, drain_user_channel);
    }
#endif

    workers_running.store(false, std::memory_order_release);
#if defined(CROWDINTEL_LIVE)
    if (heartbeat_service) heartbeat_service->stop();
    if (user_listener) user_listener->stop();
    if (alpha_receiver) alpha_receiver->stop();
    if (market_listener) market_listener->stop();
    if (gateway) gateway->stop();
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
#if defined(CROWDINTEL_LIVE)
    if (user_listener) std::fprintf(stdout,
        "  user channel reconnects=%llu events=%llu pings=%llu pongs=%llu "
        "malformed=%llu foreign=%llu server_errors=%llu dropped=%llu "
        "applied=%llu unattributed=%llu divergences=%llu illegal=%llu\n",
        static_cast<unsigned long long>(user_listener->reconnects()),
        static_cast<unsigned long long>(user_listener->events_queued()),
        static_cast<unsigned long long>(user_listener->pings_sent()),
        static_cast<unsigned long long>(user_listener->pongs_seen()),
        static_cast<unsigned long long>(user_listener->malformed_messages()),
        static_cast<unsigned long long>(user_listener->foreign_market_events()),
        static_cast<unsigned long long>(user_listener->server_errors()),
        static_cast<unsigned long long>(user_listener->events_dropped()),
        static_cast<unsigned long long>(user_applier->stats().applied),
        static_cast<unsigned long long>(user_applier->stats().unattributed),
        static_cast<unsigned long long>(user_applier->stats().divergences),
        static_cast<unsigned long long>(user_applier->stats().illegal));
    if (user_divergence_events)
        std::fprintf(stdout, "  trading paused by %llu channel divergences\n",
                     static_cast<unsigned long long>(user_divergence_events));
    if (heartbeat_service) std::fprintf(stdout,
        "  heartbeat sends=%llu acks=%llu failures=%llu resyncs=%llu "
        "timeouts=%llu state=%s\n",
        static_cast<unsigned long long>(heartbeat_service->sends()),
        static_cast<unsigned long long>(heartbeat_service->acks()),
        static_cast<unsigned long long>(heartbeat_service->failures()),
        static_cast<unsigned long long>(heartbeat_service->resyncs()),
        static_cast<unsigned long long>(heartbeat_service->timeouts()),
        heartbeat::state_name(heartbeat_service->state()));
    if (heartbeat_timeouts)
        std::fprintf(stdout, "  trading paused by %llu heartbeat timeouts\n",
                     static_cast<unsigned long long>(heartbeat_timeouts));
    if (reconciler || channel_gaps)
        std::fprintf(stdout,
            "  reconciliation runs=%llu evidence_gaps=%llu resumes=%llu "
            "pending=%d closed_only=%d\n",
            static_cast<unsigned long long>(reconciliations_run),
            static_cast<unsigned long long>(channel_gaps),
            static_cast<unsigned long long>(trading_resumes),
            reconcile_pending ? 1 : 0,
            reconciler && reconciler->report().closed_only ? 1 : 0);
#endif
    if (ledger_active) std::fprintf(stdout,
        "  ledger records=%llu intents=%llu fills=%llu unreconciled=%zu "
        "unknown=%zu bytes=%llu\n",
        static_cast<unsigned long long>(ledger.records_written()),
        static_cast<unsigned long long>(ledger.journaled_intents()),
        static_cast<unsigned long long>(ledger.journaled_fills()),
        ledger.unreconciled_orders(), ledger.unknown_orders(),
        static_cast<unsigned long long>(ledger.bytes_on_disk()));
#if defined(CROWDINTEL_LIVE)
    if (gateway) std::fprintf(stdout,
        "  gateway enqueued=%llu accepted=%llu rejected=%llu dropped=%llu retries=%llu cancelled=%llu\n",
        static_cast<unsigned long long>(gateway->enqueued()),
        static_cast<unsigned long long>(gateway->accepted()),
        static_cast<unsigned long long>(gateway->rejected()),
        static_cast<unsigned long long>(gateway->dropped()),
        static_cast<unsigned long long>(gateway->retried()),
        static_cast<unsigned long long>(gateway->cancelled()));
    if (market_listener) std::fprintf(stdout, "  ws events=%llu reconnects=%llu\n",
        static_cast<unsigned long long>(market_listener->events_seen()),
        static_cast<unsigned long long>(market_listener->reconnects()));
    if (alpha_receiver) std::fprintf(stdout, "  alpha accepted=%llu rejected=%llu\n",
        static_cast<unsigned long long>(alpha_receiver->accepted()),
        static_cast<unsigned long long>(alpha_receiver->rejected()));
#endif
    return 0;
}
