"""Integration tests: TickStore against the in-repo fake Redis server.

Exercises both backends (RedisTimeSeries semantics and the sorted-set
fallback) over a real TCP socket and checks they return identical data.
"""
from __future__ import annotations

import asyncio
import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
import fake_redis  # noqa: E402

from mds.book import OrderBook  # noqa: E402
from mds.resp import RedisClient, RedisReplyError  # noqa: E402
from mds.store import TickStore  # noqa: E402
from test_core import random_events  # noqa: E402


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start_fake(timeseries: bool) -> int:
    port = _free_port()
    ready = threading.Event()

    def run() -> None:
        loop = asyncio.new_event_loop()

        async def main() -> None:
            ev = asyncio.Event()
            task = asyncio.ensure_future(fake_redis.serve("127.0.0.1", port, timeseries, ev))
            await ev.wait()
            ready.set()
            await task

        loop.run_until_complete(main())

    threading.Thread(target=run, daemon=True).start()
    if not ready.wait(5):
        raise RuntimeError("fake redis did not start")
    return port


class RoundTripTest(unittest.TestCase):
    events = random_events(12_000, seed=42)

    @classmethod
    def setUpClass(cls):
        cls.ts_port = start_fake(True)
        cls.zset_port = start_fake(False)

    def _ingest(self, port: int, symbol: str, force_zset: bool = False):
        r = RedisClient("127.0.0.1", port)
        self.addCleanup(r.close)
        store = TickStore(r, symbol, force_zset=force_zset, chunk_events=1000)
        store.purge()
        store.init()
        stats = store.ingest(self.events)
        return r, store, stats

    def test_backend_detection(self):
        _, s1, _ = self._ingest(self.ts_port, "DET")
        _, s2, _ = self._ingest(self.zset_port, "DET")
        self.assertTrue(s1.use_ts)
        self.assertFalse(s2.use_ts)
        self.assertEqual(s1.meta()["backend"], "redistimeseries")
        self.assertEqual(s2.meta()["backend"], "zset")

    def test_event_log_is_lossless(self):
        _, store, stats = self._ingest(self.ts_port, "LOG")
        self.assertEqual(stats["chunks"], 12)
        self.assertEqual(store.load_events(), self.events)
        meta = store.meta()
        self.assertEqual(int(meta["events"]), len(self.events))
        self.assertEqual(int(meta["chunks"]), 12)

    def test_time_window_query(self):
        _, store, _ = self._ingest(self.ts_port, "WIN")
        lo, hi = self.events[3000].ts, self.events[7000].ts
        got = store.load_events(lo, hi)
        expect = [e for e in self.events if lo <= e.ts <= hi]
        self.assertEqual(got, expect)

    def test_series_match_reference_book(self):
        _, store, _ = self._ingest(self.ts_port, "SER")
        book = OrderBook()
        last_mid_per_ms = {}
        for e in self.events:
            book.apply(e)
            m = book.mid()
            if m is not None:
                last_mid_per_ms[e.ts // 1_000_000] = m
        got = dict(store.range("mid"))
        self.assertEqual(set(got), set(last_mid_per_ms))
        for k, v in last_mid_per_ms.items():
            self.assertAlmostEqual(got[k], v, places=6)

    def test_timeseries_and_fallback_agree(self):
        _, a, _ = self._ingest(self.ts_port, "AGREE")
        _, b, _ = self._ingest(self.zset_port, "AGREE")
        for field in ("mid", "spread", "trade_qty"):
            self.assertEqual(a.range(field), b.range(field))
            for agg in ("avg", "max", "first", "last", "sum", "count"):
                ra = a.range(field, agg=agg, bucket_ms=7)
                rb = b.range(field, agg=agg, bucket_ms=7)
                self.assertEqual(len(ra), len(rb), (field, agg))
                for (t1, v1), (t2, v2) in zip(ra, rb):
                    self.assertEqual(t1, t2)
                    self.assertAlmostEqual(v1, v2, places=6)

    def test_trade_volume_conserved(self):
        _, store, _ = self._ingest(self.ts_port, "VOL")
        book = OrderBook()
        volume = sum(f.qty for e in self.events for f in book.apply(e))
        stored = sum(v for _, v in store.range("trade_qty"))
        self.assertEqual(volume, stored)

    def test_unknown_command_is_error_reply(self):
        r = RedisClient("127.0.0.1", self.zset_port)
        self.addCleanup(r.close)
        reply = r.execute("TS.INFO", "x", raise_errors=False)
        self.assertIsInstance(reply, RedisReplyError)
        self.assertIn("unknown command", str(reply).lower())


if __name__ == "__main__":
    unittest.main()
