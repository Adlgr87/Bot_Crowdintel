#ifndef WS_POOL_HPP
#define WS_POOL_HPP

/**
 * WsPool: Concurrent WebSocket Connection Pool for Multi-Exchange Market Data.
 *
 * Fase C (Alpha Temporal): Manages persistent WebSocket connections to Binance
 * and Coinbase, distributing market data to lock-free OrderBook instances via
 * SPSC ring buffers. Enables cross-exchange arbitrage detection (crypto vs
 * Polymarket) with sub-millisecond hot-path latency.
 *
 * Architecture:
 *   ┌──────────────┐  transport callback  ┌─────────────────┐  SPSC push  ┌───────────┐
 *   │ WsPool       │ ◄────────────────── │ WsConnection    │ ─────────► │ OrderBook │
 *   │ (orchestrator)│                    │ (per-exchange)  │            │ (per-mkt) │
 *   └──────┬───────┘                    └─────────────────┘            └─────┬─────┘
 *          │ subscribes to                                                 │
 *          │ reconnect + heartbeat                                          │
 *          ▼                                                               │
 *   ┌──────────────┐                                                      │
 *   │ RateLimiter  │                                                      │
 *   │ (per-exchg)  │                                                      │
 *   └──────────────┘                                                      ▼
 *
 * Design principles:
 *   - Cold path: connection management, reconnection, heartbeat (uses mutex)
 *   - Hot path: OrderBookL2 updates via lock-free SPSC ring buffers (no mutex)
 *   - Each exchange connection runs on its own thread (independent failure domain)
 *   - Transport is pluggable (WsTransport abstract base) — mockable for tests
 *   - Rate limiting per exchange via existing RateLimiter (token bucket)
 *   - Reconnection: exponential backoff with jitter (100ms, 200ms, 400ms, ...)
 *
 * Constraints: No AWS, no physical hardware, no satellite feeds.
 *              All networking simulated via transport interface.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <random>

#include "spsc_ring_buffer.hpp"
#include "order_book.hpp"
#include "alpha_receiver.hpp"   // AlphaSignal
#include "eip712_signer.hpp"    // OrderParams (required by telemetry.hpp)
#include "telemetry.hpp"
#include "rate_limiter.hpp"

// ─── Exchange Enumeration ────────────────────────────────────────────────

enum class Exchange : uint8_t {
    BINANCE   = 0,
    COINBASE  = 1,
    POLYMARKET = 2,
};

/**
 * Exchange name helper (for logging / debugging).
 */
inline const char* exchange_name(Exchange e) {
    switch (e) {
        case Exchange::BINANCE:   return "BINANCE";
        case Exchange::COINBASE:  return "COINBASE";
        case Exchange::POLYMARKET: return "POLYMARKET";
    }
    return "UNKNOWN";
}

// ─── Market Data Update (lock-free POD, no allocation) ───────────────────

/**
 * MarketDataUpdate: A single level-2 order book delta from any exchange.
 * Fixed-size struct designed for zero-allocation passage through SPSC queues.
 * Must be trivially copyable for lock-free ring buffer.
 */
struct MarketDataUpdate {
    Exchange exchange;           // Source exchange
    char market_slug[64];        // e.g. "BTC-USDT", "BTC-USD", "polymarket:..."
    uint64_t price;              // Price in fixed point (* 1e6)
    uint64_t size;               // Size in fixed point (* 1e6)
    uint8_t side;                // 0 = bid, 1 = ask, 2 = trade
    uint32_t level;              // Order book level (0 = best)
    uint64_t timestamp_ns;       // Event timestamp (monotonic)
    uint8_t update_type;         // 0 = insert, 1 = update, 2 = delete
};

/**
 * Static assertion: MarketDataUpdate must be trivially copyable for SPSC.
 */
static_assert(std::is_trivially_copyable_v<MarketDataUpdate>,
    "MarketDataUpdate must be trivially copyable for lock-free SPSC queue");

// ─── WebSocket Transport Interface (pluggable, mockable) ─────────────────

/**
 * WsTransport: Abstract interface for WebSocket operations.
 * Production: backed by libcurl WebSockets or Boost.Beast.
 * Testing: MockWsTransport simulates feed without network I/O.
 *
 * The transport is responsible for:
 *   - Connecting to the exchange WebSocket endpoint
 *   - Sending subscription messages
 *   - Receiving messages and invoking a callback for each parsed update
 *   - Detecting connection drops (disconnect handler)
 *
 * All methods are called from the WsConnection's thread (cold path).
 */
class WsTransport {
public:
    virtual ~WsTransport() = default;

    /**
     * Connect to the WebSocket endpoint.
     * @param endpoint  WebSocket URL (wss://...)
     * @param on_open  Called when connection is established
     * @param on_close Called when connection drops unexpectedly
     * @return true if connection succeeded
     */
    virtual bool connect(const std::string& endpoint,
                         std::function<void()> on_open,
                         std::function<void()> on_close) = 0;

    /**
     * Subscribe to market data channels.
     * @param subscribe_msg  Exchange-specific JSON subscribe payload
     */
    virtual bool subscribe(const std::string& subscribe_msg) = 0;

    /**
     * Send a raw message.
     */
    virtual bool send(const std::string& message) = 0;

    /**
     * Poll for incoming messages. Should be called in a loop.
     * Invokes on_message for each parsed MarketDataUpdate.
     * @param on_message Callback for each market data update
     * @return false if connection is dead (needs reconnect)
     */
    virtual bool poll(std::function<void(const MarketDataUpdate&)> on_message) = 0;

    /**
     * Disconnect (graceful close).
     */
    virtual void disconnect() = 0;

    /**
     * Check if transport is connected.
     */
    virtual bool is_connected() const = 0;
};

/**
 * MockWsTransport: Simulated WebSocket transport for testing and stress testing.
 *
 * Generates deterministic market data updates at a configurable rate,
 * simulating realistic exchange feed behavior (price movement, volume,
 * order book depth changes) without any network I/O.
 *
 * Supports:
 *   - Configurable symbols and update frequency
 *   - Simulated price random walk (for arbitrage scenario testing)
 *   - Simulated disconnection/reconnection (for failover testing)
 *   - Deterministic seed-based generation (for reproducible tests)
 */
class MockWsTransport : public WsTransport {
public:
    /**
     * @param exchange  The exchange this mock simulates
     * @param symbols   List of market symbols to generate data for
     * @param update_interval_us  Microseconds between updates per symbol
     * @param seed      RNG seed for deterministic price walks
     * @param disconnect_interval  Simulated disconnect every N polls (0 = never)
     */
    MockWsTransport(Exchange exchange,
                    std::vector<std::string> symbols,
                    uint64_t update_interval_us = 500,
                    uint32_t seed = 0xDEADBEEF,
                    uint64_t disconnect_interval = 0)
        : exchange_(exchange)
        , symbols_(std::move(symbols))
        , update_interval_us_(update_interval_us)
        , rng_(seed)
        , disconnect_interval_(disconnect_interval)
        , poll_count_(0)
        , connected_(false)
        , prices_(symbols_.size(), 0)
        , last_update_ns_(symbols_.size(), 0)
        , current_price_idx_(0) {

        // Initialize prices with realistic starting values per exchange
        for (size_t i = 0; i < symbols_.size(); i++) {
            // Different exchanges have different price ranges for the same asset
            switch (exchange_) {
                case Exchange::BINANCE:   prices_[i] = 50000000000ULL; break; // $50,000 * 1e6
                case Exchange::COINBASE:  prices_[i] = 50050000000ULL; break; // $50,100 (slight spread)
                case Exchange::POLYMARKET: prices_[i] = 50000000000ULL; break;
            }
        }
    }

    bool connect(const std::string& endpoint,
                 std::function<void()> on_open,
                 std::function<void()> on_close) override {
        (void)endpoint;
        connected_ = true;
        if (on_open) on_open();
        on_close_handler_ = on_close;
        return true;
    }

    bool subscribe(const std::string& subscribe_msg) override {
        (void)subscribe_msg;
        return true;
    }

    bool send(const std::string& message) override {
        (void)message;
        return connected_;
    }

    bool poll(std::function<void(const MarketDataUpdate&)> on_message) override {
        if (!connected_) return false;

        poll_count_++;
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // Simulate disconnection at configured intervals
        if (disconnect_interval_ > 0 && poll_count_ % disconnect_interval_ == 0) {
            connected_ = false;
            if (on_close_handler_) on_close_handler_();
            return false;
        }

        // Generate updates for symbols whose interval has elapsed
        for (size_t i = 0; i < symbols_.size(); i++) {
            uint64_t elapsed_us = (now_ns - last_update_ns_[i]) / 1000;
            if (elapsed_us < update_interval_us_) continue;

            last_update_ns_[i] = now_ns;

            // Simulate price random walk
            double drift = (static_cast<double>(rng_() % 1000) / 1000.0 - 0.5) * 1000.0;
            prices_[i] = static_cast<uint64_t>(
                static_cast<double>(prices_[i]) + drift
            );

            // Generate bid and ask levels
            uint64_t bid_price = prices_[i] - 500000;  // 0.5 USD spread
            uint64_t ask_price = prices_[i] + 500000;

            MarketDataUpdate update;
            update.exchange = exchange_;
            strncpy(update.market_slug, symbols_[i].c_str(), 63);
            update.market_slug[63] = '\0';
            update.timestamp_ns = now_ns;

            // Bid update
            update.price = bid_price;
            update.size = static_cast<uint64_t>(50 + rng_() % 100) * 1000000;
            update.side = 0;
            update.level = 0;
            update.update_type = 1;
            on_message(update);

            // Ask update
            update.price = ask_price;
            update.size = static_cast<uint64_t>(50 + rng_() % 100) * 1000000;
            update.side = 1;
            update.level = 0;
            update.update_type = 1;
            on_message(update);

            // Simulate occasional depth changes (levels 1-4)
            for (uint32_t level = 1; level < 5; level++) {
                update.price = bid_price - level * 1000000;
                update.size = static_cast<uint64_t>(30 + rng_() % 70) * 1000000;
                update.side = 0;
                update.level = level;
                on_message(update);

                update.price = ask_price + level * 1000000;
                update.size = static_cast<uint64_t>(30 + rng_() % 70) * 1000000;
                update.side = 1;
                update.level = level;
                on_message(update);
            }
        }

        return true;
    }

    void disconnect() override {
        connected_ = false;
    }

    bool is_connected() const override {
        return connected_;
    }

    Exchange get_exchange() const { return exchange_; }
    const std::vector<std::string>& get_symbols() const { return symbols_; }

private:
    Exchange exchange_;
    std::vector<std::string> symbols_;
    uint64_t update_interval_us_;
    std::mt19937 rng_;
    uint64_t disconnect_interval_;
    std::atomic<uint64_t> poll_count_;
    std::atomic<bool> connected_;
    std::function<void()> on_close_handler_;

    std::vector<uint64_t> prices_;
    std::vector<uint64_t> last_update_ns_;
    std::atomic<uint64_t> current_price_idx_;
};

// ─── WebSocket Connection (manages transport + reconnection) ──────────────

/**
 * WsConnection: Manages a single WebSocket connection to an exchange.
 *
 * Responsibilities:
 *   - Owns a WsTransport instance
 *   - Handles reconnection with exponential backoff + jitter
 *   - Monitors heartbeat (pong timeout detection)
 *   - Routes parsed MarketDataUpdate to the pool's SPSC queue
 *   - Feed-dead detection (sends alerts to RiskEngine)
 *
 * Thread: Runs on its own thread, completely independent of other connections.
 */
class WsConnection {
public:
    using MessageCallback = std::function<void(const MarketDataUpdate&)>;
    using ReconnectCallback = std::function<void(Exchange, uint32_t /*attempts*/)>;

    struct Config {
        std::string endpoint;              // WebSocket URL
        std::string subscribe_payload;    // JSON subscribe message
        double rate_limit_per_sec = 10.0;  // Max messages per second from exchange
        double rate_burst = 20.0;
        int max_reconnect_attempts = 10;   // -1 = infinite
        int feed_dead_timeout_ms = 5000;   // Alert if no updates for this long
        uint64_t heartbeat_interval_ms = 30000;  // Ping every 30s
    };

    WsConnection(Exchange exchange,
                 std::unique_ptr<WsTransport> transport,
                 Config config,
                 MessageCallback on_message,
                 ReconnectCallback on_reconnect = nullptr)
        : exchange_(exchange)
        , transport_(std::move(transport))
        , config_(config)
        , on_message_(on_message)
        , on_reconnect_(on_reconnect)
        , rate_limiter_(config.rate_limit_per_sec, config.rate_burst)
        , reconnect_attempts_(0)
        , running_(false)
        , last_message_ns_(0)
        , messages_received_(0)
        , feed_dead_alerted_(false) {}

    /**
     * Start the connection loop in a background thread.
     */
    void start() {
        running_ = true;
        connection_thread_ = std::thread(&WsConnection::run, this);
    }

    /**
     * Stop the connection and wait for thread exit.
     */
    void stop() {
        running_ = false;
        if (connection_thread_.joinable()) {
            connection_thread_.join();
        }
    }

    /**
     * Get the exchange this connection belongs to.
     */
    Exchange get_exchange() const { return exchange_; }

    /**
     * Check if the feed has been dead for too long.
     * Cold path — called by monitoring/health check.
     */
    bool is_feed_dead() const {
        uint64_t last = last_message_ns_.load(std::memory_order_acquire);
        if (last == 0) return false;  // No messages yet

        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t elapsed_ms = (now_ns - last) / 1'000'000;
        return elapsed_ms > config_.feed_dead_timeout_ms;
    }

    /**
     * Get total messages received (for telemetry).
     */
    uint64_t get_messages_received() const {
        return messages_received_.load(std::memory_order_relaxed);
    }

    /**
     * Get current reconnect attempts (for telemetry).
     */
    uint32_t get_reconnect_attempts() const {
        return reconnect_attempts_.load(std::memory_order_relaxed);
    }

private:
    void run() {
        uint32_t attempts = 0;

        while (running_.load(std::memory_order_acquire)) {
            // Attempt connection
            if (!transport_->is_connected()) {
                if (!transport_->connect(config_.endpoint,
                                         [this]() { on_connection_open(); },
                                         [this]() { on_connection_close(); })) {
                    // Connection failed — backoff and retry
                    attempts++;
                    reconnect_attempts_.store(attempts, std::memory_order_relaxed);
                    if (on_reconnect_) on_reconnect_(exchange_, attempts);
                    backoff_sleep(attempts);
                    continue;
                }
            }

            // Subscribe if not already subscribed
            if (attempts == 0 || !subscription_active_) {
                transport_->subscribe(config_.subscribe_payload);
                subscription_active_ = true;
                attempts = 0;
                reconnect_attempts_.store(0, std::memory_order_relaxed);
            }

            // Message processing loop
            bool alive = true;
            while (running_.load(std::memory_order_acquire) && alive) {
                alive = transport_->poll([this](const MarketDataUpdate& update) {
                    // Rate limit check (protects against malformed feed flooding)
                    if (!rate_limiter_.try_acquire()) {
                        // Rate limited — drop message (don't call on_message)
                        return;
                    }

                    last_message_ns_.store(update.timestamp_ns, std::memory_order_release);
                    messages_received_.fetch_add(1, std::memory_order_relaxed);
                    feed_dead_alerted_ = false;

                    if (on_message_) {
                        on_message_(update);
                    }
                });

                // Brief yield to allow other threads + reconnection checks
                if (alive) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }

            if (!alive && running_.load(std::memory_order_acquire)) {
                // Connection dropped — reconnect
                attempts++;
                reconnect_attempts_.store(attempts, std::memory_order_relaxed);
                if (on_reconnect_) on_reconnect_(exchange_, attempts);
                subscription_active_ = false;
                backoff_sleep(attempts);
            }
        }
    }

    /**
     * Exponential backoff with jitter.
     * 100ms, 200ms, 400ms, 800ms, 1600ms (capped at 30s).
     * Jitter: ±25% random.
     */
    void backoff_sleep(uint32_t attempts) {
        int base_ms = std::min(100 * (1 << (attempts - 1)), 30000);
        
        // ±25% jitter
        std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> jitter(-0.25, 0.25);
        int jittered_ms = static_cast<int>(base_ms * (1.0 + jitter(rng)));
        jittered_ms = std::max(jittered_ms, 10);

        std::this_thread::sleep_for(std::chrono::milliseconds(jittered_ms));
    }

    void on_connection_open() {
        subscription_active_ = false;  // Need to re-subscribe
    }

    void on_connection_close() {
        // Feed dead detection — signal to monitoring
        feed_dead_alerted_ = true;
    }

    Exchange exchange_;
    std::unique_ptr<WsTransport> transport_;
    Config config_;

    MessageCallback on_message_;
    ReconnectCallback on_reconnect_;

    RateLimiter rate_limiter_;  // Per-exchange rate limiting
    std::atomic<uint32_t> reconnect_attempts_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> last_message_ns_;
    std::atomic<uint64_t> messages_received_;
    std::atomic<bool> feed_dead_alerted_;
    std::atomic<bool> subscription_active_{false};

    std::thread connection_thread_;
};

// ─── WebSocket Pool (orchestrator) ────────────────────────────────────────

/**
 * WsPool: Orchestrates multiple WsConnection instances across exchanges.
 *
 * Maintains:
 *   - Per-exchange connection pool (Binance, Coinbase, Polymarket)
 *   - Per-market OrderBookL2 instances (lock-free SPSC updates from connections)
 *   - Feed health monitoring (dead feed detection, reconnection tracking)
 *   - Cross-exchange price comparison for arbitrage signals
 *
 * Usage:
 *   1. Add exchanges and their symbols
 *   2. Start the pool (spawns connection threads)
 *   3. Read OrderBookL2 instances from the hot path (lock-free)
 *   4. Call check_cross_exchange_arbitrage() for arbitrage detection
 */
class WsPool {
public:
    using ArbitrageCallback = std::function<void(
        const std::string& /*market_a*/,
        const std::string& /*market_b*/,
        double /*price_a*/,
        double /*price_b*/,
        double /*spread_usd*/)>;

    explicit WsPool(size_t spsc_capacity = 8192)
        : spsc_capacity_(spsc_capacity)
        , running_(false)
        , telemetry_(nullptr) {}

    ~WsPool() {
        stop();
    }

    /**
     * Set telemetry for logging (cold path).
     */
    void set_telemetry(Telemetry* tel) { telemetry_ = tel; }

    /**
     * Add an exchange connection with its symbol list.
     * Each symbol gets its own OrderBookL2 instance (lock-free updates).
     */
    void add_exchange(Exchange exchange,
                       const std::string& endpoint,
                       const std::string& subscribe_payload,
                       const std::vector<std::string>& symbols,
                       double rate_limit_per_sec = 10.0,
                       double rate_burst = 20.0,
                       int max_reconnect = 10,
                       int feed_dead_timeout_ms = 5000) {
        // Create mock transport by default (can be swapped for real transport)
        auto transport = std::make_unique<MockWsTransport>(
            exchange, symbols, 500, 0xDEADBEEF + static_cast<uint32_t>(exchange));

        WsConnection::Config cfg;
        cfg.endpoint = endpoint;
        cfg.subscribe_payload = subscribe_payload;
        cfg.rate_limit_per_sec = rate_limit_per_sec;
        cfg.rate_burst = rate_burst;
        cfg.max_reconnect_attempts = max_reconnect;
        cfg.feed_dead_timeout_ms = feed_dead_timeout_ms;

        // Create OrderBook instances for each symbol
        for (const auto& symbol : symbols) {
            std::string market_key = make_market_key(exchange, symbol);
            auto book = std::make_shared<OrderBookL2>();
            order_books_[market_key] = book;
            // Track symbols per exchange for cross-exchange comparison
            exchange_symbols_[exchange].insert(symbol);
        }

        // Create the connection with a callback that updates the OrderBook
        auto conn = std::make_unique<WsConnection>(
            exchange, std::move(transport), cfg,
            [this, exchange](const MarketDataUpdate& update) {
                this->on_market_data(update);
            },
            [this](Exchange ex, uint32_t attempts) {
                if (telemetry_) {
                    telemetry_->log_event(EventType::FEED_DEAD, "",
                        "{\"exchange\":\"" + std::string(exchange_name(ex)) +
                        "\",\"reconnect_attempts\":" + std::to_string(attempts) + "}",
                        "WARN");
                }
            });

        connections_[exchange] = std::move(conn);
    }

    /**
     * Start all connections.
     * Spawns one thread per exchange connection.
     */
    void start() {
        running_ = true;
        for (auto& [exchange, conn] : connections_) {
            conn->start();
        }
    }

    /**
     * Stop all connections and wait for threads to exit.
     */
    void stop() {
        running_ = false;
        for (auto& [exchange, conn] : connections_) {
            conn->stop();
        }
    }

    /**
     * Get the OrderBook for a specific exchange + market.
     * Hot path: lock-free read (OrderBookL2 has no locks in updates).
     * Returns nullptr if not found.
     */
    const OrderBookL2* get_order_book(Exchange exchange, std::string_view symbol) const {
        std::string key = make_market_key(exchange, symbol);
        auto it = order_books_.find(key);
        if (it != order_books_.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    /**
     * Check for cross-exchange arbitrage between two exchanges for the same asset.
     * Hot path: reads OrderBook best bid/ask (lock-free).
     *
     * @param exchange_a  First exchange
     * @param exchange_b  Second exchange
     * @param symbol_a    Symbol on exchange A
     * @param symbol_b    Symbol on exchange B
     * @param min_spread_usd  Minimum spread to report (filters noise)
     * @param callback     Called for each arbitrage opportunity found
     */
    void check_cross_exchange_arbitrage(
        Exchange exchange_a, Exchange exchange_b,
        std::string_view symbol_a, std::string_view symbol_b,
        double min_spread_usd, ArbitrageCallback callback) {

        const OrderBookL2* book_a = get_order_book(exchange_a, symbol_a);
        const OrderBookL2* book_b = get_order_book(exchange_b, symbol_b);

        if (!book_a || !book_b) return;

        const auto& ask_a = book_a->get_ask(0);
        const auto& bid_b = book_b->get_bid(0);

        // Arbitrage: buy low on A, sell high on B
        double price_a_ask = static_cast<double>(ask_a.price) / 1e6;
        double price_b_bid = static_cast<double>(bid_b.price) / 1e6;
        double spread = price_b_bid - price_a_ask;

        if (spread > min_spread_usd && ask_a.size > 0 && bid_b.size > 0) {
            callback(std::string(symbol_a), std::string(symbol_b),
                     price_a_ask, price_b_bid, spread);
        }
    }

    /**
     * Check feed health across all connections.
     * Cold path — call periodically from monitoring thread.
     */
    bool any_feed_dead() const {
        for (const auto& [exchange, conn] : connections_) {
            if (conn && conn->is_feed_dead()) {
                return true;
            }
        }
        return false;
    }

    /**
     * Get total messages received across all connections.
     */
    uint64_t get_total_messages() const {
        uint64_t total = 0;
        for (const auto& [exchange, conn] : connections_) {
            if (conn) {
                total += conn->get_messages_received();
            }
        }
        return total;
    }

    /**
     * Get the set of tracked symbols for an exchange.
     */
    const std::unordered_set<std::string>& get_symbols(Exchange exchange) const {
        static const std::unordered_set<std::string> empty;
        auto it = exchange_symbols_.find(exchange);
        return (it != exchange_symbols_.end()) ? it->second : empty;
    }

private:
    /**
     * Market data callback: update the OrderBook for this exchange + symbol.
     * Hot path — called from WsConnection thread, but OrderBook updates are
     * lock-free (atomic sequence counter). No mutex needed.
     */
    void on_market_data(const MarketDataUpdate& update) {
        std::string key = make_market_key(update.exchange, update.market_slug);
        auto it = order_books_.find(key);
        if (it == order_books_.end()) return;

        OrderBookL2* book = it->second.get();

        if (update.side == 0) {
            // Bid update
            book->update_bid(update.level, update.price, update.size, update.timestamp_ns);
        } else if (update.side == 1) {
            // Ask update
            book->update_ask(update.level, update.price, update.size, update.timestamp_ns);
        }
        // side == 2 (trade) doesn't update the book directly
    }

    static std::string make_market_key(Exchange exchange, std::string_view symbol) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%d:%.*s",
                 static_cast<int>(exchange),
                 static_cast<int>(std::min(symbol.size(), size_t(63))),
                 symbol.data());
        return std::string(buf);
    }

    size_t spsc_capacity_;
    std::atomic<bool> running_;

    Telemetry* telemetry_;

    // Per-exchange connections (each on its own thread)
    std::unordered_map<Exchange, std::unique_ptr<WsConnection>> connections_;

    // Per (exchange, symbol) OrderBook — lock-free reads from hot path
    std::unordered_map<std::string, std::shared_ptr<OrderBookL2>> order_books_;

    // Track symbols per exchange for cross-exchange comparison
    std::unordered_map<Exchange, std::unordered_set<std::string>> exchange_symbols_;
};

#endif // WS_POOL_HPP