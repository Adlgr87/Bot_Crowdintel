#!/usr/bin/env python3
"""
Daily report for CrowdIntel canary deployment.
Generates a summary of PnL, win rate, Sharpe, drawdown, and BB calibration.

Usage: python3 scripts/daily_report.py [--journal /var/log/crowdintel/journal.ndjson]
"""
import json
import sys
import argparse
from pathlib import Path
from datetime import datetime, timezone

def main():
    ap = argparse.ArgumentParser(description="Daily PnL report")
    ap.add_argument("--journal", default="/var/log/crowdintel/journal.ndjson")
    ap.add_argument("--output", default=None)
    args = ap.parse_args()

    journal_path = Path(args.journal)
    if not journal_path.exists():
        print(f"⚠️  Journal not found: {journal_path}")
        print("   (Bot hasn't run yet or journal path is misconfigured.)")
        return 0

    trades = []
    brier_scores = []
    ece_bins = {f"{i/10:.1f}-{(i+1)/10:.1f}": {"count": 0, "correct": 0} for i in range(10)}

    with open(journal_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue

            kind = entry.get("type", "")
            if kind == "trade":
                trades.append(entry)
                # BB calibration check
                p_up = entry.get("bb_p_up", 0.5)
                outcome = entry.get("outcome_pnl", 0.0)
                was_up = 1 if outcome > 0 else 0
                brier = (p_up - was_up) ** 2
                brier_scores.append(brier)

                # Binned ECE
                bin_idx = min(int(p_up * 10), 9)
                bin_key = f"{bin_idx/10:.1f}-{(bin_idx+1)/10:.1f}"
                if bin_key in ece_bins:
                    ece_bins[bin_key]["count"] += 1
                    ece_bins[bin_key]["correct"] += was_up

    if not trades:
        print("📊 No trades in journal today.")
        return 0

    # PnL
    total_pnl = sum(t.get("net_pnl_usd", 0.0) for t in trades)
    wins = [t for t in trades if t.get("net_pnl_usd", 0) > 0]
    losses = [t for t in trades if t.get("net_pnl_usd", 0) <= 0]
    win_rate = len(wins) / len(trades) if trades else 0.0

    # PnL curve for max drawdown
    cumulative = 0
    peak = 0
    max_dd = 0.0
    pnls = []
    for t in sorted(trades, key=lambda x: x.get("timestamp", 0)):
        cumulative += t.get("net_pnl_usd", 0.0)
        peak = max(peak, cumulative)
        dd = (peak - cumulative) / peak if peak > 0 else 0.0
        max_dd = max(max_dd, dd)
        pnls.append(cumulative)

    # Sharpe (assume 0% risk-free)
    import statistics
    if len(pnls) > 1 and len(set(pnls)) > 1:
        std = statistics.stdev(pnls)
        sharpe = (sum(pnls) / len(pnls)) / std if std > 0 else 0.0
    else:
        sharpe = 0.0

    # Brier score
    avg_brier = sum(brier_scores) / len(brier_scores) if brier_scores else 0.0

    # ECE
    ece = 0.0
    total_count = sum(b["count"] for b in ece_bins.values())
    if total_count > 0:
        for bin_key, b in ece_bins.items():
            if b["count"] > 0:
                bin_mid = (int(bin_key.split("-")[0]) + 0.05)
                accuracy = b["correct"] / b["count"]
                ece += abs(bin_mid - accuracy) * b["count"] / total_count

    report = {
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
        "trades": len(trades),
        "wins": len(wins),
        "losses": len(losses),
        "win_rate": round(win_rate, 4),
        "total_pnl_usd": round(total_pnl, 2),
        "sharpe_ratio": round(sharpe, 4),
        "max_drawdown": round(max_dd, 4),
        "brier_score": round(avg_brier, 4),
        "ece": round(ece, 4),
        "max_dd_ok": max_dd < 0.05,
        "winrate_ok": win_rate > 0.52,
        "sharpe_ok": sharpe > 1.0,
    }

    output = json.dumps(report, indent=2)
    if args.output:
        Path(args.output).write_text(output)
        print(f"✅ Report written to {args.output}")

    print(output)
    return 0

if __name__ == "__main__":
    sys.exit(main())
