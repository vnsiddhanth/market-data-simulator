#!/usr/bin/env python3
"""Load an event file (MDSEVT01 .bin or CSV) into Redis using the same schema
as the C++ TickStore, so externally-sourced data can be backtested with
``mds_backtest --redis``.

    python python/scripts/ingest_events.py out/ticks.bin --redis 127.0.0.1:6379 --symbol SIM --purge
"""
from __future__ import annotations

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from mds.codec import iter_csv, read_binary  # noqa: E402
from mds.resp import RedisClient  # noqa: E402
from mds.store import TickStore  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--redis", default="127.0.0.1:6379")
    ap.add_argument("--symbol", default="SIM")
    ap.add_argument("--purge", action="store_true", help="delete existing data for the symbol first")
    ap.add_argument("--zset", action="store_true", help="force the sorted-set fallback")
    args = ap.parse_args()

    events = list(iter_csv(args.path)) if args.path.endswith(".csv") else read_binary(args.path)
    with RedisClient.from_endpoint(args.redis) as r:
        store = TickStore(r, args.symbol, force_zset=args.zset)
        if args.purge:
            store.purge()
        store.init()
        t0 = time.perf_counter()
        stats = store.ingest(events)
        dt = time.perf_counter() - t0
    print(f"ingested {stats['events']:,} events ({stats['chunks']} chunks, {stats['samples']:,} series samples) "
          f"in {dt:.2f}s -> {stats['events'] / dt:,.0f} events/s "
          f"[{'RedisTimeSeries' if store.use_ts else 'sorted-set'}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
