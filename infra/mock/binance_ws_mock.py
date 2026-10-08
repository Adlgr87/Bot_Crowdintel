#!/usr/bin/env python3
"""
Mock Binance WebSocket server for offline testing.

Implements a subset of the Binance Spot stream combined endpoint protocol:
    wss://stream.binance.com:9443/stream?streams=symbol@depth20@100ms/symbol@trade/symbol@kline_1m

Runs on: ws://localhost:9876/stream?streams=btcusdt@depth20@100ms/btcusdt@trade/btcusdt@kline_1m

Provides deterministic, synthetic L2 book data so the full hot-path pipeline
(including OFI, CfC, WindowShield, Kelly) can be exercised without network access.

Usage:
    python3 infra/mock/binance_ws_mock.py
    python3 infra/mock/binance_ws_mock.py --port 9876 --symbol btcusdt
    python3 infra/mock/binance_ws_mock.py --inject-crash --at 30.0   # simulate outage

Requires:   pip install websockets
"""

import argparse
import asyncio
import json
import math
import random
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
            "U": self.tick_count * 100,   # first update ID
            "u": self.tick_count * 100 + 1,  # final update ID
            "pu": "",
            "b": bids,   # Binance WebSocket spec uses "b" for bids
            "a": asks,   # and "a" for asks
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
            "b": self.tick_count * 2,    # buyer order ID
            "a": self.tick_count * 2 + 1,  # seller order ID
            "T": int(time.time() * 1000),
            "m": random.choice([True, False]),  # maker (buyer-initiated flag)
        }

    def _make_kline(self) -> dict:
        """Generate a 1-minute kline event."""
        price = self._rand_price()
        return {
            "e": "kline",
            "E": int(time.time() * 1000),
            "s": self.symbol.upper(),
            "k": {
                "t": int(time.time() / 60) * 60 * 1000,
                "T": int(time.time() * 1000),
                "s": self.symbol.upper(),
                "i": "1m",
                "f": 1000 + self.tick_count,
                "L": 1000 + self.tick_count + 10,
                "o": f"{price - 50:.2f}",
                "c": f"{price:.2f}",
                "h": f"{price + 100:.2f}",
                "l": f"{price - 100:.2f}",
                "v": f"{random.uniform(0.5, 5.0):.4f}",
                "n": 150 + self.tick_count,
                "x": False,
                "q": f"{price * 10:.2f}",
                "V": f"{random.uniform(5.0, 15.0):.4f}",
                "Q": f"{price * 5:.2f}",
            },
        }


class CombinedStreamHandler:
    """Wraps events in the combined stream envelope and routes by stream name."""

    def __init__(self, feed: MockBinanceFeed):
        self.feed = feed
        self.streams = {
            f"{feed.symbol}@depth20@100ms": feed._make_depth,
            f"{feed.symbol}@trade": feed._make_trade,
            f"{feed.symbol}@kline_1m": feed._make_kline,
        }

    def make_event(self, stream_name: str) -> str:
        """Generate a wrapped event for the given stream."""
        event_fn = self.streams.get(stream_name)
        if event_fn is None:
            # Default to depth update
            event_fn = self.feed._make_depth
        data = event_fn()
        envelope = {"stream": stream_name, "data": data}
        return json.dumps(envelope)


async def handler(websocket, path, feed: MockBinanceFeed, handler_obj: CombinedStreamHandler):
    """Handle a single WebSocket client connection."""
    peer = f"{websocket.remote[0]}:{websocket.remote[1]}"
    print(f"[mock] Client connected: {peer}", flush=True)

    async def send_loop():
        """Send depth updates at 100ms intervals, trades at ~500ms, klines at 60s."""
        last_depth = time.monotonic()
        last_trade = time.monotonic()
        last_kline = time.monotonic()
        DEPTH_INTERVAL = 0.1   # 100ms
        TRADE_INTERVAL = 0.5   # 500ms
        KLINE_INTERVAL = 60.0  # 60s

        while True:
            now = time.monotonic()

            # Send depth update
            if now - last_depth >= DEPTH_INTERVAL:
                msg = handler_obj.make_event(f"{feed.symbol}@depth20@100ms")
                await websocket.send(msg)
                last_depth = now

            # Send trade
            if now - last_trade >= TRADE_INTERVAL:
                msg = handler_obj.make_event(f"{feed.symbol}@trade")
                await websocket.send(msg)
                last_trade = now

            # Send kline
            if now - last_kline >= KLINE_INTERVAL:
                msg = handler_obj.make_event(f"{feed.symbol}@kline_1m")
                await websocket.send(msg)
                last_kline = now

            await asyncio.sleep(0.01)  # 10ms poll

    async def recv_loop():
        """Receive messages from client (handle PING, subscription, etc.)."""
        while True:
            msg = await websocket.recv()
            if msg == "PING":
                await websocket.send("PONG")
            elif msg == "ping":
                await websocket.send("pong")
            else:
                print(f"[mock] Received: {msg[:100]}", flush=True)

    # Run both loops, exit when either completes
    try:
        done, pending = await asyncio.wait(
            [send_loop(), recv_loop()],
            return_when=asyncio.FIRST_COMPLETED
        )
        for task in pending:
            task.cancel()
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        print(f"[mock] Client disconnected: {peer}", flush=True)


def main():
    parser = argparse.ArgumentParser(
        description="Mock Binance WebSocket server for offline testing")
    parser.add_argument("--port", type=int, default=9876,
                        help="Port to listen on (default: 9876)")
    parser.add_argument("--host", default="localhost",
                        help="Host to bind to (default: localhost)")
    parser.add_argument("--symbol", default="btcusdt",
                        help="Trading pair (default: btcusdt)")
    parser.add_argument("--inject-crash", action="store_true",
                        help="Inject random price crashes for testing resilience")
    parser.add_argument("--at", type=float, default=30.0,
                        help="Time (s) at which to start crash injection")
    args = parser.parse_args()

    random.seed(42)  # Deterministic for reproducibility

    feed = MockBinanceFeed(symbol=args.symbol)
    if args.inject_crash:
        feed.inject_crash_at = args.at

    handler_obj = CombinedStreamHandler(feed)

    async def start_server():
        server = await websockets.serve(
            lambda ws, path: handler(ws, path, feed, handler_obj),
            args.host, args.port,
            ping_interval=10, ping_timeout=30,
        )
        print(f"[mock] Binance WS mock on ws://{args.host}:{args.port}/stream"
              f"?streams={args.symbol}@depth20@100ms/{args.symbol}@trade"
              f"/{args.symbol}@kline_1m", flush=True)
        print(f"[mock] Symbol: {args.symbol}, base price: ${DEFAULT_PRICE}", flush=True)
        if args.inject_crash:
            print(f"[mock] Crash injection enabled (starts at {args.at}s)", flush=True)
        print("[mock] Running... (Ctrl+C to stop)", flush=True)
        async with server:
            await server.wait_closed()

    try:
        asyncio.run(start_server())
    except KeyboardInterrupt:
        print("\n[mock] Shutting down", flush=True)
        sys.exit(0)


if __name__ == "__main__":
    main()
