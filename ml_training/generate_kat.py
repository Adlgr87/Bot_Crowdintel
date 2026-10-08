#!/usr/bin/env python3
"""
KAT (Known-Answer Test) vector generator for CfC native inference.

Regenerates core/src/cfc_network_test.hpp from ml_training/kat_vectors.json.
Uses %.9g float format for exact float32 round-trip (no SHA256 mismatch).

Usage:
    python3 ml_training/generate_kat.py
"""
import os
import json
import numpy as np

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KAT_JSON = os.path.join(REPO_ROOT, "ml_training", "kat_vectors.json")
OUTPUT_H = os.path.join(REPO_ROOT, "core", "src", "cfc_network_test.hpp")

def main():
    import os

    REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    KAT_JSON = os.path.join(REPO_ROOT, "ml_training", "kat_vectors.json")
    OUTPUT_H = os.path.join(REPO_ROOT, "core", "src", "cfc_network_test.hpp")

    with open(KAT_JSON) as f:
        data = json.load(f)

    # Read SHA256 from model binary header
    model_path = os.path.join(REPO_ROOT, "infra", "models", "cfc_btc_5m_v1.bin")
    with open(model_path, "rb") as f:
        sha256 = f.read(64).decode("ascii").strip()

    vectors = data["vectors"]
    weights_hex = vectors[0]["weights_hex"]
    weights_bytes = bytes.fromhex(weights_hex)
    weights_np = np.frombuffer(weights_bytes, dtype=np.float32)

    offset = 0
    def take(n):
        nonlocal offset
        w = weights_np[offset:offset+n].copy()
        offset += n
        return w

    W_x = take(192)
    W_h = take(1024)
    b = take(32)
    W_gx = take(192)
    W_gh = take(1024)
    b_g = take(32)
    W_tau = take(6)
    b_tau = take(1)
    W_out = take(32)
    b_out = take(1)

    def fmt(w):
        return f"{float(w):.9g}f"

    lines = []
    lines.append("// ─────────────────────────────────────────────────────────────────────────────")
    lines.append("// cfc_network_test.hpp — Known-Answer Test vectors for CfC network")
    lines.append("// AUTO-GENERATED from ml_training/kat_vectors.json")
    lines.append("// Do not edit manually — regenerate via: python3 ml_training/generate_kat.py")
    lines.append("// ─────────────────────────────────────────────────────────────────────────────")
    lines.append("#pragma once")
    lines.append("#include <cstdint>")
    lines.append("#include <array>")
    lines.append("#include <cstring>")
    lines.append("")
    lines.append(f'static constexpr const char* CFC_KAT_SHA256 = "{sha256}";')
    lines.append("")
    lines.append("struct CfCKATVector {")
    lines.append("    alignas(32) float input[6];")
    lines.append("    alignas(32) float h_init[32];")
    lines.append("    float dt;")
    lines.append("    float y_ref;")
    lines.append("    float p_up_ref;")
    lines.append("    float h_new_ref[32];")
    lines.append("};")
    lines.append("")

    def write_array(name, arr):
        vals = ", ".join(fmt(v) for v in arr)
        lines.append(f"static constexpr float {name}[{len(arr)}] = {{{vals}}};")
        lines.append("")

    write_array("CFC_KAT_W_X", W_x)
    write_array("CFC_KAT_W_H", W_h)
    write_array("CFC_KAT_B", b)
    write_array("CFC_KAT_W_GX", W_gx)
    write_array("CFC_KAT_W_GH", W_gh)
    write_array("CFC_KAT_B_G", b_g)
    write_array("CFC_KAT_W_TAU", W_tau)
    lines.append(f"static constexpr float CFC_KAT_B_TAU = {fmt(b_tau[0])};")
    lines.append("")
    write_array("CFC_KAT_W_OUT", W_out)
    lines.append(f"static constexpr float CFC_KAT_B_OUT = {fmt(b_out[0])};")
    lines.append("")

    lines.append(f"// KAT test vectors")
    lines.append(f"static constexpr CfCKATVector CFC_KAT_VECTORS[{len(vectors)}] = {{")
    for v in vectors:
        input_str = ", ".join(fmt(x) for x in v["input"])
        h_init_str = ", ".join(fmt(x) for x in v["h_init"])
        h_new_str = ", ".join(fmt(x) for x in v["h_new_ref"])
        lines.append("    {")
        lines.append(f"        {{{input_str}}},")
        lines.append(f"        {{{h_init_str}}},")
        lines.append(f"        {fmt(v['dt'])},")
        lines.append(f"        {fmt(v['y_ref'])},")
        lines.append(f"        {fmt(v['p_up_ref'])},")
        lines.append(f"        {{{h_new_str}}}")
        lines.append("    },")
    lines.append("};")
    lines.append(f"static constexpr int CFC_KAT_N_VECTORS = {len(vectors)};")
    lines.append("")

    lines.append("// ── Helper: Load KAT weights into CfCNetwork ─────────────────────────────────")
    lines.append("inline void load_kat_weights(CfCNetwork& net) {")
    lines.append("    CfCWeights& w = net.weights_mut();")
    lines.append("    for (int i = 0; i < 192; i++) w.W_x[i] = CFC_KAT_W_X[i];")
    lines.append("    for (int i = 0; i < 1024; i++) w.W_h[i] = CFC_KAT_W_H[i];")
    lines.append("    for (int i = 0; i < 32; i++) w.b[i] = CFC_KAT_B[i];")
    lines.append("    for (int i = 0; i < 192; i++) w.W_gx[i] = CFC_KAT_W_GX[i];")
    lines.append("    for (int i = 0; i < 1024; i++) w.W_gh[i] = CFC_KAT_W_GH[i];")
    lines.append("    for (int i = 0; i < 32; i++) w.b_g[i] = CFC_KAT_B_G[i];")
    lines.append("    for (int i = 0; i < 6; i++) w.W_tau[i] = CFC_KAT_W_TAU[i];")
    lines.append("    w.b_tau = CFC_KAT_B_TAU;")
    lines.append("    for (int i = 0; i < 32; i++) w.W_out[i] = CFC_KAT_W_OUT[i];")
    lines.append("    w.b_out = CFC_KAT_B_OUT;")
    lines.append("    // Build transposed views")
    lines.append("    for (uint32_t d = 0; d < CfCConfig::D_INPUT; d++)")
    lines.append("        for (uint32_t h = 0; h < CfCConfig::N_HIDDEN; h++)")
    lines.append("            w.W_x_T[d * CfCConfig::N_HIDDEN + h] = w.W_x[h * CfCConfig::D_INPUT + d];")
    lines.append("    for (uint32_t d = 0; d < CfCConfig::N_HIDDEN; d++)")
    lines.append("        for (uint32_t h = 0; h < CfCConfig::N_HIDDEN; h++)")
    lines.append("            w.W_h_T[d * CfCConfig::N_HIDDEN + h] = w.W_h[h * CfCConfig::N_HIDDEN + d];")
    lines.append("    for (uint32_t d = 0; d < CfCConfig::D_INPUT; d++)")
    lines.append("        for (uint32_t h = 0; h < CfCConfig::N_HIDDEN; h++)")
    lines.append("            w.W_gx_T[d * CfCConfig::N_HIDDEN + h] = w.W_gx[h * CfCConfig::D_INPUT + d];")
    lines.append("    for (uint32_t d = 0; d < CfCConfig::N_HIDDEN; d++)")
    lines.append("        for (uint32_t h = 0; h < CfCConfig::N_HIDDEN; h++)")
    lines.append("            w.W_gh_T[d * CfCConfig::N_HIDDEN + h] = w.W_gh[h * CfCConfig::N_HIDDEN + d];")
    lines.append('    std::strncpy(w.sha256, CFC_KAT_SHA256, 64);')
    lines.append("    w.sha256[64] = 0;")
    lines.append("}")
    lines.append("")

    with open(OUTPUT_H, "w") as f:
        f.write("\n".join(lines))

    print(f"Generated {OUTPUT_H} with {len(vectors)} KAT vectors")

if __name__ == "__main__":
    main()
