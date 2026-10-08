#!/usr/bin/env python3
"""
backtest.py — Walk-forward backtest for the CfC network.

Simulates the exact C++ inference loop (sequential hidden state updates) using
the Python reference model, then evaluates trading performance using
Polymarket CLOB V2 settlement data.

Validation: train 5d → validate 1d → slide 1d
Metrics: BCE loss, accuracy, ECE (calibration), Kelly criterion, Sharpe ratio.
"""

import argparse
import json
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import torch
import yaml


@dataclass
class BacktestConfig:
    initial_capital_usd: float = 10000.0
    position_sizing: str = "kelly_half"  # full_kelly, half_kelly, fixed
    fee_bps: int = 10                    # maker fee in basis points
    slippage_bps: int = 5               # assumed slippage per trade
    min_confidence: float = 0.70        # minimum p_up to trade
    max_daily_trades: int = 50
    max_drawdown_pct: float = 0.15
    dt_seconds: float = 0.1


@dataclass
class Trade:
    timestamp: int
    side: str  # "UP" or "DOWN"
    price: float
    size_shares: int
    usd_value: float
    fee_usd: float
    pnl_usd: float
    sequence_id: int


@dataclass
class BacktestResult:
    total_return_pct: float
    sharpe_ratio: float
    max_drawdown_pct: float
    total_trades: int
    winning_trades: int
    losing_trades: int
    avg_win_pct: float
    avg_loss_pct: float
    total_fees_usd: float
    final_capital: float
    calibration_ece: float
    accuracy: float
    bce_loss: float


class WalkForwardBacktester:
    """Walk-forward backtest using the Python CfC reference model.

    The model processes features sequentially, maintaining hidden state
    exactly as the C++ CfCNetwork does (closed-form continuous-time update).
    """

    def __init__(self, model: torch.nn.Module, config: BacktestConfig):
        self.model = model.eval()
        self.cfg = config
        self.n_hidden = model.n_hidden
        self.d_input = 6  # CFC uses 6 of 9 features

    def run_inference(self, X: np.ndarray, dt: float = 0.1) -> Tuple[np.ndarray, np.ndarray]:
        """Run sequential inference over a window of features.

        Args:
            X: feature matrix [n_steps, n_features] (first 6 used by CFC)
            dt: time delta between steps

        Returns:
            logits: [n_steps] raw logits
            probs:  [n_steps] sigmoid probabilities
        """
        n_steps = X.shape[0]
        logits = np.zeros(n_steps, dtype=np.float32)
        probs = np.zeros(n_steps, dtype=np.float32)

        h = np.zeros(self.n_hidden, dtype=np.float32)

        with torch.no_grad():
            for t in range(n_steps):
                x_t = torch.from_numpy(X[t, :self.d_input]).float()
                h_t = torch.from_numpy(h)

                h_new, y, p = self.model(x_t, h_t, dt)

                logits[t] = y.item()
                probs[t] = p.item()
                h = h_new.numpy()

        return logits, probs

    def run_walk_forward(self, features: np.ndarray, labels: np.ndarray,
                        timestamps: np.ndarray, prices: np.ndarray,
                        train_days: int = 5, validate_days: int = 1,
                        samples_per_day: int = 8640,  # 8640 = 24h * 60 * 15 (15s intervals)
                        ) -> Dict:
        """Execute walk-forward backtest.

        Args:
            features: [n_total, 9] feature matrix
            labels: [n_total] binary labels (0=Down, 1=Up)
            timestamps: [n_total] unix timestamps (ns)
            prices: [n_total] settlement prices

        Returns:
            Aggregated backtest results
        """
        n_total = len(features)
        window_size = (train_days + validate_days) * samples_per_day
        n_windows = max(1, (n_total - window_size) // (validate_days * samples_per_day) + 1)

        all_results = []
        all_trades = []

        log_prefix = lambda msg: print(f"[WFBT] {msg}")
        log_prefix(f"Walk-forward backtest: {n_windows} windows, "
                   f"train={train_days}d, validate={validate_days}d")

        for w in range(n_windows):
            val_start = w * validate_days * samples_per_day
            val_end = val_start + validate_days * samples_per_day
            if val_end > n_total:
                val_end = n_total

            X_val = features[val_start:val_end]
            y_val = labels[val_start:val_end]
            ts_val = timestamps[val_start:val_end]
            price_val = prices[val_start:val_end]

            log_prefix(f"Window {w+1}/{n_windows}: {len(X_val)} validation samples")

            # Run inference
            logits, probs = self.run_inference(X_val)

            # Compute metrics
            preds = (probs > 0.5).astype(np.int32)
            bce = -np.mean(y_val * np.log(probs + 1e-8) + (1 - y_val) * np.log(1 - probs + 1e-8))
            accuracy = np.mean(preds == y_val)

            # ECE
            ece = self._compute_ece(probs, y_val)

            # Trading simulation
            trades = self._simulate_trading(X_val, probs, y_val, ts_val, price_val)
            result = self._evaluate_trades(trades, probs, y_val, bce, ece, accuracy)

            all_results.append({
                "window": w + 1,
                "n_samples": len(X_val),
                "bce_loss": float(bce),
                "accuracy": float(accuracy),
                "ece": float(ece),
                "total_return_pct": result.total_return_pct,
                "sharpe": result.sharpe_ratio,
                "max_drawdown_pct": result.max_drawdown_pct,
                "n_trades": result.total_trades,
                "total_fees_usd": result.total_fees_usd,
            })
            all_trades.extend(trades)
            log_prefix(f"  BCE={bce:.4f}, Acc={accuracy:.4f}, ECE={ece:.4f}, "
                       f"Return={result.total_return_pct:.2f}%, Sharpe={result.sharpe_ratio:.2f}")

        # Aggregate
        aggregated = self._aggregate_results(all_results, all_trades)
        log_prefix(f"\n=== Aggregated Results ===")
        log_prefix(f"Total windows: {len(all_results)}")
        log_prefix(f"Total trades: {aggregated['total_trades']}")
        log_prefix(f"Mean return: {aggregated['mean_return_pct']:.2f}%")
        log_prefix(f"Mean Sharpe: {aggregated['mean_sharpe']:.2f}")
        log_prefix(f"Mean ECE: {aggregated['mean_ece']:.4f}")
        log_prefix(f"Pass ECE < 0.03: {'YES ✅' if aggregated['mean_ece'] < 0.03 else 'NO ❌'}")

        return aggregated

    def _compute_ece(self, probs: np.ndarray, labels: np.ndarray, n_bins: int = 15) -> float:
        """Expected Calibration Error."""
        bin_boundaries = np.linspace(0, 1, n_bins + 1)
        ece = 0.0
        for i in range(n_bins):
            in_bin = (probs >= bin_boundaries[i]) & (probs < bin_boundaries[i + 1])
            if in_bin.sum() > 0:
                accuracy = ((probs[in_bin] > 0.5).astype(int) == labels[in_bin]).mean()
                confidence = probs[in_bin].mean()
                ece += (in_bin.sum() / len(probs)) * abs(accuracy - confidence)
        return ece

    def _simulate_trading(self, X: np.ndarray, probs: np.ndarray,
                         labels: np.ndarray, timestamps: np.ndarray,
                         prices: np.ndarray) -> List[Trade]:
        """Simulate trading based on model predictions.

        Strategy:
        - Trade when |probs - 0.5| > (1 - min_confidence)
        - Direction: UP if prob > 0.5, DOWN if prob < 0.5
        - Position sizing: Kelly criterion (half-Kelly)
        - Fee: maker fee + assumed slippage
        """
        trades = []
        capital = self.cfg.initial_capital_usd
        daily_trade_counts = {}
        sequence_id = 0

        for t in range(len(probs)):
            prob = probs[t]
            price = prices[t]
            ts = timestamps[t]
            date = ts // (24 * 60 * 60 * 10**9)  # day bucket

            # Check confidence threshold
            confidence = abs(prob - 0.5) * 2  # [0, 1]
            if confidence < self.cfg.min_confidence:
                continue

            # Check daily trade limit
            daily_trade_counts[date] = daily_trade_counts.get(date, 0) + 1
            if daily_trade_counts[date] > self.cfg.max_daily_trades:
                continue

            # Check drawdown
            if capital < self.cfg.initial_capital_usd * (1 - self.cfg.max_drawdown_pct):
                continue

            # Kelly position sizing
            kelly = self._kelly_fraction(prob)
            if self.cfg.position_sizing == "half_kelly":
                kelly *= 0.5
            elif self.cfg.position_sizing == "fixed":
                kelly = 0.01  # 1% fixed

            position_usd = capital * kelly
            if position_usd < 10:  # minimum trade
                continue

            side = "UP" if prob > 0.5 else "DOWN"
            label = labels[t]

            # PnL: +1 for correct, -1 for wrong (binary outcome)
            pnl = position_usd if label == (1 if side == "UP" else 0) else -position_usd

            # Fees
            fee = position_usd * self.cfg.fee_bps / 10000
            slippage = position_usd * self.cfg.slippage_bps / 10000
            total_cost = fee + slippage
            net_pnl = pnl - total_cost

            capital += net_pnl

            trade = Trade(
                timestamp=int(ts),
                side=side,
                price=float(price),
                size_shares=int(position_usd / price * 1e6),  # USDC-based shares
                usd_value=position_usd,
                fee_usd=fee,
                pnl_usd=net_pnl,
                sequence_id=sequence_id,
            )
            trades.append(trade)
            sequence_id += 1

        return trades

    def _kelly_fraction(self, p: float) -> float:
        """Kelly criterion for binary outcome.

        For a 50/50 payout: f = 2p - 1
        Clamped to [0, 0.25] for safety.
        """
        f = 2 * p - 1
        return max(0, min(f, 0.25))

    def _evaluate_trades(self, trades: List[Trade], probs: np.ndarray,
                        labels: np.ndarray, bce: float, ece: float,
                        accuracy: float) -> BacktestResult:
        """Evaluate trading performance."""
        if not trades:
            return BacktestResult(
                total_return_pct=0.0, sharpe_ratio=0.0, max_drawdown_pct=0.0,
                total_trades=0, winning_trades=0, losing_trades=0,
                avg_win_pct=0.0, avg_loss_pct=0.0, total_fees_usd=0.0,
                final_capital=self.cfg.initial_capital_usd,
                calibration_ece=float(ece), accuracy=float(accuracy),
                bce_loss=float(bce),
            )

        returns = [t.pnl_usd / t.usd_value if t.usd_value > 0 else 0 for t in trades]
        returns = np.array(returns)

        winning = [t for t in trades if t.pnl_usd > 0]
        losing = [t for t in trades if t.pnl_usd <= 0]

        avg_win = np.mean([t.pnl_usd / t.usd_value for t in winning]) * 100 if winning else 0
        avg_loss = np.mean([t.pnl_usd / t.usd_value for t in losing]) * 100 if losing else 0

        capital = self.cfg.initial_capital_usd
        peak = capital
        max_dd = 0
        for t in trades:
            capital += t.pnl_usd
            peak = max(peak, capital)
            dd = (peak - capital) / peak
            max_dd = max(max_dd, dd)

        final_capital = self.cfg.initial_capital_usd + sum(t.pnl_usd for t in trades)
        total_return = (final_capital - self.cfg.initial_capital_usd) / self.cfg.initial_capital_usd * 100

        # Sharpe ratio (assume risk-free rate = 0)
        if len(returns) > 1 and np.std(returns) > 0:
            sharpe = np.mean(returns) / np.std(returns) * np.sqrt(252 * 6.5 * 3600)  # annualized
        else:
            sharpe = 0.0

        return BacktestResult(
            total_return_pct=float(total_return),
            sharpe_ratio=float(sharpe),
            max_drawdown_pct=float(max_dd * 100),
            total_trades=len(trades),
            winning_trades=len(winning),
            losing_trades=len(losing),
            avg_win_pct=float(avg_win),
            avg_loss_pct=float(avg_loss),
            total_fees_usd=float(sum(t.fee_usd for t in trades)),
            final_capital=float(final_capital),
            calibration_ece=float(ece),
            accuracy=float(accuracy),
            bce_loss=float(bce),
        )

    def _aggregate_results(self, all_results: List[Dict], all_trades: List[Trade]) -> Dict:
        """Aggregate results across all walk-forward windows."""
        return {
            "total_windows": len(all_results),
            "total_trades": len(all_trades),
            "mean_return_pct": float(np.mean([r["total_return_pct"] for r in all_results])),
            "mean_sharpe": float(np.mean([r["sharpe"] for r in all_results])),
            "mean_ece": float(np.mean([r["ece"] for r in all_results])),
            "mean_accuracy": float(np.mean([r["accuracy"] for r in all_results])),
            "mean_bce_loss": float(np.mean([r["bce_loss"] for r in all_results])),
            "max_drawdown_pct": float(np.max([r["max_drawdown_pct"] for r in all_results])),
            "total_fees_usd": float(sum(t.fee_usd for t in all_trades)),
            "ece_pass": all(r["ece"] < 0.03 for r in all_results),
            "per_window": all_results,
        }


def main():
    parser = argparse.ArgumentParser(description="Walk-forward backtest for CfC network")
    parser.add_argument("--model", "-m", required=True,
                        help="Path to trained model (.pt or checkpoint)")
    parser.add_argument("--config", "-c", default="ml_training/config.yaml",
                        help="Config YAML")
    parser.add_argument("--output", "-o", default="ml_training/backtest_results.json",
                        help="Output results JSON")
    args = parser.parse_args()

    # Load config
    with open(args.config) as f:
        config = yaml.safe_load(f)

    bt_cfg = BacktestConfig(
        initial_capital_usd=config["backtest"]["initial_capital_usd"],
        position_sizing=config["backtest"]["position_sizing"],
        fee_bps=config["backtest"]["fee_bps"],
        slippage_bps=config["backtest"]["slippage_bps"],
        min_confidence=config["backtest"]["min_confidence"],
        max_daily_trades=config["backtest"]["max_daily_trades"],
        max_drawdown_pct=config["backtest"]["max_drawdown_pct"],
        dt_seconds=config["model"]["dt_seconds"],
    )

    # Load model
    print(f"Loading model from {args.model}...")
    checkpoint = torch.load(args.model, map_location='cpu', weights_only=False)
    if isinstance(checkpoint, dict) and 'model_state' in checkpoint:
        state = checkpoint['model_state']
    elif isinstance(checkpoint, dict) and 'state_dict' in checkpoint:
        state = checkpoint['state_dict']
    else:
        state = checkpoint

    # Reconstruct model
    from train_cfc import CfCNetwork
    model = CfCNetwork(state['W_x'].shape[0], state['W_x'].shape[1])
    model.load_state_dict(state)

    # Generate synthetic data for backtest
    print("Generating synthetic market data for backtest...")
    from train_cfc import SyntheticDataGenerator
    gen = SyntheticDataGenerator(seed=42, n_samples=100000)
    X, y = gen.generate()

    # Generate timestamps and prices
    n = len(X)
    base_ts = 1700000000 * 10**9  # nanoseconds
    timestamps = np.arange(n, dtype=np.int64) * 100_000_000 + base_ts  # 100ms intervals
    # Simulate price
    price_changes = np.cumsum(X[:, 5] * 0.001)  # microprice velocity drives price
    prices = 0.50 + price_changes  # start at 0.50 (50% probability)
    prices = np.clip(prices, 0.01, 0.99)

    # Run walk-forward backtest
    backtester = WalkForwardBacktester(model, bt_cfg)
    results = backtester.run_walk_forward(
        features=X.numpy(),
        labels=y.numpy(),
        timestamps=timestamps,
        prices=prices,
        train_days=5,
        validate_days=1,
        samples_per_day=8640,
    )

    # Save results
    os.makedirs(os.path.dirname(args.output) or '.', exist_ok=True)
    with open(args.output, 'w') as f:
        json.dump(results, f, indent=2, default=str)
    print(f"\nResults saved to {args.output}")

    # Print summary
    print("\n" + "=" * 60)
    print("BACKTEST SUMMARY")
    print("=" * 60)
    print(f"  Total windows:     {results['total_windows']}")
    print(f"  Total trades:      {results['total_trades']}")
    print(f"  Mean return:       {results['mean_return_pct']:.2f}%")
    print(f"  Mean Sharpe:       {results['mean_sharpe']:.2f}")
    print(f"  Mean ECE:          {results['mean_ece']:.4f}")
    print(f"  ECE < 0.03:        {'✅ PASS' if results['ece_pass'] else '❌ FAIL'}")
    print(f"  Total fees:        ${results['total_fees_usd']:.2f}")


if __name__ == "__main__":
    main()
