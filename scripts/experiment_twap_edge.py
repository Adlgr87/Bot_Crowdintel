#!/usr/bin/env python3
"""
experiment_twap_edge.py — Gate experiment for Motor A (crypto TWAP convergence).

QUESTION
  Polymarket's up/down crypto markets settle on a 60-second Chainlink TWAP.
  Chainlink publishes that report 1-2 s after DON consensus, so the report is
  the LABEL, not the signal. The strategy claim is: reconstruct the TWAP-60s
  locally from a fast price series and know the direction before the market
  finishes repricing.

  This script tests the only thing that matters before risking capital:
  does a locally reconstructed TWAP-60s beat (a) the settlement outcome and
  (b) the market's own price, net of the real fee?

METHOD
  For each settled round:
    * window = [end - duration, end], read from the slug and endDate
    * strike  = last traded price at window open
    * TWAP over the elapsed part of the window = VWAP of Binance 1m candles
      (quoteVolume / volume is exactly the time-weighted mean price)
    * final-TWAP estimate two minutes before the close, assuming the remaining
      tail trades at the current price:
          twap_final ~ (elapsed * twap_so_far + tau * price_now) / duration
      which is the conditional mean of the Brownian-bridge integral
    * market probability = CLOB price of the "Up" token at the same instant
    * outcome = Gamma outcomePrices on the settled market

HONESTY NOTES
  * The Binance VWAP is a PROXY for Chainlink's own aggregate. Residual basis
    risk is the point of the experiment, not something it hides.
  * Two minutes of lead is deliberately conservative: it is roughly 100x the
    DON's 1-2 s delivery latency, so any edge found is not a latency artefact.
  * Fees come from the market's own feeSchedule when present, else the crypto
    default. A result that only survives with zero fees is not a result.

USAGE
  python3 scripts/experiment_twap_edge.py --asset btc --duration 15m --rounds 40
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone

GAMMA = "https://gamma-api.polymarket.com"
CLOB = "https://clob.polymarket.com"
BINANCE = "https://api.binance.com/api/v3"
UA = {"User-Agent": "crowdintel-research/1.0"}

DURATIONS = {"5m": 300, "15m": 900, "1h": 3600, "4h": 14400}
SYMBOLS = {"btc": "BTCUSDT", "eth": "ETHUSDT", "sol": "SOLUSDT", "xrp": "XRPUSDT",
           "doge": "DOGEUSDT"}
# The public-search endpoint matches on the human question text, so it needs the
# full asset name and not the ticker.
ASSET_QUERY = {"btc": "bitcoin", "eth": "ethereum", "sol": "solana",
               "xrp": "xrp", "doge": "dogecoin"}


def get_json(url: str, timeout: int = 30):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def find_rounds(asset: str, duration: str, want: int):
    """Settled up/down markets for one asset+duration, newest first."""
    q = urllib.parse.quote(f"{ASSET_QUERY[asset]} up or down")
    data = get_json(f"{GAMMA}/public-search?q={q}&limit_per_type=50")

    out, seen, seen_priced = [], set(), {}

    def walk(node):
        if isinstance(node, dict):
            slug = node.get("slug") or ""
            if (slug.startswith(f"{asset}-updown-{duration}-") and
                    node.get("closed") and node.get("outcomePrices")):
                if slug not in seen:
                    seen.add(slug)
                    out.append(node)
                elif not seen_priced.get(slug) and node.get("outcomePrices"):
                    seen_priced[slug] = True
                    for i, existing in enumerate(out):
                        if existing.get("slug") == slug:
                            out[i] = node
                            break
            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    walk(data)
    out.sort(key=lambda m: m.get("endDate") or "", reverse=True)
    return out[:want]


def binance_klines(symbol: str, start_ms: int, end_ms: int):
    url = (f"{BINANCE}/klines?symbol={symbol}&interval=1m"
           f"&startTime={start_ms}&endTime={end_ms}&limit=1000")
    raw = get_json(url)
    rows = []
    for k in raw:
        rows.append({
            "open_ms": int(k[0]),
            "open": float(k[1]),
            "close": float(k[4]),
            "base_vol": float(k[5]),
            "quote_vol": float(k[7]),
        })
    return rows


def market_price_at(up_token: str, start_ts: int, end_ts: int, at_ts: int):
    """Nearest 'Up' token price at or before at_ts, from CLOB history."""
    url = (f"{CLOB}/prices-history?market={up_token}"
           f"&startTs={start_ts}&endTs={end_ts}&fidelity=1")
    try:
        payload = get_json(url, timeout=30)
    except Exception:
        return None
    hist = payload.get("history") if isinstance(payload, dict) else payload
    if not hist:
        return None
    best = None
    for pt in hist:
        t, p = int(pt.get("t", 0)), float(pt.get("p", 0))
        if t <= at_ts:
            best = p
        elif best is None:
            best = p  # first point after the instant: closest available
            break
    return best


def fee_rate(market: dict) -> float:
    sched = market.get("feeSchedule")
    if isinstance(sched, dict):
        r = sched.get("rate")
        if isinstance(r, (int, float)) and 0.0 <= r <= 1.0:
            return float(r)
    return 0.07  # crypto default from FeeCategory::Crypto


def taker_fee(rate: float, price: float) -> float:
    p = min(max(price, 0.0), 1.0)
    return rate * p * (1.0 - p)


def evaluate(asset: str, duration: str, rounds: int, lead_seconds: int, verbose: bool):
    dur_s = DURATIONS[duration]
    symbol = SYMBOLS[asset]
    markets = find_rounds(asset, duration, rounds)
    print(f"rondas liquidadas encontradas: {len(markets)}  "
          f"({asset} {duration}, {symbol})")
    if not markets:
        print("nada que analizar: prueba otro activo/duracion o mas rondas.")
        return 1

    rows = []
    for m in markets:
        slug = m["slug"]
        try:
            end_ts = int(datetime.strptime(
                m["endDate"][:19], "%Y-%m-%dT%H:%M:%S"
            ).replace(tzinfo=timezone.utc).timestamp())
        except Exception:
            # Fall back to the unix stamp embedded in the slug.
            try:
                end_ts = int(slug.rsplit("-", 1)[1])
            except Exception:
                continue

        start_ts = end_ts - dur_s
        tokens = json.loads(m["clobTokenIds"]) if m.get("clobTokenIds") else []
        prices = json.loads(m["outcomePrices"]) if m.get("outcomePrices") else []
        if len(tokens) < 2 or len(prices) < 2:
            continue
        outcome_up = prices[0] == "1"

        probe_ts = end_ts - lead_seconds
        if probe_ts <= start_ts:
            continue

        try:
            ks = binance_klines(symbol, (start_ts - 120) * 1000, (end_ts + 60) * 1000)
        except Exception as e:
            if verbose:
                print(f"  {slug}: klines fail {e}")
            continue
        if len(ks) < 4:
            continue

        # strike = last price at window open = close of the candle ending at start
        pre = [k for k in ks if k["open_ms"] < start_ts * 1000]
        if not pre:
            continue
        strike = pre[-1]["close"]

        # TWAP over the elapsed part of the window (VWAP of the 1m candles)
        elapsed = [k for k in ks
                   if start_ts * 1000 <= k["open_ms"] < probe_ts * 1000]
        if not elapsed:
            continue
        base = sum(k["base_vol"] for k in elapsed)
        quote = sum(k["quote_vol"] for k in elapsed)
        if base <= 0:
            continue
        twap_so_far = quote / base

        now_candle = [k for k in ks if k["open_ms"] <= probe_ts * 1000]
        if not now_candle:
            continue
        price_now = now_candle[-1]["close"]

        t_elapsed = probe_ts - start_ts
        tau = end_ts - probe_ts
        twap_final = (t_elapsed * twap_so_far + tau * price_now) / dur_s

        mkt_p = market_price_at(tokens[0], start_ts, end_ts, probe_ts)
        rows.append({
            "slug": slug,
            "strike": strike,
            "twap_final_est": twap_final,
            "gap_bps": (twap_final - strike) / strike * 10000.0,
            "model_up": twap_final > strike,
            "market_p": mkt_p,
            "outcome_up": outcome_up,
            "rate": fee_rate(m),
        })

    if not rows:
        print("no se pudo completar ninguna ronda con datos suficientes.")
        return 1

    n = len(rows)
    model_hits = sum(1 for r in rows if r["model_up"] == r["outcome_up"])
    print(f"\nrondas evaluadas: {n}   (sonda a T-{lead_seconds}s del cierre)")
    print(f"  precision del modelo TWAP-60s replica : {model_hits/n*100:.1f}%")

    with_mkt = [r for r in rows if r["market_p"] is not None]
    if with_mkt:
        m = len(with_mkt)
        mkt_hits = sum(1 for r in with_mkt
                       if (r["market_p"] > 0.5) == r["outcome_up"])
        print(f"  precision del propio mercado          : {mkt_hits/m*100:.1f}%"
              f"   (n={m})")
        # Brier of the market's own probability: the bar to beat.
        brier_mkt = sum((r["market_p"] - (1.0 if r["outcome_up"] else 0.0)) ** 2
                        for r in with_mkt) / m
        print(f"  Brier del mercado                     : {brier_mkt:.4f}")

        # Disagreements are where edge can live.
        dis = [r for r in with_mkt
               if (r["market_p"] > 0.5) != r["model_up"]]
        if dis:
            right = sum(1 for r in dis
                        if r["model_up"] == r["outcome_up"])
            print(f"  desacuerdos modelo vs mercado         : {len(dis)}"
                  f"  -> el modelo acierta {right}/{len(dis)}"
                  f" ({right/len(dis)*100:.0f}%)")

        # Realised PnL of the naive rule: follow the model when the gap beats
        # fee + margin, at the market's price, held to settlement.
        for margin in (0.00, 0.01, 0.02):
            pnl, trades, wins = 0.0, 0, 0
            for r in with_mkt:
                fee = taker_fee(r["rate"], r["market_p"])
                edge = (1.0 if r["model_up"] else 0.0) - r["market_p"] - fee
                if edge <= margin:
                    continue
                trades += 1
                if r["model_up"] == r["outcome_up"]:
                    pnl += 1.0 - r["market_p"] - fee
                    wins += 1
                else:
                    pnl -= r["market_p"] + fee
            if trades:
                print(f"  regla |edge|>{margin:.2f}: {trades:3d} trades  "
                      f"{wins} ganados  PnL {pnl:+.3f} u/c  "
                      f"({pnl/trades:+.4f} u/c por trade)")
            else:
                print(f"  regla |edge|>{margin:.2f}: sin trades")

    print("\nlectura:")
    print("  * 'precision del modelo' por encima de ~52% implicaria edge bruto")
    print("    sobre un mercado que abre cerca de 0.50, porque el fee de crypto")
    print("    a p=0.5 exige ~51.75% de acierto solo para empatar.")
    print("  * 'Brier del mercado' es el liston real: el modelo tiene que")
    print("    batirlo en las rondas donde discrepa, no en promedio.")
    print("  * Muestra pequena: esto es un gate, no una conclusion. Si no hay")
    print("    senal aqui, el sleeve crypto no se financia.")
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--asset", default="btc", choices=sorted(SYMBOLS))
    ap.add_argument("--duration", default="15m", choices=sorted(DURATIONS))
    ap.add_argument("--rounds", type=int, default=40)
    ap.add_argument("--lead", type=int, default=120,
                    help="segundos antes del cierre en que se decide")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)
    return evaluate(a.asset, a.duration, a.rounds, a.lead, a.verbose)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
