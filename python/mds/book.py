"""Reference price-time-priority order book in pure Python.

Not fast (that is the C++ book's job) but small enough to be obviously
correct. Used to cross-check event files produced by the C++ simulator
(``scripts/verify_events.py``) and to compute snapshots when ingesting CSV
data into Redis from Python.
"""
from __future__ import annotations

import bisect
from collections import deque
from dataclasses import dataclass
from typing import Deque, Dict, List, Optional, Tuple

from .codec import EventType, MarketEvent, PRICE_SCALE, Side


@dataclass
class Fill:
    maker_id: int
    taker_id: int
    price: int
    qty: int
    aggressor: Side
    ts: int


@dataclass
class _Order:
    id: int
    side: Side
    price: int
    qty: int


class OrderBook:
    def __init__(self) -> None:
        # Levels: price -> deque of live _Order (cancelled orders are removed eagerly).
        self._levels: Tuple[Dict[int, Deque[_Order]], Dict[int, Deque[_Order]]] = ({}, {})
        # Sorted price lists; bids stored negated so index 0 is always "best".
        self._keys: Tuple[List[int], List[int]] = ([], [])
        self._orders: Dict[int, _Order] = {}

    # -- helpers -------------------------------------------------------------
    @staticmethod
    def _key(side: Side, price: int) -> int:
        return -price if side == Side.BUY else price

    def _add_level(self, side: Side, price: int) -> Deque[_Order]:
        levels = self._levels[side]
        if price not in levels:
            levels[price] = deque()
            bisect.insort(self._keys[side], self._key(side, price))
        return levels[price]

    def _drop_level(self, side: Side, price: int) -> None:
        del self._levels[side][price]
        keys = self._keys[side]
        keys.pop(bisect.bisect_left(keys, self._key(side, price)))

    def _best(self, side: Side) -> Optional[int]:
        keys = self._keys[side]
        if not keys:
            return None
        return -keys[0] if side == Side.BUY else keys[0]

    # -- order entry -----------------------------------------------------------
    def _match(self, taker: int, side: Side, limit: Optional[int], qty: int, ts: int) -> Tuple[int, List[Fill]]:
        fills: List[Fill] = []
        opp = Side.SELL if side == Side.BUY else Side.BUY
        while qty > 0:
            best = self._best(opp)
            if best is None:
                break
            if limit is not None and (best > limit if side == Side.BUY else best < limit):
                break
            queue = self._levels[opp][best]
            while qty > 0 and queue:
                maker = queue[0]
                t = min(qty, maker.qty)
                maker.qty -= t
                qty -= t
                fills.append(Fill(maker.id, taker, best, t, side, ts))
                if maker.qty == 0:
                    queue.popleft()
                    del self._orders[maker.id]
            if not queue:
                self._drop_level(opp, best)
        return qty, fills

    def add(self, oid: int, side: Side, price: int, qty: int, ts: int = 0) -> List[Fill]:
        if qty <= 0 or price <= 0 or oid in self._orders:
            return []
        remaining, fills = self._match(oid, side, price, qty, ts)
        if remaining > 0:
            o = _Order(oid, side, price, remaining)
            self._add_level(side, price).append(o)
            self._orders[oid] = o
        return fills

    def market(self, oid: int, side: Side, qty: int, ts: int = 0) -> List[Fill]:
        return self._match(oid, side, None, qty, ts)[1]

    def cancel(self, oid: int) -> bool:
        o = self._orders.pop(oid, None)
        if o is None:
            return False
        queue = self._levels[o.side][o.price]
        queue.remove(o)
        if not queue:
            self._drop_level(o.side, o.price)
        return True

    def modify(self, oid: int, new_qty: int) -> bool:
        o = self._orders.get(oid)
        if o is None:
            return False
        if new_qty <= 0:
            return self.cancel(oid)
        if new_qty > o.qty:  # loses priority
            queue = self._levels[o.side][o.price]
            queue.remove(o)
            queue.append(o)
        o.qty = new_qty
        return True

    def apply(self, ev: MarketEvent) -> List[Fill]:
        if ev.type == EventType.ADD:
            return self.add(ev.id, ev.side, ev.price, ev.qty, ev.ts)
        if ev.type == EventType.CANCEL:
            self.cancel(ev.id)
            return []
        if ev.type == EventType.MODIFY:
            self.modify(ev.id, ev.qty)
            return []
        return self.market(ev.id, ev.side, ev.qty, ev.ts)

    # -- market data -------------------------------------------------------------
    def best_bid(self) -> Optional[int]:
        return self._best(Side.BUY)

    def best_ask(self) -> Optional[int]:
        return self._best(Side.SELL)

    def mid(self) -> Optional[float]:
        b, a = self.best_bid(), self.best_ask()
        if b is None or a is None:
            return None
        return (b + a) / 2 / PRICE_SCALE

    def depth(self, side: Side, levels: int = 5) -> List[Tuple[int, int, int]]:
        """[(price, total_qty, order_count)] best first."""
        out = []
        for k in self._keys[side][:levels]:
            price = -k if side == Side.BUY else k
            q = self._levels[side][price]
            out.append((price, sum(o.qty for o in q), len(q)))
        return out

    def imbalance(self, levels: int = 5) -> float:
        b = sum(q for _, q, _ in self.depth(Side.BUY, levels))
        a = sum(q for _, q, _ in self.depth(Side.SELL, levels))
        return (b - a) / (b + a) if a + b else 0.0

    def __len__(self) -> int:
        return len(self._orders)
