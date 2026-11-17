#!/usr/bin/env python3
"""Cross-check an event file against the Python reference order book.

    mds_sim --events 200000 --bin out/ticks.bin --depth 5
    python python/scripts/verify_events.py out/ticks.bin

Replays the MDSEVT01 (or CSV) file through ``mds.book.OrderBook`` and prints
the final top-of-book plus event statistics. Because the Python book is an
independent implementation, matching the depth printed by ``mds_sim`` is an
end-to-end check of the C++ matching engine and of the binary codec.
"""
from __future__ import annotations

import argparse
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from mds.book import OrderBook  # noqa: E402
from mds.codec import PRICE_SCALE, Side, iter_csv, read_binary  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--depth", type=int, default=5)
    args = ap.parse_args()

    events = list(iter_csv(args.path)) if args.path.endswith(".csv") else read_binary(args.path)
    mix = Counter(e.type.name for e in events)
    book = OrderBook()
    fills = volume = 0
    for e in events:
        fs = book.apply(e)
        fills += len(fs)
        volume += sum(f.qty for f in fs)

    span = (events[-1].ts - events[0].ts) / 1e9 if events else 0
    print(f"{len(events):,} events over {span:.2f}s simulated  mix={dict(mix)}")
    print(f"fills={fills:,} volume={volume:,} resting orders={len(book):,}")
    bids, asks = book.depth(Side.BUY, args.depth), book.depth(Side.SELL, args.depth)
    print(f"\n  {'bid qty':>12} {'bid':>10} | {'ask':<10} {'ask qty':<12}")
    for i in range(args.depth):
        left = f"{bids[i][1]:>12} {bids[i][0] / PRICE_SCALE:>10.4f}" if i < len(bids) else " " * 23
        right = f"{asks[i][0] / PRICE_SCALE:<10.4f} {asks[i][1]:<12}" if i < len(asks) else ""
        print(f"  {left} | {right}")
    print(f"  imbalance(5)={book.imbalance(5):+.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
