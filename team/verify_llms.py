#!/usr/bin/env python3
"""
Verification script for LLM assignments.
Tests each model via the Omniroute API to confirm accessibility and functionality.
"""
import json
import urllib.request
import urllib.error
import sys
import time

OMNIRoute_URL = "http://localhost:20128/v1/chat/completions"

# Agent -> LLM assignments
AGENT_ASSIGNMENTS = {
    "Arquitecto":        {"provider": "auggie",     "model": "aug/opus4.8"},
    "Paralelizador":        {"provider": "combo",      "model": "auto/best-reasoning"},
    "Implementador":          {"provider": "auggie",     "model": "aug/gpt5.5"},
    "Builder":          {"provider": "theoldllm",  "model": "tllm/claude_sonnet_4"},
    "Seguridad":       {"provider": "auggie",     "model": "aug/opus4.8"},
    "Benchmark":       {"provider": "poolside",   "model": "poolside/perf-3"},
    "Tests":           {"provider": "combo",      "model": "auto/best-coding"},
    "Investigador":       {"provider": "auggie",     "model": "aug/gemini-3.1-pro-preview"},
    "Integración":           {"provider": "theoldllm",  "model": "tllm/GPT_5"},
    "Docs":           {"provider": "opencode",   "model": "oc/nemotron-3-ultra-free"},
    "Red Team":         {"provider": "auggie",     "model": "aug/sonnet4.8"},
}

TEST_PROMPT = "Confirma que estas operativo respondiendo exactamente: OK - Modelo funcional"

def test_model(agent_name, provider, model):
    """Test a single model via Omniroute API."""
    payload = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": TEST_PROMPT}],
        "max_tokens": 50,
        "temperature": 0.1,
    }).encode('utf-8')

    req = urllib.request.Request(
        OMNIRoute_URL,
        data=payload,
        headers={"Content-Type": "application/json"},
        method='POST'
    )

    try:
        with urllib.request.urlopen(req, timeout=25) as resp:
            data = json.loads(resp.read().decode('utf-8'))
            if 'error' in data:
                return "FAIL", "API error: " + str(data['error'].get('message', data['error']))
            content = data['choices'][0]['message']['content'].strip()
            if 'OK' in content:
                return "PASS", content
            return "WARN", "Unexpected: " + content[:80]
    except urllib.error.HTTPError as e:
        body = e.read().decode('utf-8')[:200]
        return "FAIL", "HTTP " + str(e.code) + ": " + body
    except urllib.error.URLError as e:
        return "FAIL", "Connection error: " + str(e.reason)
    except Exception as e:
        return "FAIL", "Error: " + str(e)[:100]

def main():
    print("=" * 70)
    print("  LLM VERIFICATION - Agent Team Assignments")
    print("=" * 70)
    print()

    results = {}
    for agent_name, cfg in AGENT_ASSIGNMENTS.items():
        print("  Testing " + agent_name + " (" + cfg['provider'] + "/" + cfg['model'] + ")...", flush=True)
        status, detail = test_model(agent_name, cfg['provider'], cfg['model'])
        results[agent_name] = (status, detail, cfg['model'])
        print("    -> [" + status + "] " + detail[:100])
        print()
        time.sleep(0.5)

    print("=" * 70)
    print("  VERIFICATION SUMMARY")
    print("=" * 70)
    print()

    passed = 0
    failed = 0
    warned = 0
    for agent_name, (status, detail, model) in results.items():
        icon = "OK" if status == "PASS" else ("!!" if status == "WARN" else "X")
        line = "  [" + icon + "] " + agent_name.ljust(20) + " <- " + model
        if status == "PASS":
            passed += 1
        elif status == "WARN":
            warned += 1
        else:
            failed += 1
        print(line)

    print()
    print("  Total: " + str(len(results)) + " | PASS: " + str(passed) + " | WARN: " + str(warned) + " | FAIL: " + str(failed))
    print()

    # Write results to JSON
    output = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "results": {
            agent: {"status": status, "model": model, "detail": detail}
            for agent, (status, detail, model) in results.items()
        },
        "summary": {"total": len(results), "pass": passed, "warn": warned, "fail": failed}
    }
    output_path = "/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/team/verification/llm_verification.json"
    with open(output_path, 'w') as f:
        json.dump(output, f, indent=2, ensure_ascii=False)
    print("  Results saved to: " + output_path)

    return 0 if failed == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
