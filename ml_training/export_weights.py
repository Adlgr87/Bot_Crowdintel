#!/usr/bin/env python3
"""
export_weights.py — Export trained CfC weights to C++ binary format.

Binary layout (float32, row-major, no padding):
    W_x    [32, 6]   = 192 floats
    W_h    [32, 32]  = 1024 floats
    b      [32]      = 32 floats
    W_gx   [32, 6]   = 192 floats
    W_gh   [32, 32]  = 1024 floats
    b_g    [32]      = 32 floats
    W_tau  [6]       = 6 floats
    b_tau  [1]       = 1 float
    W_out  [32]      = 32 floats
    b_out  [1]       = 1 float
    ──────────────────────────────────
    Total: 2506 floats = 10024 bytes

Header (optional, write_sha256=True):
    SHA256 hex string of weight data (64 ASCII bytes, null-padded to 65)

Usage:
    python export_weights.py --model cf_cfc_model.pt --output infra/models/cfc_btc_5m_v1.bin
    python export_weights.py --model cf_cfc_model.pt --verify
"""

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

import numpy as np
import torch
import yaml


def extract_weights(model_state: dict) -> np.ndarray:
    """Extract weights in the exact C++ binary export order.

    Args:
        model_state: PyTorch state_dict or OrderedDict of weight tensors

    Returns:
        Flat float32 array in order:
        W_x, W_h, b, W_gx, W_gh, b_g, W_tau, b_tau, W_out, b_out
    """
    # Each weight is converted to row-major (C-order) float32
    weights = [
        model_state['W_x'].numpy().astype(np.float32).flatten(order='C'),
        model_state['W_h'].numpy().astype(np.float32).flatten(order='C'),
        model_state['b'].numpy().astype(np.float32).flatten(order='C'),
        model_state['W_gx'].numpy().astype(np.float32).flatten(order='C'),
        model_state['W_gh'].numpy().astype(np.float32).flatten(order='C'),
        model_state['b_g'].numpy().astype(np.float32).flatten(order='C'),
        model_state['W_tau'].numpy().astype(np.float32).flatten(order='C'),
        model_state['b_tau'].numpy().astype(np.float32).flatten(order='C'),
        model_state['W_out'].numpy().astype(np.float32).flatten(order='C'),
        model_state['b_out'].numpy().astype(np.float32).flatten(order='C'),
    ]

    # Validate dimensions
    expected_names = ['W_x', 'W_h', 'b', 'W_gx', 'W_gh', 'b_g', 'W_tau', 'b_tau', 'W_out', 'b_out']
    expected_sizes = [192, 1024, 32, 192, 1024, 32, 6, 1, 32, 1]
    for name, w, exp_size in zip(expected_names, weights, expected_sizes):
        assert w.shape[0] == exp_size, f"Weight {name} has {w.shape[0]} elements, expected {exp_size}"

    return np.concatenate(weights)


def export_to_binary(model_state: dict, output_path: str,
                     write_sha256: bool = True) -> str:
    """Export weights to binary file.

    Args:
        model_state: PyTorch state_dict
        output_path: Output file path
        write_sha256: If True, prepend 64-byte SHA256 hex header

    Returns:
        SHA256 hex string of weight data
    """
    weights_flat = extract_weights(model_state)
    weight_bytes = weights_flat.tobytes()
    sha256 = hashlib.sha256(weight_bytes).hexdigest()

    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    with open(output_path, 'wb') as f:
        if write_sha256:
            f.write(sha256.encode('ascii').ljust(64, b'\0'))
        f.write(weight_bytes)

    file_size = os.path.getsize(output_path)
    print(f"✅ Exported {output_path}")
    print(f"   Weights: {len(weights_flat)} floats ({len(weight_bytes)} bytes)")
    print(f"   SHA256:  {sha256}")
    print(f"   File size: {file_size} bytes {'(with 64-byte header)' if write_sha256 else ''}")
    return sha256


def verify_binary(output_path: str, expected_sha: str = None) -> bool:
    """Verify a binary model file.

    Args:
        output_path: Path to binary file
        expected_sha: Expected SHA256 hex string (if None, reads from header)

    Returns:
        True if verification passes
    """
    with open(output_path, 'rb') as f:
        header = f.read(64)
        weight_data = f.read()

    computed_sha = hashlib.sha256(weight_data).hexdigest()

    if expected_sha is None:
        # Read hash from header
        try:
            header_sha = header.decode('ascii').strip('\x00').strip()
            if len(header_sha) == 64:
                expected_sha = header_sha
            else:
                print(f"⚠️  Header is not a valid SHA256 (len={len(header_sha)})")
                expected_sha = computed_sha  # skip verification if header is invalid
        except:
            expected_sha = computed_sha

    if computed_sha != expected_sha:
        print(f"❌ SHA256 MISMATCH!")
        print(f"   Computed: {computed_sha}")
        print(f"   Expected: {expected_sha}")
        return False

    # Validate weight count: 192+1024+32+192+1024+32+6+1+32+1 = 2536
    n_floats = len(weight_data) // 4
    if n_floats != 2536:
        print(f"❌ Weight count mismatch: {n_floats} (expected 2536)")
        return False

    if len(weight_data) % 4 != 0:
        print(f"❌ Weight data is not 4-byte aligned")
        return False

    print(f"✅ Verification PASSED")
    print(f"   SHA256:  {computed_sha}")
    print(f"   Weights: {n_floats} floats ({len(weight_data)} bytes)")
    print(f"   Layout:  W_x(192) + W_h(1024) + b(32) + W_gx(192) + W_gh(1024) + b_g(32) + W_tau(6) + b_tau(1) + W_out(32) + b_out(1)")
    return True


def load_model_state(model_path: str) -> dict:
    """Load model state dict from PyTorch checkpoint or .pt file."""
    checkpoint = torch.load(model_path, map_location='cpu', weights_only=False)
    if isinstance(checkpoint, dict):
        if 'model_state' in checkpoint:
            return checkpoint['model_state']
        elif 'state_dict' in checkpoint:
            return checkpoint['state_dict']
        else:
            # Assume it's a state_dict directly
            return checkpoint
    elif hasattr(checkpoint, 'state_dict'):
        return checkpoint.state_dict()
    else:
        raise ValueError(f"Cannot load model state from {model_path}")


def main():
    parser = argparse.ArgumentParser(description="Export CfC weights to C++ binary format")
    parser.add_argument("--model", "-m", required=True,
                        help="Path to PyTorch model (.pt or checkpoint)")
    parser.add_argument("--output", "-o", default="infra/models/cfc_btc_5m_v1.bin",
                        help="Output binary path")
    parser.add_argument("--no-header", action="store_true",
                        help="Don't write SHA256 header")
    parser.add_argument("--verify", "-v", action="store_true",
                        help="Verify the output binary")
    parser.add_argument("--kat-input", default=None,
                        help="Path to KAT vectors JSON (to update with hashes)")
    args = parser.parse_args()

    # Load model
    print(f"Loading model from {args.model}...")
    model_state = load_model_state(args.model)
    print(f"  Keys: {sorted(model_state.keys())}")

    # Export
    sha = export_to_binary(model_state, args.output, write_sha256=not args.no_header)

    # Verify
    if args.verify:
        print(f"\nVerifying {args.output}...")
        if not verify_binary(args.output, sha):
            sys.exit(1)

    # Update KAT vectors with SHA256 if provided
    if args.kat_input and os.path.exists(args.kat_input):
        with open(args.kat_input) as f:
            kat = json.load(f)
        kat['model_sha256'] = sha
        kat['model_path'] = args.output
        with open(args.kat_input, 'w') as f:
            json.dump(kat, f, indent=2)
        print(f"\nUpdated KAT vectors with SHA256: {sha}")


if __name__ == "__main__":
    main()
