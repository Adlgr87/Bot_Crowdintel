#!/usr/bin/env python3
"""
experiment_copy_edge.py — Gate experiment for Motor B (insider mirroring).

QUESTION
  Motor B copies wallets that crowdintel.xyz flags as informed. Whether that
  works depends on two things that are measurable from PUBLIC data, without
  any subscription:

   1. PERSISTENCE. If a wallet's past win rate does not predict its future win
      rate, then selecting "good" wallets is curve-fitting and copying them
      earns the base rate minus fees.

   2. COPY LAG. The copier sees the trade seconds to minutes late. Whatever the
      insider captured is partly gone by then. If the price has already moved
      by more than the edge, mirroring captures nothing.

  This script measures both. It deliberately measures the raw mechanism rather
  than crowdintel's curated output, so the result is a BOUND: a curated feed can
  only be better than naive top-wallet copying if its selection is genuinely
  informative, and it still cannot beat the copy-lag physics measured here.

METHOD
  Part 1 — selection test
    * top wallets by PnL from the public leaderboard
    * every settled position (market resolved), with realized PnL and end date
    * split the whole sample at the median resolution date
    * rank wallets on the EARLY period only, take the top quartile, and measure
      their LATE-period win rate against the base rate of all wallets
    * if the selected wallets do not beat the base rate out of sample, there is
      no persistent skill to copy

  Part 2 — copy-lag decay
    * sample recent BUY trades by those wallets
    * measure the same token's traded price 60 s / 180 s / 600 s later
    * the drift, in cents, is a lower bound on what a copier pays in slippage

USAGE
  python3 scripts/experiment_copy_edge.py --wallets 25 --trade-samples 25
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone

DATA = "https://data-api.polymarket.com"
CLOB = "https://clob.polymarket.com"
UA = {"User-Agent": "crowdintel-research/1.0"}


def get_json(url: str, timeout: int = 30, retries: int = 3):
    last = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read().decode())
        except Exception as e:  # rate limit / transient
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise last  # type: ignore[misc]


def parse_ts(value) -> int | None:
    if value is None:
        return None
    if isinstance(value, (int, float)):
        return int(value)
    s = str(value).replace("Z", "+00:00")
    try:
        return int(datetime.fromisoformat(s).timestamp())
    except Exception:
        try:
            return int(datetime.strptime(str(value)[:19], "%Y-%m-%dT%H:%M:%S")
                       .replace(tzinfo=timezone.utc).timestamp())
        except Exception:
            return None


def leaderboard(limit: int, min_volume: float):
    payload = get_json(f"{DATA}/v2/leaderboard?limit={limit}")
    rows = payload.get("data", [])
    return [r for r in rows if float(r.get("volume") or 0) >= min_volume]


def positions_of(wallet: str, max_pages: int = 4):
    out, cursor, pages = [], None, 0
    while pages < max_pages:
        url = f"{DATA}/v2/positions?user={wallet}&limit=50"
        if cursor:
            url += f"&cursor={urllib.parse.quote(cursor)}"
        try:
            payload = get_json(url)
        except Exception:
            break
        rows = payload.get("data") or []
        out.extend(rows)
        pag = payload.get("pagination") or {}
        if not pag.get("has_more"):
            break
        cursor = pag.get("next_cursor")
        if not cursor:
            break
        pages += 1
    return out


def pnl_series(wallet: str):
    """Cumulative realised market PnL over time, from /v2/user-pnl.

    This endpoint exists precisely because /positions cannot answer the
    question: on a resolved market the winners are redeemed and vanish from the
    holdings list while the losers stay behind as worthless tokens, so scoring
    on /positions reported a 3.4% base rate and -99% PnL. That is a survivorship
    inversion, not a finding.
    """
    try:
        # interval=max gives a multi-week daily series. The default returns
        # only the last 24 hours, whose median split lands on today and leaves
        # the "late" window empty.
        payload = get_json(f"{DATA}/v2/user-pnl?user={wallet}"
                           f"&interval=max&fidelity=1d")
    except Exception:
        return []
    data = payload.get("data") or {}
    pts = data.get("points") or []
    out = []
    for pt in pts:
        ts = parse_ts(pt.get("timestamp"))
        val = pt.get("realized_market_pnl")
        if ts is None or val is None:
            continue
        out.append((ts, float(val)))
    out.sort()
    return out


def part1_persistence(wallets, now_ts):
    print("=" * 74)
    print("PARTE 1 — ¿persiste la habilidad? (test de contingencia)")
    print("=" * 74)
    print("  fuente: /v2/user-pnl (PnL realizado acumulado por wallet)")
    print("  test : P(rentable despues | rentable antes) vs P(rentable despues)")
    print("         escala-invariante, asi que no depende del tamano de cuenta.\n")

    series = {}
    for i, w in enumerate(wallets):
        pts = pnl_series(w["user_id"])
        if len(pts) >= 6:
            series[w["user_id"]] = pts
        if i % 10 == 0 and i:
            print(f"  ... {i + 1}/{len(wallets)} wallets, "
                  f"{len(series)} con serie utilizable")
        time.sleep(0.25)

    if len(series) < 12:
        print(f"  muestra insuficiente ({len(series)} wallets con serie).")
        return None

    # The union median is a poor split: histories start at different dates, so a
    # median over the union can land before most wallets even existed. Choose
    # the candidate split that maximises the number of wallets with real data on
    # BOTH sides, which is what the contingency test needs.
    all_ts = sorted({t for pts in series.values() for t, _ in pts})
    best_split, best_score = all_ts[len(all_ts) // 2], -1
    for frac in (0.3, 0.4, 0.5, 0.6, 0.7, 0.8):
        cand = all_ts[min(len(all_ts) - 1, int(len(all_ts) * frac))]
        score = sum(1 for pts in series.values()
                    if sum(1 for t, _ in pts if t < cand) >= 3
                    and sum(1 for t, _ in pts if t >= cand) >= 3)
        if score > best_score:
            best_score, best_split = score, cand
    split = best_split
    print(f"  wallets con serie: {len(series)}   corte: "
          f"{datetime.fromtimestamp(split, timezone.utc):%Y-%m-%d}"
          f"   (cubiertos en ambos lados: {best_score})")

    def window_pnl(pts, before: bool):
        """Realised PnL generated strictly inside one side of the split."""
        total = 0.0
        for i in range(1, len(pts)):
            t0, v0 = pts[i - 1]
            t1, v1 = pts[i]
            in_side = (t1 < split) if before else (t0 >= split)
            if in_side:
                total += (v1 - v0)
        return total

    early, late = {}, {}
    for addr, pts in series.items():
        early[addr] = window_pnl(pts, True)
        late[addr] = window_pnl(pts, False)

    active = [a for a in series
              if abs(early[a]) > 1.0 and abs(late[a]) > 1.0]
    print(f"  wallets con actividad real en AMBOS periodos: {len(active)}")
    if len(active) < 10:
        print("  muestra insuficiente para el test de contingencia.")
        return None

    base_late = sum(1 for a in active if late[a] > 0) / len(active)
    good_early = [a for a in active if early[a] > 0]
    bad_early = [a for a in active if early[a] <= 0]

    def rate(xs):
        return (sum(1 for a in xs if late[a] > 0) / len(xs)) if xs else 0.0

    print(f"\n  base rate tardio (todas)            : {base_late*100:5.1f}%"
          f"   (n={len(active)})")
    print(f"  P(rentable tardio | rentable antes) : {rate(good_early)*100:5.1f}%"
          f"   (n={len(good_early)})")
    print(f"  P(rentable tardio | perdio antes)   : {rate(bad_early)*100:5.1f}%"
          f"   (n={len(bad_early)})")

    lift = (rate(good_early) - base_late) * 100.0
    print(f"\n  >> lift de seleccion por historial: {lift:+.1f} puntos")

    # PnL de una regla concreta: copiar solo a los que ganaron antes.
    copy_pnl = sum(late[a] for a in good_early)
    all_pnl = sum(late[a] for a in active)
    print(f"  >> PnL tardio de los 'buenos' : {copy_pnl:+,.0f} USD")
    print(f"     PnL tardio de todas        : {all_pnl:+,.0f} USD")

    if lift >= 15.0 and copy_pnl > 0:
        verdict = "PERSISTE: la seleccion por historial tiene senal"
    elif lift >= 5.0:
        verdict = "DEBIL: lift marginal, no financia un sleeve por si solo"
    else:
        verdict = ("NO PERSISTE: elegir por historial no predice el futuro;"
                   " copiar cobra el base rate menos fees")
    print(f"  >> {verdict}")
    return {"lift": lift, "base_late": base_late,
            "late_good": rate(good_early), "n": len(active),
            "copy_pnl": copy_pnl}


def part2_copy_lag(wallets, samples, now_ts):
    print()
    print("=" * 74)
    print("PARTE 2 — ¿cuánto edge se evapora antes de poder copiar?")
    print("=" * 74)

    trades = []
    for w in wallets[:12]:
        try:
            payload = get_json(
                f"{DATA}/v2/trades?user={w['user_id']}&limit=20")
        except Exception:
            continue
        for t in (payload.get("data") or []):
            if str(t.get("side", "")).upper() != "BUY":
                continue
            ts = parse_ts(t.get("timestamp")) or int(t.get("matchtime") or 0)
            price = float(t.get("price") or 0)
            token = str(t.get("token_id") or "")
            if ts and price > 0 and token:
                trades.append({"ts": ts, "price": price, "token": token})
        if len(trades) >= samples:
            break
        time.sleep(0.3)

    trades.sort(key=lambda t: t["ts"], reverse=True)
    trades = trades[:samples]
    print(f"  trades BUY recolectados: {len(trades)}")
    if not trades:
        print("  sin datos suficientes (¿el filtro 'user' no aplica a /trades?).")
        return None

    horizons = {"60s": 60, "180s": 180, "600s": 600}
    drifts = {k: [] for k in horizons}
    for t in trades:
        start = t["ts"] - 60
        end = t["ts"] + 900
        try:
            payload = get_json(
                f"{CLOB}/prices-history?market={t['token']}"
                f"&startTs={start}&endTs={end}&fidelity=1")
        except Exception:
            continue
        hist = payload.get("history") if isinstance(payload, dict) else payload
        if not hist:
            continue
        pts = sorted((int(p.get("t", 0)), float(p.get("p", 0))) for p in hist)
        for label, delta in horizons.items():
            after = [p for p in pts if p[0] >= t["ts"] + delta]
            if after:
                drifts[label].append((after[0][1] - t["price"]) * 100.0)
        time.sleep(0.2)

    if not any(drifts.values()):
        print("  sin historial de precios para las muestras.")
        return None

    print(f"\n  {'horizonte':<10}{'n':>5}{'deriva mediana':>17}{'media':>10}")
    result = {}
    for label in horizons:
        d = drifts[label]
        if not d:
            continue
        med = statistics.median(d)
        mean = sum(d) / len(d)
        result[label] = med
        print(f"  {label:<10}{len(d):>5}{med:>+16.2f}c{mean:>+9.2f}c")

    med60 = result.get("60s", 0.0)
    print(f"\n  >> deriva a 60 s: {med60:+.2f}c. Con fee de crypto a p=0.5")
    print(f"     (1.75c) el copiador necesita que el edge bruto supere")
    print(f"     {abs(med60) + 1.75:.2f}c solo para empatar.")
    return result


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--wallets", type=int, default=50)
    ap.add_argument("--min-volume", type=float, default=20_000.0)
    ap.add_argument("--trade-samples", type=int, default=25)
    a = ap.parse_args(argv)

    now_ts = int(time.time())
    print("Gate del Motor B — ¿es copiable la habilidad de una wallet?")
    print(f"ahora: {datetime.fromtimestamp(now_ts, timezone.utc):%Y-%m-%d %H:%M}Z\n")

    wallets = leaderboard(a.wallets, a.min_volume)
    print(f"wallets del leaderboard con volumen >= ${a.min_volume:,.0f}: "
          f"{len(wallets)}")
    if len(wallets) < 8:
        print("muestra de wallets insuficiente.")
        return 1

    p1 = part1_persistence(wallets, now_ts)
    p2 = part2_copy_lag(wallets, a.trade_samples, now_ts)

    print()
    print("=" * 74)
    print("LECTURA")
    print("=" * 74)
    print("  * 'ventaja out-of-sample' >= +2 puntos: hay habilidad persistente")
    print("    que un servicio de seleccion podria explotar. Merece seguir.")
    print("  * ventaja ~0: elegir wallets por historial es curve-fitting; el")
    print("    copiado cobra el base rate menos fees, es decir, pierde.")
    print("  * deriva a 60 s grande frente al fee: el edge ya se fue antes de")
    print("    que un observador externo pueda actuar, sin importar cuan buena")
    print("    sea la deteccion.")
    print("  * Esta medicion es un LIMITE INFERIOR de la dificultad: usa")
    print("    copiado ingenuo del top de wallets, no las senales curadas de")
    print("    crowdintel. Una seleccion mejor puede acercarse, pero no puede")
    print("    violar la fisica del lag medida en la parte 2.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
