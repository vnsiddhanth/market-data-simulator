"""Unit tests for the Python tooling (stdlib unittest; no server needed).

    cd python && python -m unittest discover -s tests -v
"""
from __future__ import annotations

import os
import random
import struct
import tempfile
import unittest

from mds.analysis import EquityPoint, FillRecord, markout, max_drawdown, sharpe, summarize
from mds.book import OrderBook
from mds.codec import (EVENT_SIZE, EventType, MarketEvent, Side, decode_chunk, encode_chunk,
                       read_binary, write_binary)
from mds.resp import encode_command
from mds.store import aggregate


def random_events(n: int, seed: int = 1):
    """Small random order flow that only cancels live orders."""
    rng = random.Random(seed)
    book = OrderBook()
    live = []
    out = []
    oid = 1
    for i in range(n):
        ts = 1_700_000_000_000_000_000 + i * 250_000  # 4 events per ms
        r = rng.random()
        if r < 0.6 or not live:
            side = Side.BUY if rng.random() < 0.5 else Side.SELL
            px = 1_000_000 + rng.randint(-10, 10) * 100
            ev = MarketEvent(ts, oid, px, rng.randint(1, 5) * 100, 0, EventType.ADD, side)
            live.append(oid)
            oid += 1
        elif r < 0.85:
            target = live.pop(rng.randrange(len(live)))
            ev = MarketEvent(ts, target, 0, 0, 0, EventType.CANCEL)
        elif r < 0.9:
            ev = MarketEvent(ts, live[rng.randrange(len(live))], 0, rng.randint(1, 6) * 100, 0, EventType.MODIFY)
        else:
            ev = MarketEvent(ts, oid, 0, rng.randint(1, 4) * 100, 0, EventType.MARKET,
                             Side.BUY if rng.random() < 0.5 else Side.SELL)
            oid += 1
        book.apply(ev)
        live = [x for x in live if x in book._orders]  # noqa: SLF001 - test helper
        out.append(ev)
    return out


class CodecTest(unittest.TestCase):
    def test_layout_matches_cpp(self):
        self.assertEqual(EVENT_SIZE, 40)
        ev = MarketEvent(1, 2, 3, 4, 5, EventType.MODIFY, Side.SELL, 6)
        raw = ev.pack()
        # Field offsets from include/mds/common/event.hpp
        self.assertEqual(struct.unpack_from("<q", raw, 0)[0], 1)
        self.assertEqual(struct.unpack_from("<Q", raw, 8)[0], 2)
        self.assertEqual(struct.unpack_from("<I", raw, 32)[0], 5)
        self.assertEqual(raw[36], 2)
        self.assertEqual(raw[37], 1)
        self.assertEqual(MarketEvent.unpack(raw), ev)

    def test_chunk_and_file_roundtrip(self):
        events = random_events(500)
        self.assertEqual(decode_chunk(encode_chunk(events)), events)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.bin")
            write_binary(p, events)
            self.assertEqual(read_binary(p), events)

    def test_bad_chunk_rejected(self):
        with self.assertRaises(ValueError):
            decode_chunk(b"\x00" * 41)


class RespTest(unittest.TestCase):
    def test_encode_binary_safe(self):
        self.assertEqual(encode_command(["SET", "k", b"a\x00b"]),
                         b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\na\x00b\r\n")


class BookTest(unittest.TestCase):
    def test_fifo_and_price_priority(self):
        b = OrderBook()
        b.add(1, Side.SELL, 1_000_100, 100)
        b.add(2, Side.SELL, 1_000_000, 100)
        b.add(3, Side.SELL, 1_000_000, 100)
        fills = b.market(9, Side.BUY, 250)
        self.assertEqual([(f.maker_id, f.qty) for f in fills], [(2, 100), (3, 100), (1, 50)])
        self.assertEqual(b.best_ask(), 1_000_100)

    def test_modify_priority(self):
        b = OrderBook()
        b.add(1, Side.BUY, 1_000_000, 300)
        b.add(2, Side.BUY, 1_000_000, 100)
        b.modify(1, 500)  # size-up: loses priority
        self.assertEqual(b.market(9, Side.SELL, 50)[0].maker_id, 2)

    def test_book_never_crossed(self):
        b = OrderBook()
        for ev in random_events(5000, seed=3):
            b.apply(ev)
            bb, ba = b.best_bid(), b.best_ask()
            if bb is not None and ba is not None:
                self.assertLess(bb, ba)


class AnalysisTest(unittest.TestCase):
    def test_drawdown_matches_cpp_semantics(self):
        eq = [0, 10, 5, 15, 2, 20]
        ts = [i * 1_000_000_000 for i in range(6)]
        mdd, dur = max_drawdown(eq, ts)
        self.assertAlmostEqual(mdd, 13.0)
        self.assertAlmostEqual(dur, 1.0)

    def test_sharpe_sign(self):
        self.assertGreater(sharpe([1, 2, 1, 2], 1.0), 0)
        self.assertLess(sharpe([-1, -2, -1, -2], 1.0), 0)
        self.assertEqual(sharpe([1, 1, 1], 1.0), 0.0)

    def test_summary_and_markout(self):
        curve = [EquityPoint(i * 1_000_000_000, float(i), 100.0 + i * 0.01, 0) for i in range(10)]
        fills = [FillRecord(0, 1, "BUY", 100.0, 100, 0, True, -0.02),
                 FillRecord(0, 2, "SELL", 100.0, 100, 0, False, 0.03)]
        s = summarize(curve, fills)
        self.assertAlmostEqual(s.total_pnl, 9.0)
        self.assertAlmostEqual(s.maker_ratio, 0.5)
        self.assertEqual(s.volume, 200)
        self.assertAlmostEqual(markout(fills[:1], curve, 1.0), 0.01)
        self.assertAlmostEqual(markout(fills[1:], curve, 1.0), -0.01)

    def test_aggregate_epoch_aligned(self):
        raw = [(0, 1.0), (400, 3.0), (999, 2.0), (1000, 10.0), (2500, 7.0), (2600, 9.0)]
        self.assertEqual(aggregate(raw, "avg", 1000), [(0, 2.0), (1000, 10.0), (2000, 8.0)])
        self.assertEqual(aggregate(raw, "first", 1000)[2], (2000, 7.0))
        self.assertEqual(aggregate(raw, "count", 1000)[0], (0, 3.0))
        self.assertEqual(aggregate(raw, "none", 1000), raw)


if __name__ == "__main__":
    unittest.main()
