# ADR-0001: Bot Scope — Discrete Execution, NOT Market-Making

| Field | Value |
|-------|-------|
| **Status** | Proposed |
| **Date** | 2026-01-22 |
| **Author** | AUD-ARCH |
| **Deciders** | AXIOM (architect), AUD-SEC, PO (product) |
| **Related cards** | OBS-05 |

## Context

The project documentation is ambiguous about the bot's trading scope. README.md describes "envía órdenes firmadas" (sending signed orders) suggesting discrete execution, while ARCHITECTURE_MAP.md mentions "LadderSkew" and "16-order pool construction" which implies continuous market-making.

## Decision

The bot performs **DISCRETE EXECUTION**, not continuous market-making.

### What the bot DOES:
- Consumes alpha signals via HTTP (1 signal per tick)
- Builds a **single bid/ask quote pair** per signal using LadderBuilder
- Submits **one order** (BUY or SELL) per signal
- Uses TTL (TimeStrategy) for order expiration, not for quote refresh cadence

### What the bot does NOT do:
- Does NOT maintain a continuous bid/ask spread
- Does NOT cancel/replace quotes at high frequency
- Does NOT manage a multi-level order book
- Does NOT do classic market-making (quote-driven liquidity provision)

## Terminology clarification

| Term | Correct meaning | Incorrect usage |
|------|-----------------|-----------------|
| **Ladder** | Quote skew based on conviction + inventory + OFI | "LadderSkew" (implies continuous ladder) |
| **Quote** | Single bid/ask pair per signal | "Quote management" (implies continuous) |
| **TTL** | Order expiration (UTC hour) | Quote refresh interval |
| **Skew** | Price offset from mid based on conviction | Continuous market-making skew |

## Consequences

### Positive
- Simpler mental model
- Reduced regulatory exposure (not a market maker)
- Simpler compliance (no quote obligations)
- Lower operational risk (no continuous quote management)

### Negative
- Documentation inconsistency remains until updated
- "Ladder" terminology in code requires renaming or clarification

## Action items

1. Update README.md — clarify "discrete execution"
2. Update ARCHITECTURE.md — remove market-making terminology
3. Update ARCHITECTURE_MAP.md — clarify "Ladder = single pair, not continuous"
4. ADR-0002: Compliance (document that discrete execution reduces obligations)

## Metrics

- Hot path: 1 quote pair per signal, NOT continuous
- Order rate: bounded by signal arrival rate, NOT by quote refresh cadence
