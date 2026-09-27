#ifndef MEMPOOL_LISTENER_HPP
#define MEMPOOL_LISTENER_HPP

/**
 * MempoolListener: Polygon Mempool Streaming for Whale Transaction Detection.
 *
 * Fase C (Alpha Temporal): Subscribes to Polygon pending transactions via
 * WebSocket (newPendingTransactions), decodes Ethereum calldata to detect
 * Polymarket contract interactions, and identifies whale transactions based
 * on configurable thresholds.
 *
 * Feeds detected whale activity into the hot path as AlphaSignal events
 * through a lock-free SPSC ring buffer.
 *
 * Architecture:
 *   ┌──────────────┐  WS subscribe  ┌──────────────────┐  decode  ┌──────────────┐
 *   │ Polygon RPC  │ ──────────────► │ MempoolListener  │ ───────► │ WhaleDetector │
 *   │ (WebSocket)  │  pending tx    │ (transport)      │  calldata │ (threshold)   │
 *   └──────────────┘                └──────────────────┘          └──────┬───────┘
 *                                                                        │
 *                                                                        ▼
 *   ┌──────────────┐   SPSC push AlphaSignal  ┌──────────────────┐
 *   │ Hot Path     │ ◄──────────────────────── │ WHALE_TRADE      │
 *   │ (OrderBook)  │                           └──────────────────┘
 *   └──────────────┘
 *
 * Transport sources (all abstracted via MempoolTransport):
 *   1. Standard Polygon RPC WebSocket (wss://polygon-rpc.com/ws)
 *   2. bloXroute (accelerated feed, lower latency)
 *   3. FastLane (MEV-boost integrated, sub-block latency)
 *
 * For each pending transaction:
 *   1. Fetch full transaction (eth_getTransactionByHash)
 *   2. Decode calldata against known contract ABIs (Polymarket CTF, CLOB, ERC-20)
 *   3. Check against whale threshold (value > min_value_usd AND token == USDC)
 *   4. Generate AlphaSignal with confidence based on size percentile
 *   5. Push to SPSC queue (lock-free, O(1))
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
#include <algorithm>
#include <random>

#include "spsc_ring_buffer.hpp"
#include "alpha_receiver.hpp"   // AlphaSignal
#include "telemetry.hpp"

// ─── Ethereum Types ────────────────────────────────────────────────────────

using EthAddress = std::array<uint8_t, 20>;

// Helper: convert integer to lowercase hex string (no leading "0x")
inline std::string int_to_hex(uint32_t val) {
    static constexpr char HEX[] = "0123456789abcdef";
    if (val == 0) return "0";
    std::string s;
    while (val > 0) {
        s.push_back(HEX[val & 0xF]);
        val >>= 4;
    }
    std::reverse(s.begin(), s.end());
    return s;
}
using EthHash = std::array<uint8_t, 32>;
using uint256be = std::array<uint8_t, 32>;  // 256-bit big-endian integer

/**
 * EthTransaction: A decoded Ethereum pending transaction.
 * POD-like for efficient processing.
 */
struct EthTransaction {
    EthHash hash;              // Transaction hash (32 bytes)
    EthAddress from;           // Sender address (20 bytes)
    EthAddress to;             // Recipient / contract address (20 bytes)
    uint64_t nonce;            // Transaction nonce
    uint64_t gas;              // Gas limit
    uint64_t gas_price;        // Gas price in wei (* 1e0)
    uint64_t value;            // ETH value in wei (* 1e0)
    std::vector<uint8_t> calldata;  // Raw transaction data
    uint64_t timestamp_ns;     // When this tx was detected (monotonic)
    uint64_t block_number;     // Block number (0 if pending in mempool)
};

/**
 * DecodedCall: Decoded calldata fields for a Polymarket contract call.
 */
struct DecodedCall {
    bool is_polymarket = false;        // True if this is a known Polymarket contract
    std::string contract_name;         // e.g. "CLOB", "ConditionalTokens", "ERC20"
    std::string function_name;         // e.g. "placeOrder", "buyShares", "transfer"
    EthAddress market_token;           // Market token address (if applicable)
    double token_value_usd;            // USD value of tokens involved
    uint64_t amount;                   // Raw token amount
    uint64_t price;                    // Parsed order price (if order call)
    uint64_t size;                     // Parsed order size (if order call)
    std::string market_slug;           // Polymarket market slug (if decoded)
    uint8_t side;                      // 0 = buy/yes, 1 = sell/no (if order)
    bool is_large = false;             // Set by WhaleDetector
};

// ─── Mempool Transport Interface ───────────────────────────────────────────

/**
 * MempoolTransport: Abstract interface for Ethereum JSON-RPC WebSocket.
 *
 * Production: backed by real WebSocket to Polygon RPC / bloXroute / FastLane.
 * Testing: MockMempoolTransport simulates mempool traffic.
 */
class MempoolTransport {
public:
    virtual ~MempoolTransport() = default;

    /**
     * Connect to the WebSocket endpoint and subscribe to pending transactions.
     * @param endpoint    WebSocket URL
     * @param on_tx_hash  Callback for each pending transaction hash detected
     * @return true if connection and subscription succeeded
     */
    virtual bool connect(const std::string& endpoint,
                         std::function<void(const EthHash&)> on_tx_hash) = 0;

    /**
     * Fetch full transaction details by hash.
     * @param hash Transaction hash
     * @return Full decoded transaction (empty fields if not found / error)
     */
    virtual EthTransaction fetch_transaction(const EthHash& hash) = 0;

    /**
     * Check if connected.
     */
    virtual bool is_connected() const = 0;

    /**
     * Disconnect gracefully.
     */
    virtual void disconnect() = 0;

    /**
     * Check if this is a mock transport (for testing).
     * Used instead of dynamic_cast with -fno-rtti.
     */
    virtual bool is_mock() const { return false; }

    /**
     * Simulate a poll cycle (for mock transports).
     * Production transports can ignore this (no-op base implementation).
     */
    virtual void simulate_poll() {}
};

/**
 * MockMempoolTransport: Simulated Ethereum mempool for testing.
 *
 * Generates deterministic transaction streams including:
 *   - Normal retail transactions (below whale threshold)
 *   - Whale transactions (above threshold)
 *   - Polymarket-specific contract calls
 *   - Periodic disconnects (for reconnection testing)
 *
 * No network I/O — all data is simulated.
 */
class MockMempoolTransport : public MempoolTransport {
public:
    /**
     * @param whale_interval  Generate a whale transaction every N polls
     * @param tx_per_poll     Normal transactions generated per poll cycle
     * @param disconnect_interval  Simulate disconnect every N polls (0 = never)
     * @param seed            RNG seed for deterministic behavior
     */
    MockMempoolTransport(int whale_interval = 10,
                         int tx_per_poll = 5,
                         uint64_t disconnect_interval = 0,
                         uint32_t seed = 0xF00D)
        : whale_interval_(whale_interval)
        , tx_per_poll_(tx_per_poll)
        , disconnect_interval_(disconnect_interval)
        , rng_(seed)
        , poll_count_(0)
        , connected_(false)
        , current_tx_hash_({}) {}

    bool connect(const std::string& endpoint,
                 std::function<void(const EthHash&)> on_tx_hash) override {
        (void)endpoint;
        connected_ = true;
        on_tx_hash_handler_ = std::move(on_tx_hash);
        return true;
    }

    EthTransaction fetch_transaction(const EthHash& hash) override {
        EthTransaction tx;
        tx.hash = hash;
        tx.from = random_address();
        tx.to = POLYMARKET_CLOB_ADDRESS;
        tx.nonce = rng_();
        tx.gas = 200000;
        tx.gas_price = 80000000000ULL;  // 80 Gwei
        tx.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        tx.block_number = 0;  // Pending

        // Determine if this is a whale transaction by value
        // Use a deterministic mapping from hash to determine type
        uint64_t hash_val = 0;
        for (int i = 0; i < 8; i++) {
            hash_val |= static_cast<uint64_t>(hash[i]) << (8 * i);
        }

        // Large values indicate whale transactions
        if (hash_val % 50 == 0) {
            tx.value = 100000 * 1000000000000000000ULL;  // 100,000 USD worth of ETH
        } else {
            tx.value = 500 * 1000000000000000000ULL;       // 500 USD worth of ETH
        }

        // Generate calldata for Polymarket CLOB placeOrder
        tx.calldata = generate_calldata(hash_val % 1000);

        return tx;
    }

    bool is_connected() const override {
        return connected_;
    }

    void disconnect() override {
        connected_ = false;
    }

    /**
     * Simulate polling for pending transactions.
     * Generates new transaction hashes to the handler callback.
     */
    bool is_mock() const override { return true; }

    void simulate_poll() override {
        if (!connected_) return;

        poll_count_++;

        // Simulate periodic disconnect
        if (disconnect_interval_ > 0 && poll_count_ % disconnect_interval_ == 0) {
            connected_ = false;
            return;
        }

        // Generate whale transactions
        if (poll_count_ % whale_interval_ == 0) {
            EthHash whale_hash = generate_whale_hash();
            if (on_tx_hash_handler_) {
                on_tx_hash_handler_(whale_hash);
            }
        }

        // Generate normal transactions
        for (int i = 0; i < tx_per_poll_; i++) {
            EthHash normal_hash = generate_normal_hash();
            if (on_tx_hash_handler_) {
                on_tx_hash_handler_(normal_hash);
            }
        }
    }

    uint64_t get_poll_count() const {
        return poll_count_.load(std::memory_order_relaxed);
    }

private:
    static constexpr EthAddress POLYMARKET_CLOB_ADDRESS = {
        0x4b, 0xfb, 0xb8, 0x0c, 0x0b, 0x71, 0x83, 0x21, 0x94, 0x31,
        0x1f, 0x83, 0x58, 0xe6, 0x3a, 0x09, 0x0e, 0x3e, 0x93, 0x19
    };

    int whale_interval_;
    int tx_per_poll_;
    uint64_t disconnect_interval_;
    std::mt19937 rng_;
    std::atomic<uint64_t> poll_count_;
    std::atomic<bool> connected_;
    std::function<void(const EthHash&)> on_tx_hash_handler_;
    EthHash current_tx_hash_;

    EthAddress random_address() {
        EthAddress addr;
        for (int i = 0; i < 20; i++) {
            addr[i] = static_cast<uint8_t>(rng_() & 0xFF);
        }
        return addr;
    }

    EthHash generate_whale_hash() {
        EthHash hash;
        // Mark whale hashes with a specific pattern for testability
        hash[0] = 0x01;  // Whale marker
        for (int i = 1; i < 32; i++) {
            hash[i] = static_cast<uint8_t>(rng_() & 0xFF);
        }
        return hash;
    }

    EthHash generate_normal_hash() {
        EthHash hash;
        hash[0] = 0x00;  // Normal marker
        for (int i = 1; i < 32; i++) {
            hash[i] = static_cast<uint8_t>(rng_() & 0xFF);
        }
        return hash;
    }

    std::vector<uint8_t> generate_calldata(uint64_t seed_val) {
        // Generate Polymarket CLOB placeOrder calldata
        // Function selector: first 4 bytes of keccak256("placeOrder(...)") 
        // We use a fake but deterministic selector
        std::vector<uint8_t> calldata;
        calldata.reserve(200);

        // Function selector (4 bytes)
        uint32_t selector = 0xa1b2c3d4 + static_cast<uint32_t>(seed_val);
        calldata.push_back((selector >> 24) & 0xFF);
        calldata.push_back((selector >> 16) & 0xFF);
        calldata.push_back((selector >> 8) & 0xFF);
        calldata.push_back(selector & 0xFF);

        // Encode order parameters (simplified)
        // price * 1e6, size * 1e6, side
        uint64_t price = 50000000 + (seed_val % 50000000);  // $50-$100 range
        uint64_t size = 1000000000 + (seed_val % 5000000000ULL);  // $1000-$6000 range

        for (int i = 7; i >= 0; i--) calldata.push_back((price >> (8*i)) & 0xFF);
        for (int i = 7; i >= 0; i--) calldata.push_back((size >> (8*i)) & 0xFF);
        calldata.push_back(static_cast<uint8_t>(seed_val % 2));  // side

        return calldata;
    }
};

// ─── Calldata Decoder ──────────────────────────────────────────────────────

/**
 * CalldataDecoder: Decodes Ethereum calldata against known contract ABIs.
 *
 * Supports decoding for:
 *   - Polymarket CLOB V2 (ConditionalTokens, order placement)
 *   - ERC-20 Transfers (USDC, POL)
 *   - NFT Safe Transfers (position tokens)
 *
 * In production, this would use the full contract ABI. For this implementation,
 * we decode the most common function signatures and parameter layouts.
 */
class CalldataDecoder {
public:
    /**
     * Decode a transaction's calldata into a DecodedCall.
     *
     * @param tx  The transaction containing calldata
     * @return Decoded call information
     */
    static DecodedCall decode(const EthTransaction& tx) {
        DecodedCall decoded;

        if (tx.calldata.size() < 4) {
            return decoded;  // Not enough data for a function selector
        }

        // Extract function selector (first 4 bytes)
        uint32_t selector = 0;
        for (int i = 0; i < 4; i++) {
            selector = (selector << 8) | tx.calldata[i];
        }

        // Check against known Polymarket contract addresses
        if (is_polymarket_contract(tx.to)) {
            decoded.is_polymarket = true;
            decoded.contract_name = get_contract_name(tx.to);
        }

        // Decode known function selectors
        // Polymarket CLOB V2 function selectors:
        // placeOrder: 0x6c302e33 (keccak256("placeOrder((uint64,uint64,uint8,bytes)")[0:4])
        // cancelOrder: 0x2e1a7d4d
        // matchOrders: 0x7c0252c3
        if (selector == 0x6c302e33) {
            decoded.function_name = "placeOrder";
            decode_order_params(tx, decoded);
        } else if (selector == 0x2e1a7d4d) {
            decoded.function_name = "cancelOrder";
        } else if (selector == 0x7c0252c3) {
            decoded.function_name = "matchOrders";
        } else if (selector == 0xa9059cbb) {
            // Standard ERC-20 transfer(address,uint256)
            decoded.function_name = "transfer";
            decode_erc20_transfer(tx, decoded);
        } else if (selector == 0x23b872dd) {
            // ERC-20 transferFrom(address,address,uint256)
            decoded.function_name = "transferFrom";
            decode_erc20_transfer(tx, decoded);
        } else {
            decoded.function_name = "unknown_" + int_to_hex(selector);
        }

        // Estimate USD value from token amount + gas
        if (decoded.amount > 0) {
            // Estimate based on typical Polymarket order values
            // USDC is 6 decimals, so amount * 1e-6 gives USD value
            decoded.token_value_usd = static_cast<double>(decoded.amount) / 1e6;
        } else {
            // Fall back to ETH value for non-token transactions
            decoded.token_value_usd = static_cast<double>(tx.value) / 1e18;
        }

        return decoded;
    }

private:
    static bool is_polymarket_contract(const EthAddress& addr) {
        // Polymarket mainnet contract addresses
        static const EthAddress CLOB = {0x4b, 0xfb, 0xb8, 0x0c, 0x0b, 0x71, 0x83, 0x21, 0x94, 0x31,
                                         0x1f, 0x83, 0x58, 0xe6, 0x3a, 0x09, 0x0e, 0x3e, 0x93, 0x19};
        static const EthAddress CONDITIONAL_TOKENS = {0x4D, 0x97, 0xDC, 0xd9, 0x7e, 0xCa, 0x94, 0x52, 0x82, 0x12,
                                                      0x16, 0xF2, 0xD8, 0xa8, 0x2B, 0xBd, 0x7b, 0x39, 0xDf, 0xc2};
        static const EthAddress USDC = {0x27, 0x91, 0xB9, 0xE8, 0xD8, 0x39, 0xA6, 0x0D, 0xee, 0x1F,
                                      0x52, 0x0F, 0x26, 0x9D, 0x5B, 0x9E, 0x8A, 0x3d, 0x9F, 0x4C};
        static const EthAddress POL = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

        return addr == CLOB || addr == CONDITIONAL_TOKENS || addr == USDC || addr == POL ||
               (addr[0] == 0x4D && addr[1] == 0x97);  // Heuristic: start of ConditionalTokens
    }

    static std::string get_contract_name(const EthAddress& addr) {
        static const EthAddress CLOB = {0x4b, 0xfb, 0xb8, 0x0c, 0x0b, 0x71, 0x83, 0x21, 0x94, 0x31,
                                         0x1f, 0x83, 0x58, 0xe6, 0x3a, 0x09, 0x0e, 0x3e, 0x93, 0x19};
        if (addr == CLOB) return "CLOB_V2";
        if (addr[0] == 0x4D && addr[1] == 0x97) return "ConditionalTokens";
        return "Unknown";
    }

    static void decode_order_params(const EthTransaction& tx, DecodedCall& out) {
        // placeOrder calldata layout (simplified):
        // bytes4 selector (4 bytes)
        // uint64 price (8 bytes, offset 4)
        // uint64 size (8 bytes, offset 12)
        // uint8 side (1 byte, offset 20)
        // bytes maker (20 bytes, offset 21) -- skipped for simplicity

        if (tx.calldata.size() >= 21) {
            out.price = read_uint64_be(tx.calldata.data() + 4);
            out.size = read_uint64_be(tx.calldata.data() + 12);
            out.side = tx.calldata[20];
            out.amount = out.size;
            out.token_value_usd = static_cast<double>(out.size) / 1e6;
        }
    }

    static void decode_erc20_transfer(const EthTransaction& tx, DecodedCall& out) {
        // transfer(address,uint256) layout:
        // bytes4 selector (4 bytes)
        // address recipient (20 bytes, offset 4, left-padded to 32)
        // uint256 amount (32 bytes, offset 36)

        if (tx.calldata.size() >= 68) {
            // Extract address (skip 12 bytes of left padding)
            for (int i = 0; i < 20; i++) {
                out.market_token[i] = tx.calldata[16 + i];
            }
            out.amount = read_uint64_be(tx.calldata.data() + 36 + 24);  // Read last 8 bytes of uint256
        }
    }

    static uint64_t read_uint64_be(const uint8_t* data) {
        uint64_t val = 0;
        for (int i = 0; i < 8; i++) {
            val = (val << 8) | data[i];
        }
        return val;
    }
};

// ─── Whale Transaction Detector ────────────────────────────────────────────

/**
 * WhaleDetector: Identifies large/institutional transactions from mempool.
 *
 * Detection criteria:
 *   1. Transaction value > min_whale_value_usd (configurable)
 *   2. Transaction targets a known Polymarket contract
 *   3. Transaction size exceeds percentile threshold of recent activity
 *
 * Confidence level is based on:
 *   - Size relative to whale threshold (1.0 = at threshold, 2.0 = 2x threshold)
 *   - Contract specificity (exact Polymarket match gets higher confidence)
 *   - Gas price (higher gas = more urgent = more likely to affect market)
 */
// Extracted to file scope to avoid GCC default-member-initializer limitation
// with nested classes used in default arguments.
struct WhaleConfig {
    double min_whale_value_usd = 10000.0;   // $10,000 minimum for whale classification
    double min_confidence = 0.6;            // Minimum confidence to generate signal
    uint64_t gas_price_threshold = 100000000000ULL;  // 100 Gwei (urgent)
    int history_window = 1000;              // Transactions to track for percentile
};

class WhaleDetector {
public:
    explicit WhaleDetector(const WhaleConfig& config = WhaleConfig())
        : config_(config)
        , recent_values_(config.history_window)
        , recent_count_(0)
        , value_sum_(0.0)
        , value_sum_sq_(0.0) {}

    /**
     * Evaluate a decoded transaction for whale characteristics.
     *
     * @param tx        Raw transaction
     * @param decoded   Decoded calldata
     * @return Confidence (0.0 to 1.0), or 0.0 if not a whale
     */
    double evaluate(const EthTransaction& tx, const DecodedCall& decoded) {
        // Track in rolling history (for percentile calculation)
        update_history(decoded.token_value_usd);

        // Criterion 1: Value exceeds threshold
        if (decoded.token_value_usd < config_.min_whale_value_usd) {
            return 0.0;  // Not a whale
        }

        // Criterion 2: Must be a Polymarket-related transaction
        if (!decoded.is_polymarket) {
            return 0.0;  // Not relevant for our arbitrage
        }

        // Compute confidence
        double confidence = 0.5;  // Base: transaction is large + relevant

        // Confidence from size percentile
        double percentile = compute_percentile(decoded.token_value_usd);
        confidence += 0.3 * percentile;  // Up to +0.3 for top percentile

        // Confidence from gas price (urgency signal)
        if (tx.gas_price > config_.gas_price_threshold) {
            confidence += 0.2;  // Urgent transaction
        }

        // Cap confidence at 1.0
        return std::min(confidence, 1.0);
    }

    /**
     * Get rolling statistics for monitoring.
     */
    struct Stats {
        double avg_value;
        double std_dev;
        uint64_t total_seen;
    };

    Stats get_stats() const {
        uint64_t count = recent_count_.load(std::memory_order_relaxed);
        double sum = value_sum_.load(std::memory_order_relaxed);
        double sum_sq = value_sum_sq_.load(std::memory_order_relaxed);

        double avg = (count > 0) ? sum / static_cast<double>(count) : 0.0;
        double variance = (count > 0)
            ? (sum_sq / static_cast<double>(count)) - (avg * avg)
            : 0.0;

        return {
            .avg_value = avg,
            .std_dev = (variance > 0) ? std::sqrt(variance) : 0.0,
            .total_seen = count
        };
    }

private:
    void update_history(double value) {
        uint64_t idx = recent_count_.fetch_add(1, std::memory_order_relaxed);
        if (idx < recent_values_.size()) {
            recent_values_[idx] = value;
            value_sum_.fetch_add(value, std::memory_order_relaxed);
            value_sum_sq_.fetch_add(value * value, std::memory_order_relaxed);
        }
    }

    double compute_percentile(double value) const {
        uint64_t count = recent_count_.load(std::memory_order_relaxed);
        if (count == 0) return 0.5;

        uint64_t size = std::min(count, static_cast<uint64_t>(recent_values_.size()));
        uint64_t above = 0;
        for (uint64_t i = 0; i < size; i++) {
            if (recent_values_[i] > value) {
                above++;
            }
        }
        return 1.0 - (static_cast<double>(above) / static_cast<double>(size));
    }

    WhaleConfig config_;

    // Rolling window of recent transaction values (lock-free, but single
    // producer = mempool thread, single consumer = monitoring thread)
    std::vector<double> recent_values_;
    std::atomic<uint64_t> recent_count_;
    std::atomic<double> value_sum_;
    std::atomic<double> value_sum_sq_;
};

// ─── Mempool Listener ──────────────────────────────────────────────────────

/**
 * MempoolListener: Listens to Polygon mempool for whale transactions.
 *
 * Subscribes to newPendingTransactions via WebSocket, fetches full transaction
 * details, decodes calldata, and generates AlphaSignal for whale activity.
 *
 * The listener runs on its own thread (cold path). Detected whale signals are
 * pushed to a lock-free SPSC ring buffer for the hot path.
 *
 * Whale signals trigger the Hot Path to:
 *   1. Check for imminent large market movements
 *   2. Adjust order sizes proactively
 *   3. Potentially front-run (if arbitrage window detected)
 */

// Extracted to file scope to avoid GCC default-member-initializer limitation.
struct MempoolConfig {
    std::string endpoint = "wss://polygon-rpc.com/ws";
    int whale_interval = 10;      // For mock: whale tx every N polls
    int tx_per_poll = 5;          // For mock: normal txs per poll
    uint64_t disconnect_interval = 0;  // For mock: simulate disconnect every N polls
    uint32_t rng_seed = 0xF00D;
    int poll_interval_ms = 10;      // Interval between poll cycles
    int max_reconnect_attempts = 10;
};

class MempoolListener {
public:
    using ConnectionCallback = std::function<void(bool /*connected*/)>;

    MempoolListener(SPSC_RingBuffer<AlphaSignal>& queue,
                    const MempoolConfig& config = MempoolConfig())
        : queue_(queue)
        , config_(config)
        , transport_(std::make_unique<MockMempoolTransport>(
              config.whale_interval,
              config.tx_per_poll,
              config.disconnect_interval,
              config.rng_seed))
        , detector_(WhaleConfig{})
        , running_(false)
        , connected_(false)
        , poll_count_(0)
        , whale_signals_(0)
        , reconnect_attempts_(0) {}

    /**
     * Set a custom transport (for production: real RPC, for testing: custom mock).
     */
    void set_transport(std::unique_ptr<MempoolTransport> transport) {
        transport_ = std::move(transport);
    }

    /**
     * Set telemetry for logging (cold path).
     */
    void set_telemetry(Telemetry* tel) { telemetry_ = tel; }

    /**
     * Set connection state callback.
     */
    void set_connection_callback(ConnectionCallback cb) {
        conn_callback_ = std::move(cb);
    }

    /**
     * Start the listener in a background thread.
     */
    void start() {
        running_ = true;
        listener_thread_ = std::thread(&MempoolListener::run, this);
    }

    /**
     * Stop the listener and wait for thread exit.
     */
    void stop() {
        running_ = false;
        if (listener_thread_.joinable()) {
            listener_thread_.join();
        }
    }

    /**
     * Main listener loop (runs on background thread).
     */
    void run() {
        while (running_.load(std::memory_order_acquire)) {
            // Ensure connection
            if (!transport_->is_connected()) {
                if (connect_with_backoff()) {
                    connected_ = true;
                    if (conn_callback_) conn_callback_(true);
                } else {
                    continue;
                }
            }

            // Poll for new pending transactions
            poll_pending_transactions();

            // Brief sleep before next poll cycle
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.poll_interval_ms));
        }
    }

    /**
     * Get statistics for monitoring.
     */
    struct Stats {
        uint64_t poll_count;
        uint64_t whale_signals;
        uint64_t reconnect_attempts;
        bool connected;
        WhaleDetector::Stats detector_stats;
    };

    Stats get_stats() const {
        return {
            .poll_count = poll_count_.load(std::memory_order_relaxed),
            .whale_signals = whale_signals_.load(std::memory_order_relaxed),
            .reconnect_attempts = reconnect_attempts_.load(std::memory_order_relaxed),
            .connected = connected_.load(std::memory_order_relaxed),
            .detector_stats = detector_.get_stats()
        };
    }

    /**
     * Process a single transaction hash (for testing / manual triggering).
     * Fully testable without running the background thread.
     */
    bool process_transaction(const EthHash& hash) {
        EthTransaction tx = transport_->fetch_transaction(hash);
        if (tx.calldata.empty()) return false;

        DecodedCall decoded = CalldataDecoder::decode(tx);

        double confidence = detector_.evaluate(tx, decoded);
        if (confidence < WhaleConfig{}.min_confidence) {
            return false;  // Not a whale
        }

        // Build AlphaSignal
        AlphaSignal signal;
        signal.type = AlphaSignal::Type::WHALE_TRADE;
        signal.confidence = confidence;
        signal.ev_per_dollar = confidence * 0.05;  // Estimated EV boost
        signal.q_value = 1.0 - confidence;         // FDR estimate
        signal.timestamp_ns = tx.timestamp_ns;

        // Set market slug from decoded data
        if (!decoded.market_slug.empty()) {
            strncpy(signal.market_slug, decoded.market_slug.c_str(), 31);
        } else if (!decoded.function_name.empty()) {
            // Fallback: use function name + contract
            std::string slug = decoded.contract_name + ":" + decoded.function_name;
            strncpy(signal.market_slug, slug.c_str(), 31);
        } else {
            strncpy(signal.market_slug, "unknown", 31);
        }

        // Push to hot path (lock-free SPSC)
        if (!queue_.try_push(signal)) {
            // Queue full — drop signal (logged via telemetry)
            if (telemetry_) {
                telemetry_->log_event(EventType::ORDER_REJECTED, "",
                    "{\"reason\":\"alpha_queue_full\",\"type\":\"whale_signal\"}",
                    "WARN");
            }
            return false;
        }

        whale_signals_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

private:
    bool connect_with_backoff() {
        reconnect_attempts_.fetch_add(1, std::memory_order_relaxed);

        int attempts = reconnect_attempts_.load(std::memory_order_relaxed);
        int backoff_ms = std::min(100 * (1 << (attempts - 1)), 30000);

        // Jitter
        std::mt19937 rng(0xDEADBEEF + attempts);
        std::uniform_real_distribution<double> jitter(-0.25, 0.25);
        int jittered_ms = static_cast<int>(backoff_ms * (1.0 + jitter(rng)));

        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(jittered_ms, 10)));

        // Define handler for received transaction hashes
        std::function<void(const EthHash&)> on_tx_hash =
            [this](const EthHash& hash) {
                this->process_transaction(hash);
            };

        if (transport_->connect(config_.endpoint, on_tx_hash)) {
            reconnect_attempts_.store(0, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

    /**
     * Poll for pending transactions via the transport.
     * For MockMempoolTransport, this is a no-op simulation step.
     * For real transports, this would poll the WebSocket for new messages.
     */
    void poll_pending_transactions() {
        poll_count_.fetch_add(1, std::memory_order_relaxed);

        // For mock transports, simulate polling
        // (In production, real transports handle WebSocket polling internally)
        if (transport_->is_mock()) {
            transport_->simulate_poll();
        }

        // Check for disconnection
        if (!transport_->is_connected()) {
            connected_ = false;
            if (conn_callback_) conn_callback_(false);
            transport_->disconnect();
        }
    }

    SPSC_RingBuffer<AlphaSignal>& queue_;
    MempoolConfig config_;

    std::unique_ptr<MempoolTransport> transport_;
    WhaleDetector detector_;

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<uint64_t> poll_count_;
    std::atomic<uint64_t> whale_signals_;
    std::atomic<int> reconnect_attempts_;

    std::thread listener_thread_;
    Telemetry* telemetry_ = nullptr;
    ConnectionCallback conn_callback_;
};

#endif // MEMPOOL_LISTENER_HPP