# Canary Scaling Policy — Phase 6

## Principle
"Protege el capital primero. Gana segundo."
Capital is increased ONLY when explicit gate conditions are met via manual review.

## Phases

| Fase   | Bankroll  | Duration | Gate Conditions (all must pass)              | Manual Review |
|--------|-----------|----------|----------------------------------------------|---------------|
| Canary | $50       | 24h      | 0 crashes, edge ≥ 0.5%, BB calib ECE ≤ 0.05  | ✅ Required   |
| Phase 1| $100      | 72h      | Win rate > 52%, max DD < 3%, 0 anomalies     | ✅ Required   |
| Phase 2| $250      | 7d       | Sharpe > 1.0, OFI spikes → freeze verified   | ✅ Required   |
| Phase 3| $500      | 14d      | Sharpe > 1.2, max DD < 5%, latency p99 < 50μs| ✅ Required   |
| Max    | $500      | —        | LÍMITE HARD-CODED. No auto-escalation.        | N/A           |

> **Above $500: manual review by 2 senior engineers required.**

## Automated Rollback Triggers

| Metric                    | Threshold                      | Action                  |
|--------------------------|--------------------------------|------------------------|
| Daily PnL                | < -3% bankroll                 | HALT + alert + rollback |
| Daily PnL                | < -5% bankroll                 | HALT (critical)         |
| Consecutive losing windows| 3 in 1 hour                    | Pause 1 hour            |
| Win rate (24h)           | < 48%                          | HALT + rollback to mock |
| CircuitBreaker OPEN      | > 3 times in 1 hour OR > 60s  | HALT                    |
| Latency p99              | > 200μs for > 5 min           | Restart container       |
| Memory RSS               | > 100MB                       | HALT                    |

## Kill Switch
- **File-based**: `touch /tmp/crowdintel_kill` → next tick HALT
- **Signal-based**: `kill -SIGUSR1 <pid>` → graceful shutdown
- **API-based**: `POST /admin/kill` (if implemented)

## Rollback Procedure
1. Signal HALT (kill switch)
2. Close all open positions at market
3. `git revert <last_good_commit>`
4. Rebuild + redeploy
5. Preserve journal at `/var/log/crowdintel/` for post-mortem
