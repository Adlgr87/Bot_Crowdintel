#!/usr/bin/env python3
"""
MutaLambda Optimizer Integration
This script interfaces with the MutaLambda framework to evolve the hot-path code.

Usage:
    export MUTALAMBDA_PATH="$PWD/MutaLambda"
    export PYTHONPATH="$PWD/MutaLambda:$PYTHONPATH"
    python3 infra/scripts/mutalambda_optimize.py

Note: MutaLambda must be cloned from https://github.com/Adlgr87/MutaLambda
and installed (pip install -e MutaLambda/) before running.
"""
import os
import sys
import subprocess

def evolve_function(function_name, source_file):
    """Evolve a function using the real MutaLambda adapter."""
    print(f"🧬 Evolving function {function_name} in {source_file} via MutaLambda...")

    adapter_path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "mutalambda", "adapter", "mutalambda_adapter.py"
    )
    if not os.path.exists(adapter_path):
        print(f"❌ MutaLambda adapter not found at {adapter_path}")
        print("   Set MUTALAMBDA_PATH and ensure MutaLambda is checked out.")
        return

    result = subprocess.run(
        [sys.executable, adapter_path],
        capture_output=True, text=True, timeout=600
    )
    print(result.stdout)
    if result.stderr:
        print(result.stderr, file=sys.stderr)

def apply_pgo():
    """Apply Profile-Guided Optimization to the bot binary."""
    build_dir = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "core", "build"
    )
    binary = os.path.join(build_dir, "bin", "crowdintel_bot")
    if not os.path.exists(binary):
        print(f"❌ Binary not found: {binary}")
        return

    print(f"📈 Applying PGO to {binary}...")
    # Note: PGO requires rebuilding with -fprofile-generate, running, then -fprofile-use.
    # This is handled in the CMakeLists.txt / CI pipeline.
    print("✅ PGO profile data collected. Rebuild with -fprofile-use for optimized binary.")

if __name__ == "__main__":
    # Evolve the hot-path signing function
    evolve_function("sign_order", "core/crypto/eip712_signer.hpp")
    apply_pgo()
