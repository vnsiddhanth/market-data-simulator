# Order Book

`include/mds/orderbook/order_book.hpp` · `src/order_book.cpp`

## Data structures

```
bids_  : std::map<Price, PriceLevel, std::greater<>>      begin() == best bid
asks_  : std::map<Price, PriceLevel, std::less<>>         begin() == best ask

PriceLevel
  queue      : std::deque<QueueEntry{id, seq}>   FIFO (time priority)
  total_qty  : Qty                               exact displayed size
  live       : uint32                            live orders at level
  tombstones : uint32                            cancelled entries still queued

orders_ : std::unordered_map<OrderId, Order>     id -> {price, qty, side, ts, seq}
```

### Why `std::map` for price levels

* **Ordered iteration** is the core requirement: best price, depth snapshots,
  sweeping a market order across levels and imbalance over the top *N* all walk
  levels in price order.
* `begin()` is O(1) (libstdc++/libc++ cache the leftmost node), so best
  bid/ask is constant time.
* Insert/erase of a level is O(log L) with **iterator stability** - other
  levels never move, which keeps the matcher simple.
* L (populated levels) is small in practice (tens to low hundreds), so
  log L is a handful of pointer hops.

Alternatives considered (see [TECHNICAL_NOTES.md](TECHNICAL_NOTES.md#adr-2)):
a flat array indexed by tick offset (O(1) everything, but needs a bounded
price band and re-centering), or a sorted `std::vector` (cache-friendly but
O(L) insert). `std::map` is the best general-purpose choice; the array is the
natural next step for a single liquid instrument.

### Why `std::deque` for the FIFO

* `push_back` / `pop_front` are O(1) and **do not invalidate references** to
  other elements.
* Elements are stored in fixed-size blocks, so iteration is far more
  cache-friendly than `std::list` and there is no per-node allocation.
* `QueueEntry` is 16 bytes, so a 512-byte deque block holds 32 orders.

The catch: erasing from the *middle* is O(n) - which is exactly what a cancel
needs. Hence lazy cancellation.

## Lazy cancellation (tombstones)

A cancel:

1. erases the order from `orders_` (O(1) average),
2. decrements the level's `total_qty` and `live` **immediately** - depth and
   imbalance are always exact,
3. increments `tombstones`; the queue entry is left in place.

The matcher, when it meets a queue head whose id is missing from `orders_`
(or whose `seq` differs), pops it and decrements `tombstones`.

**Sequence numbers prevent ABA.** Each resting order gets a monotonically
increasing `seq`. If an id were reused, a stale tombstone could otherwise be
mistaken for the new live order. The check `orders_[id].seq == entry.seq`
makes that impossible. The same mechanism implements priority loss on
size-up: the old entry becomes a tombstone and the order is re-queued at the
back with a fresh `seq`.

**Compaction bounds memory.** When `tombstones >= 64 && tombstones > live`, the
level's deque is rebuilt with only live entries. The rebuild is O(n) but is
paid at most once per *n/2* cancels at that level, so the amortised cost per
cancel stays O(1), and queue memory is bounded by ~2× live orders.

If a level's last live order is cancelled, the whole level is erased from the
map, discarding its tombstones for free.

## Matching algorithm

```
match(opposite_levels, aggressor, limit, qty):
  while qty > 0 and opposite not empty:
      level = opposite.begin()                         # best price
      if not crosses(level.price, limit): break        # buy: ask <= limit
      while qty > 0 and level.queue not empty:
          head = level.queue.front()
          if head is tombstone: pop, --tombstones; continue
          traded = min(qty, maker.qty)
          emit Fill{maker, taker, level.price, traded}
          maker.qty -= traded; level.total_qty -= traded; qty -= traded
          if maker.qty == 0: pop, --live, erase from orders_
      if level.live == 0: erase level
  return filled
```

* Trades print at the **maker's** price - an aggressive order priced through
  the book receives price improvement.
* Market orders use `limit = ±∞` and never rest (IOC semantics).
* Limit orders rest any unfilled remainder at their limit price.

## Operations and complexity

| Operation | Cost | Notes |
|---|---|---|
| `best_bid / best_ask / spread / mid` | O(1) | `map::begin()` |
| `add_limit` (rests, existing level) | O(log L) | find level + `push_back` |
| `add_limit` (rests, new level) | O(log L) | tree insert |
| `add_limit` (crosses, k fills over m levels) | O(k + m log L) | |
| `cancel` | O(1) avg + O(log L) level lookup | lazy; amortised compaction |
| `modify` size-down | O(1) avg + O(log L) | in place, keeps priority |
| `modify` size-up | O(log L) | tombstone + re-queue |
| `depth(side, n)` / `imbalance(n)` | O(n) | aggregates are pre-computed |
| `volume_at(side, price)` | O(log L) | |

`orders_` is created with `max_load_factor(0.5)` and a reserve of 65 536
entries, so normal operation never rehashes on the hot path (a rehash of a
large table is a multi-millisecond stall).

## Exchange semantics implemented

| Behaviour | Implemented as |
|---|---|
| Price-time priority | map ordering + deque FIFO |
| Partial fills | maker remains at queue head with reduced qty |
| Price improvement | trade at resting price |
| Market order with insufficient liquidity | fills what exists, remainder dropped |
| Cancel unknown / already-filled order | `UnknownOrder`, counted as reject |
| Duplicate live id | `DuplicateOrder` reject |
| Size-down keeps queue position | in-place qty change |
| Size-up loses queue position | re-queued at back |
| Self-trade prevention | *not implemented* (synthetic feed has no owner ids) |
| Hidden / iceberg / pegged orders | *not implemented* (see roadmap) |

## Invariants (`OrderBook::validate()`)

Checked in tests after randomized operation sequences:

1. Every level in the map has `live > 0` (empty levels are erased).
2. For each level, `total_qty` equals the sum of its live orders' `qty`, and
   `live` equals their count.
3. `queue.size() == live + tombstones`.
4. Every live order is reachable from exactly the level matching its price
   and side; the number of reachable orders equals `orders_.size()`.
5. The book is never crossed: `best_bid < best_ask`.

## Verification

`tests/test_order_book.cpp` contains targeted unit tests plus a
**differential test**: 20 000 random adds / cancels / modifies / market orders
over a 21-tick price band are applied to both `OrderBook` and a deliberately
naive O(n) reference implementation. Every fill (maker id, price, qty) must
match exactly after every operation, and full depth plus `validate()` are
compared periodically. The Python package contains a third independent
implementation (`python/mds/book.py`) used to cross-check event files.
