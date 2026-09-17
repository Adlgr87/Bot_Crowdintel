# 🏗️ WORKFLOW MAESTRO: Bot de Ultra-Baja Latencia para Polymarket

> **Arquitectura determinista | Hot Path C++/Rust | CLOB V2 nativo | Integración CrowdIntel PRR**

---

## 📊 RESUMEN EJECUTIVO

| Métrica | Objetivo | Métrica Clave |
|---------|----------|---------------|
| **Tick-to-Wire** | &lt; 50 µs (hot path) | Incluye: red → procesamiento → firma → wire |
| **Wire-to-Block** | &lt; 500 ms (optimizable) | Envío RPC → mempool → inclusión en bloque |
| **Frecuencia máxima** | 10,000 órdenes/s | Limitado por CLOB y nonce management |
| **Jitter (desviación)** | &lt; 5 µs (99th percentile) | Aislamiento CPU + kernel tuning |

---

## 🎯 FASE 1: Topología de Infraestructura y Adquisición de Alpha

### 1.1 Co-ubicación y Servidor Bare-Metal

| Requisito | Especificación | Justificación |
|-----------|----------------|---------------|
| **Ubicación** | Amsterdam (eu-west-3) | RTT al CLOB &lt; 1ms según benchmarks TradingVPS |
| **Hardware** | AWS c7i.metal o bare-metal equivalente | ENA Express + CPU dedicada, sin "noisy neighbors" |
| **CPU** | Intel Xeon Sapphire Rapids (o superior) | Microcódigos optimizados para trading, C0.2 idle states |
| **NIC** | ENA Express (AWS) o Solarflare XtremeScale | Latencia &lt; 5 µs en kernel estándar |
| **Memoria** | DDR5 ECC, 32GB mínimo | Para lock-free SPSC buffers + libros de órdenes L2 |

### 1.2 Configuración del Kernel (Linux 6.18+ o PREEMPT_RT)

```bash
# /etc/default/grub
GRUB_CMDLINE_LINUX_DEFAULT="quiet splash isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 \
  intel_pstate=disable intel_idle.max_cstate=1 processor.max_cstate=1 \
  nohz_full=2,3 selinux=0"

# /etc/sysctl.d/99-lowlatency.conf
net.core.rmem_max = 134217728
net.core.wmem_max = 134217728
net.core.rmem_default = 262144
net.core.wmem_default = 262144
net.core.netdev_max_backlog = 5000
net.core.somaxconn = 65535
net.ipv4.tcp_congestion_control = bbr2
net.ipv4.tcp_low_latency = 1
net.ipv4.tcp_no_metrics_save = 1
net.ipv4.tcp_mtu_probing = 1
net.ipv4.tcp_slow_start_after_idle = 0
net.core.busy_poll = 50
net.core.busy_read = 50

# Aplicar
sysctl -p /etc/sysctl.d/99-lowlatency.conf
update-grub
```

### 1.3 Conexión a CrowdIntel (Alpha de Insiders)

**Arquitectura de ingestión de datos de CrowdIntel:**

```
                    CrowdIntel MCP Server
                          │
                    (SSE/HTTP + Webhooks)
                          │
            ┌─────────────┴──────────────┐
            │                          │
      [Webhook Receiver]     [Polling Worker]
      (Event-triggered)      (Backup every 30s)
            │                          │
            ▼                          ▼
    ┌──────────────────┐   ┌──────────────────┐
    │  Alpha Parser    │   │  Alpha Validator │
    │  - JSON → Binary │   │  - Checks FDR q-  │
    │  - SPSC Queue    │   │    values       │
    └──────────────────┘   │  - Filters by    │
            │              │    confidence    │
            └──────────────▶  - EV/score      │
                             └──────────────────┘
                                    │
                        [lock-free SPSC Queue → Hot Path]
```

**Configuración de webhooks de CrowdIntel:**

```python
# alpha_receiver.py - Webhook endpoint para alertas de insiders
from fastapi import FastAPI, Request
import time

app = FastAPI()
# SPSC lock-free queue
alpha_queue = Queue(maxsize=1024)

@app.post("/webhook/crowdintel")
async def receive_alpha(request: Request):
    """
    Recibe alertas de CrowdIntel vía webhook
    - Insider signals (whale trades)
    - Funding cluster alerts  
    - Flawless machine detection
    - Copyable human signals
    """
    payload = await request.json()
    
    signal_type = payload.get("type")
    market_slug = payload.get("market")
    confidence = payload.get("confidence", 0.0)
    ev_per_dollar = payload.get("ev_per_dollar", 0.0)
    q_value = payload.get("q_value", 0.0)
    
    # Filtro estadístico (estilo CrowdIntel FDR)
    if q_value > 0.05:  # 5% false discovery rate
        return {"status": "rejected", "reason": "q_value too high"}
    
    if confidence < 0.85:
        return {"status": "rejected", "reason": "confidence too low"}
    
    if ev_per_dollar < 0.02:
        return {"status": "rejected", "reason": "ev_per_dollar too low"}
    
    # Encola para el hot path
    alpha_queue.put_nowait({
        "type": signal_type,
        "market": market_slug,
        "confidence": confidence,
        "ev": ev_per_dollar,
        "timestamp_ns": time.time_ns()
    })
    
    return {"status": "accepted"}
```

### 1.4 Configuración de Red y WebSocket

```python
# config.py
import os

# Polymarket CLOB V2
CLOB_HOST = os.getenv("POLYMARKET_CLOB_HOST", "https://clob-v2.polymarket.com")
CLOB_WS = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
RTDS_WS = "wss://ws-live-data.polymarket.com"  # Chainlink BTC/USD

# Polygon RPC (proveedor de baja latencia)
POLYGON_RPC = os.getenv("POLYGON_RPC_URL")
# Alternativa MEV: FastLane relay
USE_PRIVATE_RELAY = True

# TCP_NODELAY para WebSocket
WS_TCP_NODELAY = True
WS_CONNECT_TIMEOUT_MS = 3000
WS_READ_TIMEOUT_MS = 100  # Agresivo
```

---

## 🔥 FASE 2: Pipeline de Datos y Motor de Estrategia

### 2.1 Order Book L2 Lock-Free (C++)

```cpp
// orderbook.cpp
#include <atomic>
#include <array>
#include <cstdint>
#include <memory>

template<typename T, size_t N>
class SPSC_RingBuffer {
    std::unique_ptr<T[]> buffer_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
    static constexpr size_t mask_ = N - 1;
    
public:
    SPSC_RingBuffer() : buffer_(std::make_unique<T[]>(N)), head_(0), tail_(0) {}
    
    inline bool write(T* item) {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t next = (h + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buffer_[h] = std::move(*item);
        head_.store(next, std::memory_order_release);
        return true;
    }
    
    inline bool read(T& item) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        item = buffer_[t];
        tail_.store((t + 1) & mask_, std::memory_order_release);
        return true;
    }
};

struct Level2Entry {
    uint64_t price;      // Precio × 1e6 (fixed point)
    uint64_t size;       // Tamaño en pUSD × 1e6
    uint64_t timestamp;  // Nanosegundos (RDTSCP)
} __attribute__((packed));

class OrderBookL2 {
private:
    static constexpr size_t MAX_LEVELS = 100;
    std::array<Level2Entry, MAX_LEVELS> bids_;
    std::array<Level2Entry, MAX_LEVELS> asks_;
    std::atomic<uint64_t> sequence_number_;
    char padding_[56];  // Cache line padding
    
public:
    inline Level2Entry* get_bid(uint32_t level) {
        return level < MAX_LEVELS ? &bids_[level] : nullptr;
    }
    
    inline Level2Entry* get_ask(uint32_t level) {
        return level < MAX_LEVELS ? &asks_[level] : nullptr;
    }
};
```

### 2.2 Integración de Alpha Signals

```rust
// alpha_engine.rs
use crossbeam::queue::SegQueue;

pub struct AlphaSignal {
    pub signal_type: String,
    pub market_slug: String,
    pub confidence: f64,
    pub ev_per_dollar: f64,
    pub q_value: f64,
    pub timestamp_ns: u128,
}

pub struct AlphaEngine {
    signal_queue: SegQueue<AlphaSignal>,
    min_confidence: f64,      // 0.85
    min_ev_per_dollar: f64,   // 0.02
    max_q_value: f64,         // 0.05
}

impl AlphaEngine {
    pub fn new() -> Self {
        Self {
            signal_queue: SegQueue::new(),
            min_confidence: 0.85,
            min_ev_per_dollar: 0.02,
            max_q_value: 0.05,
        }
    }
    
    pub fn process_signal(&self, signal: AlphaSignal) -> Option<TradeDecision> {
        if signal.q_value > self.max_q_value { return None; }
        if signal.confidence < self.min_confidence { return None; }
        if signal.ev_per_dollar < self.min_ev_per_dollar { return None; }
        
        let kelly_fraction = calculate_kelly(signal.ev_per_dollar, signal.confidence);
        
        Some(TradeDecision {
            market_slug: signal.market_slug.clone(),
            size_usd: kelly_fraction * MAX_POSITION_USD,
            direction: if signal.ev_per_dollar > 0.0 { Side::Buy } else { Side::Sell },
            timestamp_ns: signal.timestamp_ns,
        })
    }
}
```

### 2.3 Estrategia TWAP Mean-Reversion

```rust
// strategy_tw_twap_scalping.rs
pub struct TWAPMeanReversion {
    twap_window_seconds: u32,       // 60 (settlement de Polymarket)
    reversal_threshold: f64,        // Z-score > 2.0σ
    max_position_usd: f64,          // $10,000
    max_slippage_bps: i32,          // 5 bps
    price_history: Vec<f64>,
    twap: f64,
    std_dev: f64,
}

impl TradingStrategy for TWAPMeanReversion {
    fn evaluate(&self, book: &OrderBookL2, alpha: Option<&AlphaSignal>) -> Option<Order> {
        let current_twap = self.calculate_twap(book);
        let z_score = (book.mid_price() - current_twap) / self.std_dev;
        
        if z_score.abs() > self.reversal_threshold {
            let alpha_confidence = alpha.map(|a| a.confidence).unwrap_or(0.5);
            let kelly = calculate_fractional_kelly(
                self.expected_value(z_score.abs(), alpha_confidence)
            );
            
            let net_ev = self.ev_after_slippage(kelly, z_score, alpha_confidence);
            
            // Optimizar por EV, no por win rate (estilo CrowdIntel)
            if net_ev > 0.02 {  // 2¢ EV/USD
                return Some(Order {
                    price: self.calculate_optimal_price(z_score, book),
                    size: kelly * self.max_position_usd,
                    side: if z_score > 0.0 { Side::Sell } else { Side::Buy },
                    timestamp_ns: self.get_rdtscp_timestamp(),
                });
            }
        }
        None
    }
}
```

---

## ⚡ FASE 3: Hot Path de Ejecución (Tick-to-Wire)

### 3.1 Diagrama de Flujo Tick-to-Wire

```
┌─────────────────────────────────────────────────────────────────────┐
│                        HOT PATH (μs target < 50)                    │
├─────────────────────────────────────────────────────────────────────┤
│  [NIC] → [Packet Parser] → [OrderBook Update] → [Strategy]         │
│    │         │              │                    │                  │
│    │       0.1µs          0.5µs            1-3µs (C++/Rust)       │
│    │                                                                    │
│    └────────────────────────────────────────────────────────────────    │
│                    [Strategy Decision]                                │
│                │                  │                                  │
│                │               ~5µs (lock-free SPSC)                 │
│                ▼                  ▼                                  │
│        [Position Sizing: Kelly]   [Order Builder]                    │
│              │                      │                              │
│          ~2µs (CPU intrinsics)   ~3µs (zero-alloc)                  │
│              │                      │                              │
├─────────────────────────────────────────────────────────────────┤
│                   [EIP-712 Signing (Rust)]                       │
│         - Domain separator (cached)                              │
│         - k1/k2 precomputation (cached)                          │
│         - ECDSA signature generation                             │
│                    ~15-25µs total                                  │
├─────────────────────────────────────────────────────────────────┤
│                    [Wire Transmission]                            │
│         - TCP_NODELAY socket                                      │
│         - Kernel bypass (DPDK opcional)                           │
│                    ~2-5µs (kernel) / ~2µs (DPDK)                  │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 Implementación Rust EIP-712 (15-25 µs)

```rust
// eip712_signer.rs
use secp256k1::{Secp256k1, SecretKey};
use tiny_keccak::{Hasher, Keccak};

pub struct FastSigner {
    secp: Secp256k1<secp256k1::All>,
    secret_key: SecretKey,
    public_key: PublicKey,
}

impl FastSigner {
    pub fn sign_order(&self, params: &OrderParams) -> [u8; 65] {
        // 1. Domain separator (cached) - 0.5µs
        static DOMAIN: OnceLock<[u8; 32]> = OnceLock::new();
        
        // 2. Hash order struct - 1-2µs
        let struct_hash = {
            let mut keccak = Keccak::v256();
            keccak.update(b"Order(...)");
            keccak.update(&params.salt.to_be_bytes());
            keccak.update(&params.maker);
            // ... rest of fields
            let mut output = [0u8; 32];
            keccak.finalize(&mut output);
            output
        };
        
        // 3. EIP-712 hash - 0.5µs
        let mut keccak = Keccak::v256();
        keccak.update(b"\x19\x01");
        keccak.update(&DOMAIN.get_or_init(...));
        keccak.update(&struct_hash);
        
        // 4. ECDSA sign - 10-15µs
        self.secp.sign_ecdsa_recoverable(
            &SecretKey::from_slice(&hash).unwrap(),
            &self.secret_key
        )
    }
}
```

### 3.3 Nonce Manager y feeRateBps Dinámico

```rust
// nonce_manager.rs
use std::sync::atomic::{AtomicU64, Ordering};

pub struct NonceManager {
    address_nonces: DashMap<[u8; 20], AtomicU64>,
    base_timestamp: u64,
}

impl NonceManager {
    /// V2 usa timestamp (no counter como V1)
    pub fn get_unique_timestamp(&self) -> u64 {
        let now_ms = self.base_timestamp + 
            (SystemTime::now().duration_since(UNIX_EPOCH).as_millis() % 1_000_000) as u64;
        let _rdtsc = __rdtscp();
        
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let counter = COUNTER.fetch_add(1, Ordering::Relaxed);
        
        now_ms + (counter % 1000)
    }
    
    /// feeRateBps dinámico (maker-only)
    pub fn calculate_fee_rate(&self, liquidity: f64, size: f64) -> u16 {
        // Maker fee: 0.01% (10 bps) - ajuste dinámico si liquidez insuficiente
        if liquidity < size * 0.01 {
            return 15.min(50);
        }
        10
    }
}
```

### 3.4 Cliente HTTP Lightweight (No SDK)

```rust
// lightweight_client.rs
pub struct LightweightCLOBClient {
    host: String,
    api_key: String,
    secret: String,
    passphrase: String,
    pool: Vec<Connection>,
}

impl LightweightCLOBClient {
    /// POST /v2/order - Latencia < 500µs (excluyendo red)
    pub async fn submit_order(&mut self, order: &SignedOrder) -> OrderResponse {
        let start = Instant::now();
        
        // 1. Serialize binary (no JSON)
        let payload = serialize_order_raw(order);
        
        // 2. HMAC auth header
        let timestamp = get_timestamp_ms();
        let signature = hmac_sign(&self.secret, &format!("{timestamp}POST/v2/order{payload}"));
        
        // 3. Build request (stack allocated)
        let request = http::Request::builder()
            .method("POST")
            .uri(format!("https://{}/v2/order", self.host))
            .header("X-API-Key", &self.api_key)
            .header("X-Passphrase", &self.passphrase)
            .header("X-Signature", &signature)
            .header("X-Timestamp", &timestamp.to_string())
            .header("Content-Type", "application/json")
            .body(payload)
            .unwrap();
        
        let response = self.send_with_pool(request).await?;
        
        tracing::debug!("Order submit latency: {:?}", start.elapsed());
        response
    }
}
```

---

## 🧪 FASE 4: Simulación y Backtesting

### 4.1 Backtester L2 con Tick Replay

```python
# backtest_engine.py
import numpy as np
from dataclasses import dataclass
from typing import List, Tuple
import logging

@dataclass
class TickData:
    timestamp_ns: int
    bids: List[Tuple[float, float]]
    asks: List[Tuple[float, float]]
    trades: List[Tuple[float, float, str]]

class L2Backtester:
    """
    Backtester con replay de ticks L2 para simular slippage real.
    Basado en PR&R: TWAP settlement 60s, L2 order book depth.
    """
    
    def __init__(self, market_data: List[TickData], capital: float = 10000.0):
        self.ticks = market_data
        self.capital = capital
        self.maker_fee_bps = 10   # 0.01% maker
        self.taker_fee_bps = 20   # 0.02% taker
        self.trade_log = []
    
    def simulate_order_fill(self, price: float, size: float, 
                           book: TickData, side: str) -> Tuple[float, float]:
        """
        Simula slippage real basado en orden book L2.
        NO asume fills al mid-price.
        """
        filled_price = 0.0
        filled_size = 0.0
        remaining = size
        
        # Order book traversal
        if side == "buy":
            levels = sorted(book.asks, key=lambda x: x[0])
        else:
            levels = sorted(book.bids, key=lambda x: x[0], reverse=True)
        
        for level_price, level_size in levels:
            if remaining <= 0: break
            take = min(remaining, level_size)
            filled_price += level_price * take
            filled_size += take
            remaining -= take
        
        if filled_size > 0:
            return filled_price / filled_size, filled_size
        return price, 0.0
    
    def detect_self_fill(self, window_ms: int = 100) -> float:
        """
        Detecta "MM-ing yourself" en horas de baja liquidez.
        CRITICAL: evitar buys/sells simultáneos que se canjean entre sí.
        """
        self_fills = 0
        for i in range(len(self.trade_log) - 1):
            t1, t2 = self.trade_log[i], self.trade_log[i + 1]
            if t2['timestamp'] - t1['timestamp'] <= window_ms * 1_000_000:
                if t1['side'] != t2['side']:
                    self_fills += 1
                    logging.warning(f"Self-fill at {t1['timestamp']}")
        return self_fills / len(self.trade_log) if self.trade_log else 0.0
```

### 4.2 Tests de Regresión de Latencia

```python
# test_latency_regression.py
import pytest
import time
import statistics
from hot_path import HotPath

class TestLatencyRegression:
    
    @pytest.fixture
    def hot_path(self):
        return HotPath()
    
    def test_tick_to_order_p99(self, hot_path):
        """99% de operaciones < 50µs"""
        latencies = []
        
        for _ in range(10_000):
            tick = generate_mock_tick()
            start = time.perf_counter_ns()
            hot_path.process(tick)
            elapsed = (time.perf_counter_ns() - start) / 1000
            latencies.append(elapsed)
        
        p99 = statistics.quantiles(latencies, n=100)[98]
        assert p99 < 50.0, f"P99: {p99:.2f}µs > 50µs"
        
        jitter = statistics.stdev(latencies)
        assert jitter < 5.0, f"Jitter: {jitter:.2f}µs too high"
    
    def test_signature_latency(self, hot_path):
        """Firma EIP-712 < 25µs median"""
        latencies = []
        for _ in range(1000):
            start = time.perf_counter_ns()
            hot_path.sign_order(mock_order())
            elapsed = (time.perf_counter_ns() - start) / 1000
            latencies.append(elapsed)
        
        median = statistics.median(latencies)
        assert median < 15.0, "Median signature > 15µs"
```

---

## 🛡️ FASE 5: Gestión de Riesgo y Resiliencia

### 5.1 Circuit Breakers

```rust
// risk_manager.rs
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};

pub struct RiskManager {
    max_daily_loss: f64,
    max_order_value: f64,
    max_slippage_bps: i32,
    circuit_open: AtomicBool,
    daily_pnl: AtomicI64,
}

impl RiskManager {
    pub fn pre_trade_check(&self, order_value: f64, 
                          slippage_bps: i32) -> Result<(), RiskError> {
        if self.circuit_open.load(Ordering::Relaxed) {
            return Err(RiskError::CircuitOpen);
        }
        
        if order_value > self.max_order_value {
            return Err(RiskError::OrderTooLarge);
        }
        
        if slippage_bps > self.max_slippage_bps {
            return Err(RiskError::SlippageExceeded);
        }
        
        let loss = self.daily_pnl.load(Ordering::Relaxed) as f64 / 100.0;
        if loss > self.max_daily_loss {
            self.circuit_open.store(true, Ordering::Relaxed);
            return Err(RiskError::DailyLossLimit);
        }
        
        Ok(())
    }
}
```

### 5.2 Detección de Anomalías en el Feed

```rust
// feed_monitor.rs
use std::time::Instant;
use std::time::Duration;

pub struct FeedMonitor {
    last_heartbeat: Instant,
    heartbeat_timeout: Duration,  // 6 segundos (especificación)
    jitter_stats: JitterStats,
}

impl FeedMonitor {
    pub fn check_heartbeat(&self) -> Result<(), FeedError> {
        if self.last_heartbeat.elapsed() > Duration::from_secs(6) {
            tracing::error!("Feed heartbeat timeout!");
            return Err(FeedError::HeartbeatTimeout);
        }
        Ok(())
    }
    
    pub fn validate_sequence(&self, seq: u64, expected: u64) -> bool {
        if seq != expected {
            tracing::warn!("Sequence gap: expected {}, got {}", expected, seq);
            return false;
        }
        true
    }
}
```

### 5.3 Logging Asíncrono y Auditoría

```rust
// telemetry.rs
use std::sync::mpsc;
use serde::{Serialize, Deserialize};

#[derive(Serialize, Deserialize)]
pub struct AuditEvent {
    timestamp_ns: u128,
    event_type: EventType,
    correlation_id: String,
    data: serde_json::Value,
    hot_path_latency_us: Option<f64>,
}

pub struct AsyncLogger {
    sender: mpsc::SyncSender<AuditEvent>,
    _thread: Option<JoinHandle<()>>,
}

impl AsyncLogger {
    pub fn new(log_dir: &str) -> Self {
        let (sender, receiver) = mpsc::sync_channel(16384);
        
        let thread = std::thread::spawn(move || {
            let mut file = OpenOptions::new()
                .append(true)
                .create(true)
                .open(format!("{}/audit.log", log_dir))
                .unwrap();
            
            for event in receiver {
                let json = serde_json::to_string(&event).unwrap();
                writeln!(file, "{}", json).ok();
            }
        });
        
        Self { sender, _thread: Some(thread) }
    }
    
    #[inline(always)]
    pub fn log(&self, event: AuditEvent) {
        let _ = self.sender.try_send(event);
    }
}
```

---

## 📊 MATRIZ DE LATENCIAS OBJETIVO

| Componente | Objetivo | Real (estimada) | Prioridad |
|------------|----------|-----------------|-----------|
| **Tick-to-Process** | 0.5 µs | 0.1-0.5 µs (ENA Express) | ⭐⭐⭐⭐⭐ |
| **OrderBook Update** | 0.5 µs | 0.2-0.5 µs | ⭐⭐⭐⭐⭐ |
| **Strategy Eval** | 3 µs | 1-3 µs | ⭐⭐⭐⭐⭐ |
| **Position Sizing** | 2 µs | 1-2 µs | ⭐⭐⭐⭐ |
| **Order Building** | 3 µs | 2-3 µs | ⭐⭐⭐⭐ |
| **EIP-712 Signing** | 25 µs | 15-25 µs | ⭐⭐⭐⭐⭐ |
| **Wire Transmission** | 5 µs | 2-5 µs | ⭐⭐⭐⭐⭐ |
| **RPC Submit** | 500 µs | 200-500 µs | ⭐⭐⭐⭐ |
| **Mempool → Block** | Variable | 100ms-2s | ⭐⭐⭐ |

---

## 🚦 Checklist por Fase

### Fase 1 ✅
- [ ] Bare-metal en Amsterdam (eu-west-3)
- [ ] Kernel: isolcpus, PREEMPT_RT, TCP_NODELAY
- [ ] Webhook receiver para CrowdIntel
- [ ] WebSocket CLOB V2 + RTDS conectados
- [ ] SPSC Ring Buffers implementados

### Fase 2 ✅
- [ ] Order book L2 lock-free en C++/Rust
- [ ] Alpha engine con filtro FDR q-value
- [ ] Estrategia TWAP Mean-Reversion
- [ ] Kelly Criterion para sizing
- [ ] Slippage model integrado

### Fase 3 ✅
- [ ] Hot path con LTO + PGO
- [ ] EIP-712 signer Rust (15-25µs)
- [ ] Nonce manager con timestamp único
- [ ] Cliente HTTP lightweight (sin SDK)
- [ ] Connection pooling activo

### Fase 4 ✅
- [ ] L2 backtester con tick replay
- [ ] Slippage model validado
- [ ] Tests de regresión latencia
- [ ] Self-fill detection
- [ ] Benchmark suite

### Fase 5 ✅
- [ ] Circuit breakers implementados
- [ ] Feed anomaly detection (>6s heartbeat)
- [ ] Async logger auditoría
- [ ] Jitter metrics monitoreados
- [ ] Dashboard de telemetría

---

## ⚠️ Consideraciones Adversarial

### API Fallback
1. **`/events` vs `/markets` desincronización**: Validar que `condition_id` coincidan. No asumir sinonimidad.
2. **Rate limit 429**: Exponential backoff con jitter: `delay = base * (2^attempt) + random(0, 0.1)`
3. **FOK orders en mercados condicionales**: Validar `order_type` antes de enviar.

### Seguridad
- **Private key**: Nunca en hot path. Usar HSM si disponible.
- **API keys**: Rotar cada 90 días. Secret manager.
- **feeRateBps**: Maker-only. Nunca ser taker activamente.

---

*Workflow integrado con CrowdIntel (insider signals, FDR q-values) y Poly Research & Robotics (TWAP settlement, L2 backtesting, EIP-712 optimization)*

<!-- HINDSIGHT:SAVE_MEMORY --><!-- HINDSIGHT_SAVE:{"category":"bot-architecture","content":"Polymarket bot architecture: C++/Rust hot path, CrowdIntel alpha integration, TWAP mean-reversion strategy, EIP-712 signing in Rust (15-25µs), SPSC lock-free ring buffers. Target Tick-to-Wire <50µs. FDR q-value filter for signals. Self-fill detection in backtester. CLOB V2 migration required (April 28, 2026 cutover)."} --><tool_call>terminal<arg_key>command</arg_key><arg_value>wc -l "/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/WORKFLOW_MAESTRO_POLYMARKET.md" && echo "✅ Archivo creado correctamente"