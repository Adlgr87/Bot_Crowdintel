# Scope Analysis — OBS-05: Market-making ambiguity

## Code analysis of LadderBuilder

### File: core/src/ladder_builder.hpp

**Function: `build()` (líneas 53-121)**

The LadderBuilder constructs a **single bid/ask quote pair** per invocation:

```cpp
Quote build(double p_up, double market_mid, double ofi_direction,
            double ofi_multiplier, double inventory,
            double spread_base, double vol_multiplier) const noexcept
{
    Quote q{};
    
    q.is_active = (ofi_multiplier > 0.0);
    
    double skew = bb_skew * 0.5 + ofi_skew * 0.2 + inv_skew * 0.3;
    double bid_price = market_mid - half * (1.0 - skew);
    double ask_price = market_mid + half * (1.0 + skew);
    
    double bid_size = base_size * ofi_multiplier * (1.0 + skew);
    double ask_size = base_size * ofi_multiplier * (1.0 - skew);
    
    return q;  // SINGLE pair, not a book
}
```

### Key observations:

1. **Single pair output:** Returns ONE Quote with ONE bid_price, ONE ask_price, ONE bid_size, ONE ask_size. NOT a multi-level order book.

2. **No cancel/replace loop:** The build() function is called once per tick (when a signal arrives). There is NO continuous quote management loop that cancels and replaces orders.

3. **No continuous market-making:** There's no logic for:
   - Maintaining a persistent bid/ask spread
   - Canceling stale quotes
   - Replacing quotes as market moves
   - Managing a quote inventory

4. **TTL via TimeStrategy:** TimeStrategy (time_strategy.hpp) sets TTL per UTC hour, but this is for order expiration, not for quote refresh cadence.

### Execution flow:

```
run_tick()
  → engine_extensions::run_dsh_tick()
    → LadderBuilder::build()  ← ONE quote pair
    → PresignedOrderPool::acquire_at_most()  ← consume presigned slot
    → OrderGateway/OrderGateway::submit()  ← SINGLE order
```

### Conclusion:

The bot performs **discrete execution** — it builds a single quote pair when a signal arrives and places orders. It does **NOT** perform **continuous market-making** (cancel/replace at high frequency).

**Documentation inconsistency:**

- README.md: "envía órdenes firmadas" (sends signed orders) — DISCRETE
- ARCHITECTURE_MAP.md line 53: "LadderSkew ... 16-order pool construction" — IMPLIES market-making
- README line 140: "LadderBuilder + TimeStrategy: quotes con TTL" — IMPLIES quote management

The term "Ladder" and "LadderSkew" in documentation creates ambiguity about whether the bot does market-making (continuous quote management) or discrete execution (single trade per signal).
