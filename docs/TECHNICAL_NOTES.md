# Technical Notes

Design decisions (ADR style), trade-offs, limitations and a FAQ.

---

## ADR-1: Fixed-point integer prices

**Decision.** `Price` is `int64` with 4 implied decimals.

**Why.** Prices are `std::map` keys; floating-point keys make level identity
depend on rounding error. Integer compare/add is also cheaper and exact, and
tick arithmetic (`floor_to_tick`) becomes integer division.

**Cost.** Conversions at I/O boundaries; a fixed scale (adjust `kPriceScale`
for instruments needing more precision, e.g. FX).

## ADR-2: `std::map` + `std::deque` for the book

**Decision.** Ordered map of price → level; each level a deque of
`{id, seq}`; a hash map id → order.

**Alternatives.**

| Option | Best price | Insert level | Cancel | Memory | Verdict |
|---|---|---|---|---|---|
| `std::map` + `std::deque` (chosen) | O(1) | O(log L) | O(1) lazy | proportional | general, robust |
| Flat array indexed by tick offset | O(1) | O(1) | O(1) | fixed band | fastest for one liquid name; needs re-centering |
| Sorted `std::vector` of levels | O(1) | O(L) | O(1) lazy | compact, cache-friendly | good when L is tiny |
| `std::list` per level + iterator in order map | O(1) | O(log L) | O(1) exact | node per order, cache-hostile | classic but slower in practice |

The map/deque design handles arbitrary price ranges with no tuning, and
lazy cancellation recovers the O(1) cancel that intrusive lists are usually
chosen for, without per-order allocation.

## ADR-3: Lazy (tombstone) cancellation with sequence numbers

**Decision.** Cancels fix aggregates immediately but leave the queue entry;
the matcher discards stale entries; levels compact when tombstones > live.

**Why.** `deque::erase` in the middle is O(n). Real flow is cancel-heavy (the
synthetic feed is ~32 % cancels; real equities markets are often > 90 %), so
cancel cost dominates.

**Correctness hinges on** the `seq` check: an entry is live iff
`orders_[id]` exists **and** has the same `seq`. This handles id reuse and
size-up re-queueing.

## ADR-4: SPSC instead of MPSC/MPMC

**Decision.** One producer, one consumer, per pipeline.

**Why.** A single instrument's book is sequential, so there is exactly one
consumer. One feed handler per line means one producer. The SPSC contract is
what allows zero RMW operations. Multi-instrument scale-out is achieved by
running independent pipelines (sharding by symbol), not by sharing a queue.

## ADR-5: Mutex baseline with identical storage

**Decision.** The baseline is a mutex around the *same* ring, not a
`std::queue` with a condition variable.

**Why.** A deque-plus-condvar baseline would conflate allocation, notify
syscalls and data-structure differences with locking. Matching storage makes
the benchmark answer one question: *what does the lock cost?*

## ADR-6: Two Redis representations

**Decision.** RedisTimeSeries for analytics series + binary chunks for the
lossless event log. See [REDIS_STORAGE.md](REDIS_STORAGE.md).

**Rejected.** *Redis Streams* (`XADD` per event): convenient consumer-group
semantics, but ~100 B overhead per 40 B event and field/value encoding cost on
both ends. *One key per tick*: catastrophic key overhead. *Only
RedisTimeSeries*: cannot store order ids / cancels needed to rebuild queues.

## ADR-7: No third-party dependencies

**Decision.** Own the RESP client, the test framework and the CLI parser.

**Why.** Builds anywhere with a C++17 compiler + CMake, including offline and
in minimal CI images; every line on the critical path is reviewable. The
pieces are small (RESP client ~250 lines, test framework ~100).

**When to revisit.** For production: hiredis/redis-plus-plus (TLS, RESP3,
cluster redirects), GoogleTest + Google Benchmark (richer reporting).

## ADR-8: Virtual (no-impact) strategy orders

**Decision.** Simulated orders never enter the replayed book.

**Why.** Replaying recorded flow *and* inserting our orders would require
re-simulating how every subsequent participant would have reacted - which is
unknowable. Keeping the historical flow fixed is the standard, explainable
choice. Queue position is still modelled exactly (see
[BACKTESTING.md](BACKTESTING.md)).

---

## Memory-model checklist (SPSC ring)

* [x] Each atomic has a single writer → no RMW required.
* [x] Data written before release-store of the index; read after the
      acquire-load that observes it.
* [x] Slot reused only after acquire-load of the consumer's release-store.
* [x] Own index loaded `relaxed`.
* [x] Producer/consumer state on separate lines + adjacent-line padding.
* [x] `static_assert(std::atomic<uint64_t>::is_always_lock_free)`.
* [x] Tested under ThreadSanitizer with tiny rings (constant full/empty).

## Timing

`steady_clock` (≈20-25 ns per read via vDSO / QPC) is used for latency.
`RDTSC` would be ~5-10 ns but requires invariant-TSC detection, frequency
calibration and care around core migration; for 1-in-64 sampling the
difference is immaterial. Exchange timestamps (`MarketEvent::ts`) are
simulated nanoseconds and unrelated to wall time.

## Limitations

* Single venue, continuous trading only (no auctions, halts, LULD bands).
* No hidden/iceberg/pegged order types, no self-trade prevention.
* Backtester: no market impact; taker walk doesn't account for our own
  earlier takes in the same event.
* Binary formats assume a little-endian host.
* RESP2 only; no TLS/AUTH/cluster redirection in the C++ client.
* The synthetic feed is a stylised model (Poisson arrivals, Brownian fair
  value, informed takers, linear permanent impact), not calibrated to a
  specific market. Strategy PnL on it is explainable, not predictive.

## Roadmap

1. Flat-array price ladder variant of `OrderBook` behind the same interface,
   with a benchmark comparing it to the map version.
2. ITCH 5.0 / PCAP replay source for the pipeline.
3. Multi-symbol sharding: N pipelines, one per core, symbol-hash routing.
4. Huge-page backed ring and order pool; NUMA-aware placement.
5. Market-impact model (e.g. square-root law) as an optional backtest mode.
6. Parameter sweeps (latency, spread, skew) driven from Python, results in
   Redis, plotted with `analyze_backtest.py`.

---

## FAQ / discussion points

**Why is the ring wait-free and not just lock-free?**
Each `try_push` / `try_pop` completes in a bounded number of steps
regardless of what the other thread does (no CAS retry loop). Lock-free only
guarantees *system-wide* progress; wait-free guarantees per-operation
progress.

**What breaks if two threads push?**
Both read the same `tail`, both write the same slot, both store `tail + 1`:
one message is lost and the consumer may read a torn value. It is a
contract, not a checked condition.

**Why `memory_order_acquire` on the peer index and not `relaxed`?**
`relaxed` would allow the consumer to observe the new `tail_` but read the
slot's *old* contents (no happens-before edge). On x86 this rarely shows up
in practice because of TSO, but it is a real bug on ARM and is UB per the
C++ model regardless.

**Why does batching help the mutex more than the ring?**
The mutex's cost is per *acquisition*; batching divides it by the batch
size. The ring's per-message cost is already near the cost of copying the
slot, so there's less to amortise.

**How do you know the order book is right?**
Three independent implementations agree: the C++ book, a brute-force C++
reference in the differential test, and the Python reference book. The
pipeline test additionally checks that a book rebuilt on another thread from
the event stream is identical, level for level, to the exchange's shadow
book.

**Why would a backtest still be optimistic?**
No impact, perfect knowledge of displayed queue ahead, no exchange
throttling, deterministic latency. The docs list each bias; sweeping latency
and fees shows how sensitive a result is to them.

**How would you take this to production?**
Kernel-bypass feed input (DPDK / Solarflare ef_vi), flat-array books,
pre-allocated object pools, huge pages, core isolation (`isolcpus`,
`nohz_full`), per-symbol sharding, a binary journal on local NVMe with
asynchronous export to Redis / a column store, and RESP3/TLS/cluster support
in the storage client.
