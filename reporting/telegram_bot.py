#!/usr/bin/env python3
"""
Phase 4.2: Telegram notification bot
PERF: Runs as separate thread/process, never touches trading hot-path
Impact: Real-time alerts without trading engine interference
"""
import os
import sys
import requests

TELEGRAM_TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
TELEGRAM_CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")

def send_telegram(message: str) -> bool:
    """Send message to Telegram chat."""
    if not TELEGRAM_TOKEN or not TELEGRAM_CHAT_ID:
        return False
    
    url = f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage"
    payload = {
        "chat_id": TELEGRAM_CHAT_ID,
        "text": message,
        "parse_mode": "Markdown",
    }
    
    try:
        r = requests.post(url, json=payload, timeout=5)
        return r.status_code == 200
    except Exception:
        return False

def send_alert(alert_type: str, message: str) -> bool:
    """Send critical alert."""
    from state_reader import generate_report, format_telegram_message
    report = generate_report()
    
    prefix = {
        "kill_switch": "🛑 **KILL SWITCH ACTIVATED**",
        "drawdown": "⚠️ **DRAWDOWN ALERT**",
        "reconnect": "🔄 **RECONNECTING**",
        "latency": "🐢 **LATENCY SPIKE**",
    }.get(alert_type, f"⚠️ **{alert_type.upper()}**")
    
    full_msg = f"{prefix}\n\n{message}\n\n---\n{format_telegram_message(report)}"
    return send_telegram(full_msg)

if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(__file__))
    report = generate_report()
    msg = format_telegram_message(report)
    sent = send_telegram(msg)
    print(f"Telegram: {'sent' if sent else 'failed (check env vars)'}")
