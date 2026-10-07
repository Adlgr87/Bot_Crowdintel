// ─────────────────────────────────────────────────────────────────────────────
// window_shield.hpp — Phase 3: Window Lifecycle State Machine
//
// Defiende contra manipulación de liquidación en los últimos segundos.
// Transiciones de estado en función del tiempo restante de la ventana.
//
// ESTADOS (MERCADO 5m = 300s):
//   t=0-240s:  MAKER_PASSIVE    (spread normal, quote simétrico)
//   t=240-270s: MAKER_SKEWED     (si CfC > 0.6, skew quotes)
//   t=270-290s: DIRECTIONAL     (si CfC > 0.75, solo posición direccional)
//   t=290-300s: CLOSE_ONLY      (cancel all, solo cerrar inventario)
//   t=300s+:   HALTED            (5s cooldown, no órdenes)
//
// INVARIANTS:
//   - HOT path: state transition check < 200ns (timer arithmetic + lookup)
//   - FAIL CLOSED: stale timestamp → HALTED
//   - All timers use CLOCK_MONOTONIC from window_start_timestamp
//
// TODO(P3-T1): Implement state machine + transitions.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <atomic>

// ── Configuration ────────────────────────────────────────────────────────────
struct WindowShieldConfig {
    // Time thresholds (seconds into the window)
    uint32_t window_seconds;
    uint32_t skewed_start_sec;
    uint32_t directional_start_sec;
    uint32_t close_only_start_sec;
    uint32_t halt_duration_sec;

    // Conviction thresholds (CfC probability)
    float skewed_threshold;       // 0.6
    float directional_threshold;  // 0.75

    // Constructor for 5-minute windows
    static constexpr WindowShieldConfig BTC_5M() {
        return WindowShieldConfig{
            .window_seconds = 300,
            .skewed_start_sec = 240,
            .directional_start_sec = 270,
            .close_only_start_sec = 290,
            .halt_duration_sec = 5,
            .skewed_threshold = 0.6f,
            .directional_threshold = 0.75f
        };
    }

    // Constructor for 15-minute windows
    static constexpr WindowShieldConfig BTC_15M() {
        return WindowShieldConfig{
            .window_seconds = 900,
            .skewed_start_sec = 810,
            .directional_start_sec = 870,
            .close_only_start_sec = 890,
            .halt_duration_sec = 3,
            .skewed_threshold = 0.6f,
            .directional_threshold = 0.75f
        };
    }
};

// ── State Enum ───────────────────────────────────────────────────────────────
enum class ShieldState : uint8_t {
    MAKER_PASSIVE,     // Normal quoting, symmetric
    MAKER_SKEWED,      // Quote skewed toward conviction
    DIRECTIONAL,       // Directional only, no passive quotes
    CLOSE_ONLY,        // Cancel all, only close inventory
    HALTED             // Emergency, no orders, cooldown period
};

// ── State Query Result ───────────────────────────────────────────────────────
struct ShieldStateInfo {
    ShieldState state;
    uint32_t remaining_sec;
    uint32_t elapsed_sec;
    bool is_settlement_time;    // true during HALTED after window close
    const char* name() const noexcept;
};

// ── WindowShield ─────────────────────────────────────────────────────────────
// Single-market state machine. Hot-path safe (all O(1)).
class WindowShield {
public:
    explicit WindowShield(const WindowShieldConfig& cfg) : cfg_(cfg) {}

    // Set window start timestamp (called once at window open, from hot path)
    void set_window_start(uint64_t start_ns_monotonic) noexcept {
        window_start_ns_ = start_ns_monotonic;
        current_state_.store(ShieldState::MAKER_PASSIVE);
    }

    // Update: call every tick with current monotonic time.
    // cfc_confidence: [0, 1] from CfCNetwork.
    // Returns the active state and whether a transition occurred.
    ShieldState update(uint64_t now_ns, float cfc_confidence) noexcept;

    // Get current state (hot-path read, atomic load)
    ShieldState current_state() const noexcept {
        return current_state_.load(std::memory_order_acquire);
    }

    // Get full state info for strategy layer
    ShieldStateInfo snapshot(uint64_t now_ns) const noexcept;

    // Force reset (window closed, starting new window)
    void reset(uint64_t new_window_start_ns) noexcept {
        window_start_ns_ = new_window_start_ns;
        current_state_.store(ShieldState::MAKER_PASSIVE);
    }

    // Check if this is a new window (for CfC reset signal)
    bool should_reset_cfc(uint64_t now_ns) const noexcept;

    const char* state_name(ShieldState s) const noexcept;

private:
    WindowShieldConfig cfg_;
    uint64_t window_start_ns_ = 0;
    std::atomic<ShieldState> current_state_{ShieldState::MAKER_PASSIVE};

    ShieldState compute_state(uint32_t elapsed_sec,
                              float cfc_confidence) const noexcept;
};
