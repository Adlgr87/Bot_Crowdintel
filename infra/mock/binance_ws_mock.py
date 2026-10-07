#!/usr/bin/env python3
"""
Mock Binance WebSocket server for offline testing.

Implements a subset of the Binance Spot stream combined endpoint protocol:
  wss://stream.binance.com:9443/stream?streams=symbol@depth20@100ms/symbol@trade/symbol@kline_1m

Runs on ws://localhost:9876/stream?streams=btcusdt@depth20@100ms/btcusdt@trade/btcusdt@kline_1m

Provides deterministic, synthetic L2 book data so the full hot-path pipeline
(including OFI, CfC, WindowShield, Kelly) can be exercised without network access.

Usage:
    python3 infra/mock/binance_ws_mock.py
    python3 infra/mock/binance_ws_mock.py --port 9876 --symbol btcusdt
    python3 infra/mock/binance_ws_mock.py --inject-crash --at 30.0   # simulate outage
"""
import argparse
import asyncio
import json
import random
import signal
import sys
import time
from typing import Optional

import websockets

DEFAULT_PRICE = 60000.00
DEFAULT_SPREAD_BPS = 1.0
BASE_VOLUME = 1.5  # BTC per trade event

class MockBinanceFeed:
    """Generates synthetic Binance streams: depth20, trade, kline_1m."""

    def __init__(self, symbol: str = "btcusdt"):
        self.symbol = symbol
        self.base_price = DEFAULT_PRICE
        self.last_price = DEFAULT_PRICE
        self.start_time = time.time()
        self.tick_count = 0
        self.inject_crash_at: Optional[float] = None
        self.running = True

    def _rand_price(self) -> float:
        """Random walk price with mean reversion."""
        drift = (self.base_price - self.last_price) * 0.0001
        vol = 50.0  # $50 std dev per step
        change = random.gauss(drift, vol)
        self.last_price = max(100.0, self.last_price + change)
        return self.last_price

    def _make_depth(self) -> dict:
        """Generate depth20 snapshot: 20 bids + 20 asks."""
        mid = self._rand_price()
        spread = mid * DEFAULT_SPREAD_BPS / 10000.0
        bid_px = mid - spread / 2
        ask_px = mid + spread / 2

        bids = []
        asks = []
        for i in range(20):
            price = bid_px - i * (mid * 0.00005)  # 5bps per level
            vol = BASE_VOLUME * (1.0 - i * 0.04)
            bids.append([round(price, 2), round(vol, 4)])
        for i in range(20):
            price = ask_px + i * (mid * 0.00005)
            vol = BASE_VOLUME * (1.0 - i * 0.04)
            asks.append([round(price, 2), round(vol, 4)])

        self.tick_count += 1

        # Simulate crash if configured
        if self.inject_crash_at is not None:
            elapsed = time.time() - self.start_time
            if elapsed > self.inject_crash_at and self.tick_count % 10 == 0:
                # Inject 10% price spike
                mid = mid * (1.10 if random.random() > 0.5 else 0.90)

        return {
            "e": "depthUpdate",
            "E": int(time.time() * 1000),
            "s": self.symbol.upper(),
            "U": self.tick_count * 100,  # first update ID
            "u": self.tick_count * 100 + 1,  # final update ID
            "pu": "",
            "b": bids,
            "a": asks,
        }

    def _make_trade(self) -> dict:
        """Generate a trade event."""
        price = self._rand_price()
        qty = round(random.uniform(0.1, 2.0), 4)
        return {
            "e": "trade",
            "E": int(time.time() * 1000),
            "s": self.symbol.upper(),
            "t": self.tick_count,
            "p": f"{price:.2f}",
            "q": f"{qty:.4f}",
            "b": self.tick_count * 2,
            "a": self.tick_count * 2 + 1,
            "T": int(time.time() * 1000),
            "m": random.choice([True, False]),  # buyer is buyer-maker
            "M": True,
        }

    def _make_kline(self) -> dict:
        """Generate 1-minute kline."""
        price = self._rand_price()
        return {
            "e": "kline",
            "E": int(time.time() * 1000),
            "s": self.symbol.upper(),
            "k": {
                "t": int(time.time() / 60) * 60 * 1000,
                "T": int(time.time() / 60) * 60 * 1000 + 60000,
                "s": self.symbol.upper(),
                "i": "1m",
                "f": 100,
                "L": 200,
                "o": f"{price - 100:.2f}",
                "c": f"{price:.2f}",
                "h": f"{price + 200:.2f}",
                "l": f"{price - 200:.2f}",
                "v": "1.5000",
                "n": 10,
                "x": False,
                "V": "1.5000",
                "q": "90000.0000",
                "V": "1.5000",
                "Q": "1",
                "B": "123456",
            },
        }


async def handler(websocket, path, feed: MockBinanceFeed):
    """Handle a connected client, sending synthetic stream data."""
    print(f"[mock] Client connected from {websocket.remote_address}")

    try:
        # Send initial depth snapshot
        depth = feed._make_depth()
        await websocket.send(json.dumps({
            "stream": f"{feed.symbol}@depth20@100ms",
            "data": depth
        }))

        msg_count = 0
        while feed.running and websocket.open:
            # Every ~200ms send depth update
            depth = feed._make_depth()
            await websocket.send(json.dumps({
                "stream": f"{feed.symbol}@depth20@100ms",
                "data": depth
            }))

            # Every ~100ms send a trade
            if msg_count % 2 == 0:
                trade = feed._make_trade()
                await websocket.send(json.dumps({
                    "stream": f"{feed.symbol}@trade",
                    "data": trade
                }))

            # On minute boundary, send kline
            if msg_count % 300 == 0:
                kline = feed._make_kline()
                await websocket.send(json.dumps({
                    "stream": f"{feed.symbol}@kline_1m",
                    "data": kline
                }))

            msg_count += 1
            await asyncio.sleep(0.1)  # 100ms cadence

    except websockets.exceptions.ConnectionClosed:
        print(f"[mock] Client disconnected")
    except Exception as e:
        print(f"[mock] Error: {e}")


def main():
    parser = argparse.ArgumentParser(description="Mock Binance WebSocket server")
    parser.add_argument("--port", type=int, default=9876, help="WebSocket port")
    parser.add_argument("--symbol", type=str, default="btcusdt", help="Trading pair")
    parser.add_argument("--inject-crash", type=float, default=None,
                        help="Inject price spike after N seconds")
    args = parser.parse_args()

    feed = MockBinanceFeed(args.symbol)
    if args.inject_crash is not None:
        feed.inject_crash_at = args.inject_crash

    start_server = websockets.serve(
        lambda ws, path: handler(ws, path, feed),
        "localhost", args.port
    )

    print(f"[mock] Binance WS mock on ws://localhost:{args.port}/stream?streams={args.symbol}@depth20@100ms/{args.symbol}@trade/{args.symbol}@kline_1m")
    print(f"[mock] Symbol: {args.symbol}, base price: ${DEFAULT_PRICE}")

    async def run():
        server = await start_server
        await server.wait_closed()

    try:
        asyncio.get_event_loop().run_until_complete(
            websockets.serve(
                lambda ws, path: handler(ws, path, feed),
                "localhost", args.port
            )
        )
        print(f"[mock] Running... (Ctrl+C to stop)")
        asyncio.get_event_loop().run_forever()
    except KeyboardInterrupt:
        print("\n[mock] Shutting down")
        sys.exit(0)


if __name__ == "__main__":
    main()
