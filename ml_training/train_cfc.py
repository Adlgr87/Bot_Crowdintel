#!/usr/bin/env python3
"""
train_cfc.py — Offline training of the Closed-Form Continuous-time (CfC) network.

CfC model (Hasani et al., Nature Machine Intelligence, 2022):
    f(x, h) = tanh(W_x·x + W_h·h + b)
    g(x, h) = sigmoid(W_gx·x + W_gh·h + b_g)
    τ(x)    = softplus(W_τ·x + b_τ)          (> 0)
    h_new   = h + (f(x,h) - h) · (1 - exp(-Δt/τ(x))) · g(x,h)
    y       = W_out · h_new + b_out
    p_up    = sigmoid(y)

Training:
    - Loss: BCEWithLogits + Expected Calibration Error (ECE) penalty
    - Optimizer: AdamW (lr=1e-3, weight_decay=0.01)
    - Scheduler: Cosine annealing over T_max epochs
    - Walk-forward: train 5d → validate 1d → slide 1d
    - Early stopping: patience=10 epochs on validation loss
    - Oversample: last 15s of each window

This script also generates Known-Answer Test (KAT) vectors for cfc_network_test.hpp.
"""

import argparse
import hashlib
import json
import logging
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
import yaml

# ── Logging ────────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("train_cfc")

# ── Constants (must match C++ CfCConfig) ───────────────────────────────────────
N_HIDDEN = 32
D_INPUT = 6
DT_SECONDS = 0.1


# ─────────────────────────────────────────────────────────────────────────────
# CfC Network — PyTorch reference implementation
# ─────────────────────────────────────────────────────────────────────────────
class CfCNetwork(nn.Module):
    """Closed-Form Continuous-time Neural Network.

    Implements the exact same equations as core/src/cfc_network.hpp.
    All weight matrices use row-major layout (matching the C++ binary export).

    Weight layout:
        W_x:   [N_HIDDEN, D_INPUT]   — input modulation
        W_h:   [N_HIDDEN, N_HIDDEN]  — recurrent modulation
        b:     [N_HIDDEN]           — input bias
        W_gx:  [N_HIDDEN, D_INPUT]   — input gate
        W_gh:  [N_HIDDEN, N_HIDDEN]  — hidden gate
        b_g:   [N_HIDDEN]           — gate bias
        W_tau: [D_INPUT]            — time-constant input weights
        b_tau: scalar              — time-constant bias
        W_out: [N_HIDDEN]          — output projection
        b_out: scalar              — output bias
    """

    def __init__(self, n_hidden: int = N_HIDDEN, d_input: int = D_INPUT,
                 dt: float = DT_SECONDS):
        super().__init__()
        self.n_hidden = n_hidden
        self.d_input = d_input
        self.dt = dt
        self.tau_min = 1e-4
        self.tau_max = 100.0

        # Input modulation
        self.W_x = nn.Parameter(torch.empty(n_hidden, d_input))
        self.b = nn.Parameter(torch.zeros(n_hidden))

        # Recurrent modulation
        self.W_h = nn.Parameter(torch.empty(n_hidden, n_hidden))

        # Input gate (sigmoid)
        self.W_gx = nn.Parameter(torch.empty(n_hidden, d_input))
        self.W_gh = nn.Parameter(torch.empty(n_hidden, n_hidden))
        self.b_g = nn.Parameter(torch.zeros(n_hidden))

        # Time constant (softplus ensures > 0)
        self.W_tau = nn.Parameter(torch.empty(d_input))
        self.b_tau = nn.Parameter(torch.tensor(1.0))

        # Output
        self.W_out = nn.Parameter(torch.empty(n_hidden))
        self.b_out = nn.Parameter(torch.tensor(0.0))

        self.reset_parameters()

    def reset_parameters(self):
        """Initialize weights — same strategy as C++ reference."""
        # Xavier for recurrent, smaller for input
        for W in [self.W_x, self.W_gx, self.W_h, self.W_gh]:
            nn.init.xavier_uniform_(W)
        nn.init.uniform_(self.W_tau, -0.1, 0.1)
        # W_out: xavier_uniform_ on a 2D view
        with torch.no_grad():
            # Xavier uniform for W_out [H]
            fan_in = self.n_hidden
            bound = (6.0 / fan_in) ** 0.5
            self.W_out.uniform_(-bound, bound)
        nn.init.zeros_(self.b)
        nn.init.zeros_(self.b_g)
        nn.init.constant_(self.b_tau, 1.0)
        nn.init.zeros_(self.b_out)

    def forward(self, x: torch.Tensor, h: torch.Tensor,
                dt: Optional[float] = None) -> Tuple[torch.Tensor, torch.Tensor, torch.Tensor]:
        """Single-step inference.

        Args:
            x:  input features, shape [D_INPUT]
            h:  hidden state, shape [N_HIDDEN]
            dt: time delta (default: self.dt)

        Returns:
            h_new: updated hidden state [N_HIDDEN]
            y:     output logit (scalar)
            p_up:  sigmoid(y) probability
        """
        dt = dt if dt is not None else self.dt

        # f = tanh(W_x·x + W_h·h + b)
        a_f = F.linear(x, self.W_x, self.b) + F.linear(h, self.W_h)
        f = torch.tanh(a_f)

        # g = sigmoid(W_gx·x + W_gh·h + b_g)
        a_g = F.linear(x, self.W_gx, self.b_g) + F.linear(h, self.W_gh)
        g = torch.sigmoid(a_g)

        # τ = softplus(W_τ·x + b_τ) — shared across all neurons
        tau_raw = F.linear(x, self.W_tau.unsqueeze(0), self.b_tau).squeeze()
        tau = F.softplus(tau_raw)
        tau = torch.clamp(tau, self.tau_min, self.tau_max)

        # h_new = h + (f - h)·(1 - exp(-dt/τ))·g
        decay = 1.0 - torch.exp(-dt / tau)
        h_new = h + (f - h) * decay * g

        # y = W_out·h_new + b_out
        y = F.linear(h_new, self.W_out.unsqueeze(0), self.b_out).squeeze()

        # p_up = sigmoid(y)
        p_up = torch.sigmoid(y)

        return h_new, y, p_up


# ─────────────────────────────────────────────────────────────────────────────
# Loss functions
# ─────────────────────────────────────────────────────────────────────────────
def compute_ece(probabilities: torch.Tensor, labels: torch.Tensor,
                n_bins: int = 15) -> float:
    """Expected Calibration Error.

    ECE = Σ_b (n_b/N) · |accuracy_b - confidence_b|
    """
    bin_boundaries = torch.linspace(0, 1, n_bins + 1, device=probabilities.device)
    ece = 0.0
    for i in range(n_bins):
        in_bin = (probabilities >= bin_boundaries[i]) & (probabilities < bin_boundaries[i + 1])
        prop = in_bin.float().mean()
        if prop.item() > 0:
            accuracy = (probabilities[in_bin] > 0.5).float().eq(labels[in_bin].float()).float().mean()
            confidence = probabilities[in_bin].mean()
            ece += prop.item() * abs(accuracy.item() - confidence.item())
    return ece


class CfCLoss(nn.Module):
    """BCEWithLogits + ECE penalty."""

    def __init__(self, ece_threshold: float = 0.03, penalty_weight: float = 0.5,
                 ece_bins: int = 15):
        super().__init__()
        self.ece_threshold = ece_threshold
        self.penalty_weight = penalty_weight
        self.ece_bins = ece_bins
        self.bce = nn.BCEWithLogitsLoss()

    def forward(self, logits: torch.Tensor, labels: torch.Tensor) -> torch.Tensor:
        bce = self.bce(logits, labels.unsqueeze(1).float())
        probs = torch.sigmoid(logits)
        ece = compute_ece(probs, labels, self.ece_bins)
        penalty = torch.clamp(torch.tensor(ece) - self.ece_threshold, min=0.0)
        return bce + self.penalty_weight * penalty


# ─────────────────────────────────────────────────────────────────────────────
# Synthetic data generation (for development/testing)
# ─────────────────────────────────────────────────────────────────────────────
class SyntheticDataGenerator:
    """Generate synthetic market data matching the 9-feature schema.

    The CFC uses 6 of these 9 features:
        [0] ofi_norm, [1] trade_intensity, [2] spread_bps,
        [3] depth_imbalance, [4] microprice, [5] mid_velocity
        [6] realized_vol_1h  (training only, not in CFC)
        [7] funding_rate     (training only, not in CFC)
        [8] volume_zscore    (training only, not in CFC)
    """

    def __init__(self, seed: int = 42, n_samples: int = 100_000):
        self.rng = np.random.default_rng(seed)
        self.n_samples = n_samples

    def generate(self) -> Tuple[torch.Tensor, torch.Tensor]:
        """Generate synthetic features and labels.

        Returns:
            X: [n_samples, 9] float32 features
            y: [n_samples] {0,1} labels (1=Up)
        """
        X = np.zeros((self.n_samples, 9), dtype=np.float32)

        # Feature 0: OFI (Order Flow Imbalance) — mean-reverting around 0
        X[:, 0] = self.rng.standard_normal(self.n_samples) * 0.3

        # Feature 1: Trade intensity — correlated with market activity
        trend = self.rng.standard_normal(self.n_samples) * 0.002
        X[:, 1] = np.abs(self.rng.standard_normal(self.n_samples)) * 0.5 + trend

        # Feature 2: Spread in bps — mean ~5bps, std ~3bps
        X[:, 2] = np.abs(self.rng.standard_normal(self.n_samples)) * 3 + 5

        # Feature 3: Depth imbalance — bounded [-1, 1]
        X[:, 3] = np.clip(self.rng.standard_normal(self.n_samples) * 0.3, -1, 1)

        # Feature 4: Microprice deviation from mid (normalized)
        X[:, 4] = self.rng.standard_normal(self.n_samples) * 0.0002

        # Feature 5: Mid price velocity
        X[:, 5] = self.rng.standard_normal(self.n_samples) * 0.0001

        # Feature 6: Realized volatility (1h) — always positive
        X[:, 6] = np.abs(self.rng.standard_normal(self.n_samples)) * 0.02 + 0.005

        # Feature 7: Funding rate
        X[:, 7] = self.rng.standard_normal(self.n_samples) * 0.0001

        # Feature 8: Volume z-score
        X[:, 8] = self.rng.standard_normal(self.n_samples) * 1.5

        # Generate labels based on a non-linear combination of features
        # (simulating real market dynamics where Up probability depends on
        # multiple correlated factors)
        signal = (
            0.3 * X[:, 0]           # positive OFI → up
            - 0.15 * X[:, 2] / 10   # high spread → slight down (adverse selection)
            + 0.25 * X[:, 3]        # high depth imbalance → up
            + 0.15 * X[:, 5] * 5000  # positive velocity → up
            + 0.1 * X[:, 4] * 5000  # microprice above mid → up
            + 0.1 * X[:, 7] * 1000  # positive funding → up
            + 0.05 * X[:, 8]        # high volume z-score → up
        )
        # Add noise and normalize
        noise = self.rng.standard_normal(self.n_samples) * 0.5
        signal = signal + noise
        # Convert to probability via sigmoid, then sample
        prob = 1 / (1 + np.exp(-signal))
        y = (self.rng.random(self.n_samples) < prob).astype(np.int64)

        # Oversample last 15s of each window (every 100 samples ~ 10s at 10Hz)
        # Duplicate recent samples to emphasize recent signal
        window_size = 100
        oversample_factor = 2
        n_windows = self.n_samples // window_size
        X_aug = [X]
        y_aug = [y]
        for w in range(n_windows):
            start = w * window_size
            end = start + window_size
            recent_start = end - 15  # last 15 of each window
            if recent_start >= start:
                for _ in range(oversample_factor):
                    X_aug.append(X[recent_start:end].copy())
                    y_aug.append(y[recent_start:end].copy())
        X_final = np.concatenate(X_aug)
        y_final = np.concatenate(y_aug)

        # Shuffle
        idx = self.rng.permutation(len(X_final))
        return torch.from_numpy(X_final[idx]), torch.from_numpy(y_final[idx])


# ─────────────────────────────────────────────────────────────────────────────
# Walk-forward training
# ─────────────────────────────────────────────────────────────────────────────
def train_walk_forward(X: torch.Tensor, y: torch.Tensor,
                       config: dict, device: str = "cpu") -> Dict:
    """Walk-forward training: train 5d → validate 1d → slide 1d."""
    cfg = config["training"]
    model_cfg = config["model"]
    wfa_cfg = cfg["walk_forward"]

    n_samples = len(X)
    # Split into train/val (85/15 initially)
    train_end = int(n_samples * 0.85)
    X_train, X_val = X[:train_end], X[train_end:]
    y_train, y_val = y[:train_end], y[train_end:]

    results = {
        "folds": [],
        "best_val_loss": float("inf"),
        "best_state": None,
    }

    # For synthetic data, simulate time-based splits
    n_folds = max(1, (train_end - wfa_cfg["train_days"] * 288 * 6) // (wfa_cfg["slide_days"] * 288 * 6))
    n_folds = min(n_folds, 5)  # cap at 5 folds for speed

    fold_size = train_end // (n_folds + 2)
    train_size = wfa_cfg["train_days"] * fold_size // (wfa_cfg["train_days"] + wfa_cfg["validate_days"])

    for fold in range(n_folds):
        fold_start = fold * fold_size // n_folds if n_folds > 1 else 0
        fold_train_end = fold_start + train_size
        fold_val_end = fold_train_end + (fold_size - train_size)

        X_fold_train = X[fold_start:fold_train_end]
        y_fold_train = y[fold_start:fold_train_end]
        X_fold_val = X[fold_train_end:fold_val_end]
        y_fold_val = y[fold_train_end:fold_val_end]

        log.info(f"  Fold {fold+1}/{n_folds}: train={len(X_fold_train)}, val={len(X_fold_val)}")

        model = CfCNetwork(model_cfg["n_hidden"], model_cfg["d_input"], model_cfg["dt_seconds"])
        model = model.to(device)
        model.train()

        optimizer = torch.optim.AdamW(
            model.parameters(),
            lr=cfg["optimizer"]["lr"],
            weight_decay=cfg["optimizer"]["weight_decay"],
            betas=tuple(cfg["optimizer"]["betas"]),
        )
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=cfg["scheduler"]["T_max"])
        loss_fn = CfCLoss(
            ece_threshold=cfg["loss"]["ece_threshold"],
            penalty_weight=cfg["loss"]["penalty_weight"],
            ece_bins=cfg["loss"]["ece_bins"],
        )

        best_val_loss = float("inf")
        patience_counter = 0

        for epoch in range(wfa_cfg["max_epochs"]):
            # Training
            model.train()
            perm = torch.randperm(len(X_fold_train))
            batch_size = cfg["batch_size"]
            n_batches = max(1, len(X_fold_train) // batch_size)

            for b in range(n_batches):
                idx = perm[b * batch_size:(b + 1) * batch_size]
                x_batch = X_fold_train[idx][:, :6].to(device)  # first 6 features
                y_batch = y_fold_train[idx].to(device)

                # Simulate sequential inference (each step feeds next)
                h = torch.zeros(model.n_hidden, device=device)
                logits_sum = torch.zeros(len(x_batch), device=device)

                for t in range(len(x_batch)):
                    h_new, y_logit, _ = model(x_batch[t], h)
                    logits_sum += y_logit
                    h = h_new

                loss = loss_fn(logits_sum / len(x_batch), y_batch)
                optimizer.zero_grad()
                loss.backward()
                torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
                optimizer.step()

            scheduler.step()

            # Validation
            model.eval()
            with torch.no_grad():
                h = torch.zeros(model.n_hidden, device=device)
                logits_val = torch.zeros(len(X_fold_val), device=device)

                for t in range(len(X_fold_val)):
                    x_t = X_fold_val[t, :6].to(device)
                    h_new, y_logit, _ = model(x_t, h)
                    logits_val[t] = y_logit
                    h = h_new

                val_loss = loss_fn(logits_val, y_fold_val.to(device)).item()
                with torch.no_grad():
                    probs_val = torch.sigmoid(logits_val)
                    val_acc = ((probs_val > 0.5).long() == y_fold_val.to(device).long()).float().mean().item()
                    val_ece = compute_ece(probs_val, y_fold_val.to(device), cfg["loss"]["ece_bins"])

            if epoch % 10 == 0 or val_loss < best_val_loss:
                log.info(f"    Epoch {epoch}: val_loss={val_loss:.6f}, "
                        f"val_acc={val_acc:.4f}, ECE={val_ece:.4f}")

            if val_loss < best_val_loss - cfg["early_stopping"]["min_delta"]:
                best_val_loss = val_loss
                patience_counter = 0
                results["best_state"] = {k: v.clone() for k, v in model.state_dict().items()}
            else:
                patience_counter += 1
                if patience_counter >= wfa_cfg["early_stopping"]["patience"]:
                    log.info(f"    Early stopping at epoch {epoch}")
                    break

        fold_result = {
            "fold": fold + 1,
            "val_loss": val_loss,
            "val_acc": val_acc,
            "val_ece": val_ece,
            "best_val_loss": best_val_loss,
        }
        results["folds"].append(fold_result)

        if best_val_loss < results["best_val_loss"]:
            results["best_val_loss"] = best_val_loss

    # Final model from best fold
    if results["best_state"] is not None:
        model.load_state_dict(results["best_state"])
    results["model"] = model

    log.info(f"Walk-forward complete. Best val_loss: {results['best_val_loss']:.6f}")
    for f in results["folds"]:
        log.info(f"  Fold {f['fold']}: loss={f['val_loss']:.6f}, acc={f['val_acc']:.4f}, ece={f['val_ece']:.4f}")

    return results


# ─────────────────────────────────────────────────────────────────────────────
# Gradient-free training fallback (for environments without PyTorch autograd)
# ─────────────────────────────────────────────────────────────────────────────
def train_simple_gradient(model: CfCNetwork, X: torch.Tensor, y: torch.Tensor,
                          config: dict, n_steps: int = 1000) -> Dict:
    """Simple training using AdamW + BCE loss (no walk-forward for speed)."""
    cfg = config["training"]
    loss_fn = nn.BCEWithLogitsLoss()
    optimizer = torch.optim.AdamW(
        model.parameters(),
        lr=cfg["optimizer"]["lr"],
        weight_decay=cfg["optimizer"]["weight_decay"],
    )
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=cfg["scheduler"]["T_max"])

    batch_size = cfg["batch_size"]
    losses = []

    for step in range(n_steps):
        # Random batch
        idx = torch.randint(0, len(X), (batch_size,))
        # Simulate sequential inference over batch
        h = torch.zeros(model.n_hidden)
        logits = torch.zeros(batch_size)

        for t in range(batch_size):
            h_new, y_logit, _ = model(X[idx[t], :6], h)
            logits[t] = y_logit
            h = h_new

        loss = loss_fn(logits, y[idx].float())
        optimizer.zero_grad()
        loss.backward()
        optimizer.step()
        scheduler.step()
        losses.append(loss.item())

        if step % 100 == 0:
            acc = ((torch.sigmoid(logits) > 0.5).long() == y[idx].long()).float().mean().item()
            log.info(f"  Step {step}: loss={loss.item():.6f}, acc={acc:.4f}")

    return {"losses": losses, "model": model}


# ─────────────────────────────────────────────────────────────────────────────
# KAT (Known-Answer Test) vector generation
# ─────────────────────────────────────────────────────────────────────────────
def generate_kat_vectors(model: CfCNetwork, n_vectors: int = 10,
                         seed: int = 12345) -> List[Dict]:
    """Generate Known-Answer Test vectors for verification against C++ implementation.

    Each vector contains:
        - weights: all model weights as flat float32 lists
        - input:   6 input features
        - h_init:  32 initial hidden state values
        - dt:      time delta
        - y:       output logit (full precision)
        - p_up:    sigmoid(y)
        - h_new:   32 updated hidden state values

    The C++ test loads these weights and verifies infer() produces matching y and p_up
    within ±1e-6.
    """
    rng = np.random.default_rng(seed)
    model.eval()

    vectors = []

    # Extract all weights in the binary export order
    state = model.state_dict()
    weights_flat = torch.cat([
        state['W_x'].flatten(),        # [32, 6]
        state['W_h'].flatten(),        # [32, 32]
        state['b'].flatten(),          # [32]
        state['W_gx'].flatten(),       # [32, 6]
        state['W_gh'].flatten(),       # [32, 32]
        state['b_g'].flatten(),        # [32]
        state['W_tau'].flatten(),      # [6]
        state['b_tau'].flatten(),      # [1]
        state['W_out'].flatten(),      # [32]
        state['b_out'].flatten(),      # [1]
    ]).numpy().astype(np.float32)

    for i in range(n_vectors):
        # Random input features
        x = rng.standard_normal(D_INPUT).astype(np.float32) * 0.5

        # Random initial hidden state
        h_init = rng.standard_normal(N_HIDDEN).astype(np.float32) * 0.1

        # Random dt
        dt = rng.uniform(0.05, 0.2)

        # Run inference (matching C++ equations exactly)
        x_t = torch.from_numpy(x)
        h_t = torch.from_numpy(h_init.copy())
        dt_t = float(dt)

        with torch.no_grad():
            h_new, y, p_up = model(x_t, h_t, dt_t)

        vector = {
            "id": i,
            "dt": dt,
            "input": x.tolist(),
            "h_init": h_init.tolist(),
            "h_new_ref": h_new.numpy().astype(np.float32).tolist(),
            "y_ref": float(y.item()),
            "p_up_ref": float(p_up.item()),
            "weights_hex": weights_flat.tobytes().hex(),
        }
        vectors.append(vector)

        log.info(f"  KAT vector {i}: y={float(y.item()):.10f}, p_up={float(p_up.item()):.10f}")

    return vectors


# ─────────────────────────────────────────────────────────────────────────────
# Export weights to binary (matching C++ CfCWeights layout)
# ─────────────────────────────────────────────────────────────────────────────
def export_weights(model: CfCNetwork, path: str, write_sha: bool = True) -> str:
    """Export model weights to binary format matching C++ load_weights().

    Binary layout (float32, row-major):
        W_x, W_h, b, W_gx, W_gh, b_g, W_tau, b_tau, W_out, b_out

    If write_sha256=True, prepend 64-byte SHA256 hex string of the weight data.
    Returns the SHA256 hex string.
    """
    state = model.state_dict()

    # Convert each weight to row-major float32, in the exact export order
    weights = [
        state['W_x'].numpy().astype(np.float32).flatten(order='C'),      # [32, 6]
        state['W_h'].numpy().astype(np.float32).flatten(order='C'),      # [32, 32]
        state['b'].numpy().astype(np.float32).flatten(order='C'),        # [32]
        state['W_gx'].numpy().astype(np.float32).flatten(order='C'),     # [32, 6]
        state['W_gh'].numpy().astype(np.float32).flatten(order='C'),     # [32, 32]
        state['b_g'].numpy().astype(np.float32).flatten(order='C'),      # [32]
        state['W_tau'].numpy().astype(np.float32).flatten(order='C'),    # [6]
        state['b_tau'].numpy().astype(np.float32).flatten(order='C'),    # [1]
        state['W_out'].numpy().astype(np.float32).flatten(order='C'),     # [32]
        state['b_out'].numpy().astype(np.float32).flatten(order='C'),     # [1]
    ]

    weight_bytes = np.concatenate(weights).tobytes()

    # Compute SHA256 of weight data only (not the header)
    sha256_hash = hashlib.sha256(weight_bytes).hexdigest()

    os.makedirs(os.path.dirname(path), exist_ok=True)

    with open(path, 'wb') as f:
        if write_sha:
            # Write 64-byte SHA256 hex header (null-terminated, 64 chars + 1)
            # The C++ code reads 64 bytes and treats them as hex string
            f.write(sha256_hash.encode('ascii'))
        f.write(weight_bytes)

    log.info(f"Exported weights to {path} (SHA256: {sha256_hash})")
    log.info(f"  Total floats: {len(weight_bytes) // 4} ({len(weight_bytes)} bytes)")
    log.info(f"  With header: {len(weight_bytes) + 64} bytes")

    return sha256_hash


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Train CfC network for Polymarket")
    parser.add_argument("--config", default="ml_training/config.yaml",
                        help="Path to config YAML")
    parser.add_argument("--output", default="infra/models/cfc_btc_5m_v1.bin",
                        help="Output model path")
    parser.add_argument("--kat-output", default="ml_training/kat_vectors.json",
                        help="Output KAT vectors JSON")
    parser.add_argument("--simple-train", action="store_true",
                        help="Use simple gradient training (faster) instead of walk-forward")
    parser.add_argument("--n-steps", type=int, default=1000,
                        help="Training steps (simple mode)")
    parser.add_argument("--n-samples", type=int, default=100000,
                        help="Synthetic data samples")
    args = parser.parse_args()

    # Load config
    with open(args.config) as f:
        config = yaml.safe_load(f)

    torch.manual_seed(config["training"]["seed"])
    np.random.seed(config["training"]["seed"])

    # Generate synthetic data
    log.info("Generating synthetic market data...")
    gen = SyntheticDataGenerator(seed=config["training"]["seed"], n_samples=args.n_samples)
    X, y = gen.generate()
    log.info(f"  Generated {len(X)} samples (features: {X.shape[1]}, labels: {y.unique(return_counts=True)})")

    # Train
    model_cfg = config["model"]
    if args.simple_train:
        log.info("Training (simple gradient mode)...")
        model = CfCNetwork(model_cfg["n_hidden"], model_cfg["d_input"], model_cfg["dt_seconds"])
        result = train_simple_gradient(model, X, y, config, args.n_steps)
        model = result["model"]
    else:
        log.info("Training (walk-forward)...")
        result = train_walk_forward(X, y, config)
        model = result["model"]

    # Generate KAT vectors
    log.info("Generating KAT vectors...")
    kat_vectors = generate_kat_vectors(model, n_vectors=10, seed=12345)

    with open(args.kat_output, 'w') as f:
        json.dump({"vectors": kat_vectors}, f, indent=2)
    log.info(f"  Saved {len(kat_vectors)} KAT vectors to {args.kat_output}")

    # Export weights
    sha256 = export_weights(model, args.output, write_sha=True)

    # Verify hash
    log.info(f"Verifying exported weights...")
    with open(args.output, 'rb') as f:
        header = f.read(64)
        weight_data = f.read()
    computed_hash = hashlib.sha256(weight_data).hexdigest()
    if computed_hash == sha256:
        log.info(f"  SHA256 verification: PASS ({sha256})")
    else:
        log.error(f"  SHA256 verification: FAIL (computed={computed_hash}, stored={sha256})")
        return 1

    log.info("\n=== Training complete ===")
    log.info(f"Model: {args.output}")
    log.info(f"SHA256: {sha256}")
    log.info(f"KAT vectors: {args.kat_output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
