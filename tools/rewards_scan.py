#!/usr/bin/env python3
"""Escaner de mercados con recompensas de liquidez de Polymarket.

Ordena los mercados incentivados por pago diario (pUSD/dia) y, opcionalmente,
los enriquece con metadatos del mercado (pregunta, spread, tick, comision).

Uso:
    python3 tools/rewards_scan.py --live --grep btc
    python3 tools/rewards_scan.py --live --min-rate 20 --enrich 25
    python3 tools/rewards_scan.py --live --json > rewarded.json
    python3 tools/rewards_scan.py --self-test

Solo usa la biblioteca estandar (urllib + json). No requiere claves: los
endpoints usados son publicos.

Advertencia: el pago real no es el "daily rate" completo: se reparte de forma
proporcional al puntaje cuadratico de cada maker dentro de la banda
(rewardsMaxSpread) y por encima del tamano minimo (rewardsMinSize), muestreado
una vez por minuto. La columna "cap_1lado" es una estimacion gruesa del capital
necesario para UN lado a precio 0.50; los programas suelen exigir dos lados
fuera del rango [0.10, 0.90] del punto medio.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request

REWARDS_URL = "https://clob.polymarket.com/rewards/markets/current"
GAMMA_MARKETS_URL = "https://gamma-api.polymarket.com/markets"
USER_AGENT = "crowdintel-rewards-scan/1.0"


def http_json(url: str, timeout: float = 20.0):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT,
                                                   "Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8", "replace"))


def _as_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def normalize_rewards_entry(entry: dict) -> dict | None:
    """Extrae los campos utiles de una entrada de /rewards/markets/current."""
    if not isinstance(entry, dict):
        return None
    condition_id = entry.get("condition_id") or entry.get("conditionId")
    if not condition_id:
        return None
    configs = entry.get("rewards_config") or entry.get("rewardsConfig") or []
    start_date = end_date = None
    if isinstance(configs, list) and configs and isinstance(configs[0], dict):
        start_date = configs[0].get("start_date")
        end_date = configs[0].get("end_date")
    rate = entry.get("total_daily_rate")
    if rate is None:
        rate = entry.get("native_daily_rate")
    if rate is None:
        rate = entry.get("rate_per_day")
    return {
        "condition_id": condition_id,
        "rate_per_day": _as_float(rate),
        "min_size": _as_float(entry.get("rewards_min_size")),
        "max_spread": _as_float(entry.get("rewards_max_spread")),
        "start_date": start_date,
        "end_date": end_date,
        "question": None,
        "slug": None,
        "tick": None,
        "min_order_size": None,
        "spread": None,
        "liquidity": None,
        "volume24hr": None,
        "fee_rate": None,
        "holding_rewards": None,
    }


def fetch_rewards(pages: int, page_size: int = 100) -> list[dict]:
    out: list[dict] = []
    cursor = ""
    seen_cursors: set[str] = set()
    for _ in range(max(pages, 1)):
        params = {"page_size": str(page_size)}
        if cursor:
            params["next_cursor"] = cursor
        payload = http_json(REWARDS_URL + "?" + urllib.parse.urlencode(params))
        if isinstance(payload, list):
            rows = payload
            cursor = ""
        elif isinstance(payload, dict):
            rows = payload.get("data") or []
            cursor = payload.get("next_cursor") or ""
        else:
            rows = []
            cursor = ""
        for row in rows:
            parsed = normalize_rewards_entry(row)
            if parsed:
                out.append(parsed)
        if not cursor or cursor in seen_cursors:
            break
        seen_cursors.add(cursor)
    return out


def enrich_with_gamma(rows: list[dict], limit: int) -> list[dict]:
    """Completa metadatos por mercado. Tolerante a fallos: si un lookup falla,
    la fila se conserva sin metadatos."""
    for row in rows[: max(limit, 0)]:
        query = urllib.parse.urlencode({"condition_ids": row["condition_id"]})
        try:
            payload = http_json(GAMMA_MARKETS_URL + "?" + query)
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError,
                json.JSONDecodeError):
            return rows
        market = None
        if isinstance(payload, list):
            for candidate in payload:
                if isinstance(candidate, dict) and \
                        candidate.get("conditionId") == row["condition_id"]:
                    market = candidate
                    break
        if not market:
            # El parametro condition_ids no fue soportado: mejor no enriquecer
            # con datos de otro mercado.
            return rows
        fee_schedule = market.get("feeSchedule") or {}
        row.update({
            "question": market.get("question"),
            "slug": market.get("slug"),
            "tick": market.get("orderPriceMinTickSize"),
            "min_order_size": market.get("orderMinSize"),
            "spread": market.get("spread"),
            "liquidity": market.get("liquidityNum"),
            "volume24hr": market.get("volume24hr"),
            "fee_rate": fee_schedule.get("rate") if isinstance(fee_schedule, dict) else None,
            "holding_rewards": market.get("holdingRewardsEnabled"),
        })
    return rows


def apply_filters(rows: list[dict], grep: str | None, min_rate: float) -> list[dict]:
    filtered = []
    needle = (grep or "").strip().lower()
    for row in rows:
        if row["rate_per_day"] < min_rate:
            continue
        if needle:
            haystack = " ".join(str(row.get(key) or "") for key in
                                 ("question", "slug", "condition_id")).lower()
            if needle not in haystack:
                continue
        filtered.append(row)
    return filtered


def print_table(rows: list[dict], limit: int) -> None:
    header = f"{'pUSD/dia':>9} {'min_size':>8} {'max_spread':>10} {'tick':>6} " \
             f"{'spread':>7} {'vol24h':>10} {'lic':>10} {'cap_1lado':>10}  mercado"
    print(header)
    print("-" * len(header))
    def show(value):
        return "-" if value is None else value

    for row in rows[:limit]:
        cap = row["min_size"] * 0.5 if row["min_size"] else 0.0
        label = row["question"] or row["slug"] or row["condition_id"]
        print(f"{row['rate_per_day']:9.2f} "
              f"{row['min_size']:8.2f} "
              f"{row['max_spread']:10.2f} "
              f"{show(row['tick']):>6} "
              f"{show(row['spread']):>7} "
              f"{show(row['volume24hr']):>10} "
              f"{show(row['liquidity']):>10} "
              f"{cap:10.2f}  {str(label)[:70]}")


def self_test() -> int:
    """Valida parser, filtros y orden sin tocar la red."""
    fixture = {
        "data": [
            {"condition_id": "0xaaa1", "total_daily_rate": 3, "rewards_min_size": 20,
             "rewards_max_spread": 4.5,
             "rewards_config": [{"start_date": "2026-10-03", "end_date": "2500-12-31"}]},
            {"condition_id": "0xbbb2", "total_daily_rate": 500, "rewards_min_size": 1000,
             "rewards_max_spread": 2.5, "rewards_config": []},
            {"condition_id": "0xccc3", "rate_per_day": 33, "rewards_min_size": 20,
             "rewards_max_spread": 4.5},
            "ruido",
            {"rewards_min_size": 5},
        ],
        "next_cursor": "",
    }
    parsed = [normalize_rewards_entry(row) for row in fixture["data"]]
    parsed = [row for row in parsed if row]
    assert len(parsed) == 3, parsed

    parsed.sort(key=lambda row: row["rate_per_day"], reverse=True)
    assert [row["condition_id"] for row in parsed] == ["0xbbb2", "0xccc3", "0xaaa1"]

    fake_meta = [dict(parsed[0], question="Bitcoin Up or Down on October 4?",
                      slug="bitcoin-up-or-down-on-october-4-2026")]
    assert apply_filters(fake_meta, "bitcoin", 0)[0]["slug"].startswith("bitcoin")
    assert apply_filters(fake_meta, "ethereum", 0) == []
    assert apply_filters(parsed, None, 100)[0]["condition_id"] == "0xbbb2"
    assert normalize_rewards_entry({"condition_id": "0xddd4"})["rate_per_day"] == 0.0
    print("self-test OK: parser, orden y filtros verificados sin red.")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--live", action="store_true",
                        help="consulta la API publica (requiere red saliente)")
    parser.add_argument("--self-test", action="store_true",
                        help="valida la logica de parseo sin red")
    parser.add_argument("--grep", default=None, help="filtra por texto (p. ej. btc)")
    parser.add_argument("--min-rate", type=float, default=1.0,
                        help="pago diario minimo en pUSD (default 1.0)")
    parser.add_argument("--pages", type=int, default=3,
                        help="paginas de /rewards/markets/current (100 c/u)")
    parser.add_argument("--enrich", type=int, default=25,
                        help="cuantos mercados enriquecer con Gamma")
    parser.add_argument("--limit", type=int, default=40, help="filas a imprimir")
    parser.add_argument("--json", action="store_true", help="salida JSON cruda")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.live:
        parser.print_help()
        return 2

    try:
        rows = fetch_rewards(args.pages)
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError,
            json.JSONDecodeError) as error:
        print(f"ERROR consultando la API publica: {error}", file=sys.stderr)
        return 1

    rows.sort(key=lambda row: row["rate_per_day"], reverse=True)
    rows = apply_filters(rows, args.grep, args.min_rate)
    rows = enrich_with_gamma(rows, args.enrich)

    if args.json:
        json.dump(rows, sys.stdout, indent=2, ensure_ascii=False)
        print()
        return 0

    print(f"Mercados incentivados: {len(rows)} (min_rate={args.min_rate}, "
          f"grep={args.grep or '-'})")
    print_table(rows, args.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
