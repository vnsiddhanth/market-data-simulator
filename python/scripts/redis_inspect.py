#!/usr/bin/env python3
"""Inspect what the simulator stored in Redis.

    python python/scripts/redis_inspect.py --redis 127.0.0.1:6379 --symbol SIM
    python python/scripts/redis_inspect.py --redis 127.0.0.1:6379 --bars 1000 --field mid

Shows the metadata hash, per-series sample counts, 1-second OHLC bars built
with server-side aggregation, and verifies that the stored event chunks decode
and replay into a sane order book.
"""
from __future__ import annotations

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from mds.book import OrderBook  # noqa: E402
from mds.resp import RedisClient  # noqa: E402
from mds.store import FIELDS, TickStore  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--redis", default="127.0.0.1:6379")
    ap.add_argument("--symbol", default="SIM")
    ap.add_argument("--field", default="mid")
    ap.add_argument("--bars", type=int, default=1000, help="bar size in ms")
    ap.add_argument("--rows", type=int, default=8)
    args = ap.parse_args()

    with RedisClient.from_endpoint(args.redis) as r:
        store = TickStore(r, args.symbol)
        meta = store.meta()
        print(f"backend : {'RedisTimeSeries' if store.use_ts else 'sorted-set fallback'}")
        print("meta    :", meta or "(none - run mds_sim --redis first)")
        if not meta:
            return 1

        print("\nseries:")
        for f in FIELDS:
            pts = store.range(f)
            span = f"{pts[0][0]} .. {pts[-1][0]}" if pts else "-"
            print(f"  {store.key('ts:' + f):<28} {len(pts):>9,} samples  [{span}]")

        print(f"\n{args.bars} ms OHLC bars of '{args.field}' (server-side aggregation when available):")
        o = store.range(args.field, agg="first", bucket_ms=args.bars)
        h = store.range(args.field, agg="max", bucket_ms=args.bars)
        lo = store.range(args.field, agg="min", bucket_ms=args.bars)
        c = store.range(args.field, agg="last", bucket_ms=args.bars)
        vol = dict(store.range("trade_qty", agg="sum", bucket_ms=args.bars))
        print(f"  {'bucket_ms':>14} {'open':>10} {'high':>10} {'low':>10} {'close':>10} {'volume':>10}")
        for (t, ov), (_, hv), (_, lv), (_, cv) in list(zip(o, h, lo, c))[: args.rows]:
            print(f"  {t:>14} {ov:>10.4f} {hv:>10.4f} {lv:>10.4f} {cv:>10.4f} {vol.get(t, 0):>10,.0f}")

        t0 = time.perf_counter()
        events = store.load_events()
        dt = time.perf_counter() - t0
        print(f"\nevent log: {len(events):,} events loaded in {dt:.2f}s "
              f"({len(events) / dt if dt else 0:,.0f} events/s)")
        if events:
            book = OrderBook()
            for e in events:
                book.apply(e)
            print(f"  replayed into reference book: {len(book):,} resting orders, "
                  f"bid={book.best_bid()} ask={book.best_ask()} mid={book.mid()}")
            ordered = all(a.ts <= b.ts for a, b in zip(events, events[1:]))
            print(f"  timestamps monotonic: {ordered}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
