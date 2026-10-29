#!/usr/bin/env python3
"""
experiment_maker_edge.py — Gate experiment for Motor C (passive maker carry).

WHY THIS SLEEVE IS DIFFERENT
  Motors A and B both needed a prediction to be right. Motor C does not: the
  return comes from three mechanical sources, each measurable from public data.

    1. LIQUIDITY REWARDS  — `clobRewards[].rewardsDailyRate` on the market, paid
       daily to makers quoting inside `rewardsMaxSpread` with at least
       `rewardsMinSize` shares. Yield per dollar = pool / qualifying capital,
       and qualifying capital is exactly the book depth inside that band.
    2. SPREAD CAPTURE     — quoting at the touch earns the spread on a round trip.
       Half of it is the fair share of a fill.
    3. ADVERSE SELECTION  — the cost. You are filled precisely when the price is
       moving against you, so the expected mark-out loss per fill is what eats
       the first two.

  The gate is the sign of:  spread/2 + carry-per-fill  -  E[adverse move].

  Note from the docs that shapes the whole design: maker rebates are funded by
  taker fees, so a 0%-fee market pays NO rebate — only liquidity rewards. Fee
  category therefore trades off two different income streams, not one.

METHOD
  * top markets by 24h volume that have an active clobRewards entry
  * book snapshot -> mid, spread, and depth inside the rewards band
  * 1-minute price history -> realised volatility in cents
  * report carry yield, spread/2, sigma_1min and the net per fill

USAGE
  python3 scripts/experiment_maker_edge.py --markets 25 --hours 72
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
import urllib.request
from datetime import datetime, timezone

GAMMA = "https://gamma-api.polymarket.com"
CLOB = "https://clob.polymarket.com"
UA = {"User-Agent": "crowdintel-research/1.0"}


def get_json(url: str, timeout: int = 30, retries: int = 3):
    last = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read().decode())
        except Exception as e:
            last = e
            time.sleep(1.0 * (attempt + 1))
    raise last  # type: ignore[misc]


def active_reward_rate(market: dict) -> float:
    """Daily reward pool in USDC, or 0 when the program is not active."""
    now = time.time()
    total = 0.0
    for rw in market.get("clobRewards") or []:
        try:
            start = rw.get("startDate")
            end = rw.get("endDate")
            if start:
                t0 = datetime.fromisoformat(f"{start}T00:00:00+00:00").timestamp()
                if now < t0:
                    continue
            if end:
                t1 = datetime.fromisoformat(f"{end}T00:00:00+00:00").timestamp()
                if now > t1:
                    continue
            total += float(rw.get("rewardsDailyRate") or 0)
        except Exception:
            continue
    return total


def book_snapshot(token: str):
    book = get_json(f"{CLOB}/book?token_id={token}")
    bids = [(float(l["price"]), float(l["size"])) for l in (book.get("bids") or [])]
    asks = [(float(l["price"]), float(l["size"])) for l in (book.get("asks") or [])]
    if not bids or not asks:
        return None
    bids.sort(key=lambda x: -x[0])
    asks.sort(key=lambda x: x[0])
    bb, ba = bids[0][0], asks[0][0]
    if not (0.0 < bb < ba < 1.0):
        return None
    return {"bids": bids, "asks": asks, "best_bid": bb, "best_ask": ba,
            "mid": (bb + ba) / 2.0, "spread": ba - bb}


def sigma_per_minute(token: str, hours: int):
    """Realised 1-minute volatility of the token price, in probability units."""
    end = int(time.time())
    start = end - hours * 3600
    try:
        payload = get_json(f"{CLOB}/prices-history?market={token}"
                           f"&startTs={start}&endTs={end}&fidelity=1")
    except Exception:
        return None
    hist = payload.get("history") if isinstance(payload, dict) else payload
    if not hist or len(hist) < 10:
        return None
    pts = sorted((int(p.get("t", 0)), float(p.get("p", 0))) for p in hist)
    rets = []
    for i in range(1, len(pts)):
        if pts[i][1] > 0 and pts[i - 1][1] > 0:
            rets.append(math.log(pts[i][1] / pts[i - 1][1]))
    if len(rets) < 8:
        return None
    return statistics.pstdev(rets)


def evaluate(markets_wanted: int, hours: int):
    listing = get_json(
        f"{GAMMA}/markets?limit=300&closed=false&order=volume24hr&ascending=false")
    print(f"mercados activos considerados: {len(listing)}")

    rows = []
    seen = 0
    for m in listing:
        if seen >= markets_wanted:
            break
        pool = active_reward_rate(m)
        if pool <= 0:
            continue
        spread = float(m.get("spread") or 0)
        price = float(m.get("bestBid") or 0)
        band = float(m.get("rewardsMaxSpread") or 0) / 100.0
        min_size = float(m.get("rewardsMinSize") or 0)
        if band <= 0 or min_size <= 0 or price <= 0:
            continue
        tokens = m.get("clobTokenIds")
        if not tokens:
            continue
        try:
            token = json.loads(tokens)[0]
        except Exception:
            continue

        try:
            book = book_snapshot(token)
        except Exception:
            continue
        if not book:
            continue
        mid = book["mid"]

        # Capital that currently qualifies for rewards, both sides, at mid.
        qb = sum(size for p, size in book["bids"] if p >= mid - band)
        qa = sum(size for p, size in book["asks"] if p <= mid + band)
        qualifying_capital = (qb + qa) * mid
        if qualifying_capital < 1000.0:
            continue

        sig = sigma_per_minute(token, hours)
        if sig is None:
            continue

        carry_per_day = pool / qualifying_capital             # fraction of capital
        spread_half_c = book["spread"] * 100.0 / 2.0          # cents
        # Adverse move over a one-minute hold, converted to cents of price.
        adverse_c = sig * mid * 100.0
        # Carry earned per round trip assumes ~1 fill per side per minute, a
        # deliberately generous assumption; it is reported, not hidden.
        carry_per_trip_c = carry_per_day / 1440.0 * mid * 100.0
        net_c = spread_half_c + carry_per_trip_c - adverse_c

        rows.append({
            "slug": m.get("slug", ""),
            "pool": pool,
            "qual_cap": qualifying_capital,
            "carry_pct_day": carry_per_day * 100.0,
            "spread_c": book["spread"] * 100.0,
            "spread_half_c": spread_half_c,
            "carry_trip_c": carry_per_trip_c,
            "adverse_c": adverse_c,
            "net_c": net_c,
            "price": mid,
            "min_size": min_size,
            "competitive": float(m.get("competitive") or 0),
            "fee": (m.get("feeSchedule") or {}).get("rate"),
            "seen": seen,
        })
        seen += 1
        time.sleep(0.3)

    if not rows:
        print("sin mercados con rewards activos y datos de book utilizables.")
        return 1

    rows.sort(key=lambda r: -r["carry_pct_day"])
    print(f"\nmercados con rewards activos y book medible: {len(rows)}\n")
    print(f"{'slug':<34}{'pool/d':>9}{'cap.calif':>12}{'carry%/d':>10}"
          f"{'spr/2':>8}{'adv1m':>8}{'neto':>8}")
    for r in rows[:18]:
        print(f"{r['slug'][:33]:<34}{r['pool']:>9,.0f}{r['qual_cap']:>12,.0f}"
              f"{r['carry_pct_day']:>9.2f}%{r['spread_half_c']:>8.2f}"
              f"{r['adverse_c']:>8.3f}{r['net_c']:>+8.3f}")

    positive = [r for r in rows if r["net_c"] > 0]
    print(f"\nresumen sobre {len(rows)} mercados:")
    print(f"  carry mediano            : "
          f"{statistics.median(r['carry_pct_day'] for r in rows):.2f}%/dia")
    print(f"  spread/2 mediano         : "
          f"{statistics.median(r['spread_half_c'] for r in rows):.2f}c")
    print(f"  adverso 1min mediano     : "
          f"{statistics.median(r['adverse_c'] for r in rows):.3f}c")
    print(f"  neto por fill mediano    : "
          f"{statistics.median(r['net_c'] for r in rows):+.3f}c")
    print(f"  mercados con neto > 0    : {len(positive)}/{len(rows)}"
          f"  ({len(positive)/len(rows)*100:.0f}%)")

    if positive:
        best = max(positive, key=lambda r: r["net_c"])
        print(f"\n  mejor: {best['slug'][:44]}")
        print(f"    pool ${best['pool']:,.0f}/dia sobre ${best['qual_cap']:,.0f} "
              f"de capital que califica -> {best['carry_pct_day']:.2f}%/dia")
        print(f"    spread/2 {best['spread_half_c']:.2f}c + carry "
              f"{best['carry_trip_c']:.4f}c - adverso {best['adverse_c']:.3f}c "
              f"= {best['net_c']:+.3f}c por fill")

    print("\nlectura:")
    print("  * 'carry%/d' es el pool diario dividido por el capital que hoy")
    print("    califica en ese mercado. Es el rendimiento si fueses el unico")
    print("    maker; con competencia te llevas tu proporcion.")
    print("  * 'neto' compara lo que ganas por fill (medio spread + carry)")
    print("    contra lo que pierdes por seleccion adversa en 1 minuto.")
    print("    Positivo de forma sostenida = el sleeve existe.")
    print("  * Sesgo optimista declarado: asume ~1 fill por lado por minuto.")
    print("    Si el fill rate real es menor, el carry por fill baja.")
    return 0


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--markets", type=int, default=25)
    ap.add_argument("--hours", type=int, default=72)
    a = ap.parse_args(argv)
    print("Gate del Motor C — ¿paga el maker mas de lo que le cuesta?")
    print(f"ahora: {datetime.now(timezone.utc):%Y-%m-%d %H:%M}Z\n")
    return evaluate(a.markets, a.hours)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
