# Lock-Free Market-Data Pipeline

`include/mds/pipeline/spsc_ring.hpp` · `mutex_queue.hpp` · `pipeline.hpp`

## 1. The ring buffer

```
           head_ (consumer writes)            tail_ (producer writes)
                 │                                   │
   slots: [ . . . X X X X X X X X X X X X X . . . . . ]   capacity = 2^k
                 └────────── tail - head ────────────┘
   index = counter & (capacity - 1)
```

* **Free-running 64-bit counters.** `head_` and `tail_` only ever increase.
  The slot is `counter & mask`; size is `tail - head`; full is
  `tail - head == capacity`. Unlike the classic "leave one slot empty"
  scheme, all `capacity` slots are usable, and there is no modulo or branch
  on wrap. A 64-bit counter at 10⁹ msg/s wraps after ~585 years.
* **Power-of-two capacity** makes the index a single `AND`.
* **Pre-constructed slots** (`std::unique_ptr<T[]>`): no allocation after
  construction; push/pop are move-assignments into existing storage.

## 2. Memory ordering - why it is correct

There are two threads, P (producer) and C (consumer), and two shared atomics.
Each atomic has **exactly one writer**, so no read-modify-write is ever needed.

**Push (P):**
```cpp
t = tail_.load(relaxed);                 // P is the only writer: relaxed is exact
if (t - cached_head == cap) {
    cached_head = head_.load(acquire);   // (A1) pairs with C's release (R2)
    if (t - cached_head == cap) return false;
}
slots[t & mask] = value;                 // (W) plain write
tail_.store(t + 1, release);             // (R1) publishes W
```

**Pop (C):**
```cpp
h = head_.load(relaxed);                 // C is the only writer
if (h == cached_tail) {
    cached_tail = tail_.load(acquire);   // (A2) pairs with R1
    if (h == cached_tail) return false;
}
out = slots[h & mask];                   // (Rd) plain read
head_.store(h + 1, release);             // (R2) "slot h is free"
```

* **Visibility of data (R1 → A2).** If C's acquire load (A2) reads the value
  written by P's release store (R1), then everything sequenced-before R1 in P -
  in particular the slot write (W) - *happens-before* everything after A2 in C,
  including the slot read (Rd). C therefore never observes a half-written
  element.
* **Safe slot reuse (R2 → A1).** P only overwrites slot `h & mask` after
  observing `head_ > h` via an acquire load that reads from C's release store
  (R2). So C's read (Rd) of that slot happens-before P's subsequent write -
  no write-after-read race.
* **Relaxed own-index loads.** A thread reading an atomic only it writes
  always sees its latest value (sequenced-before), so `relaxed` suffices.
* **No `seq_cst` anywhere.** We need pairwise ordering between two threads,
  not a single total order across all atomics.

On x86-64 (TSO) the acquire loads and release stores compile to **plain
`MOV`s** - no `LOCK` prefix, no `MFENCE`. The only cost of synchronisation is
the cache-coherence traffic of the counters themselves. On AArch64 they become
`LDAR` / `STLR`, still far cheaper than a lock.

ThreadSanitizer (CI job `sanitize: thread`) validates the absence of data
races under the stress tests in `tests/test_pipeline.cpp`.

## 3. Cache behaviour

### False sharing

If `head_` and `tail_` shared a 64-byte line, every push would invalidate the
consumer's copy of the line and every pop would invalidate the producer's -
even though they write different variables. The ring places:

```
[ pad0 64B ][ ProducerState: tail_, cached_head ][ pad1 64B ][ ConsumerState: head_, cached_tail ][ pad2 64B ]
```

* Each side's hot fields live on **their own cache line**, which that side
  writes and the other only occasionally reads.
* The **extra padding line** defeats the Intel L2 *adjacent-line (spatial)
  prefetcher*, which fetches lines in 128-byte aligned pairs and would
  otherwise re-couple neighbouring lines.
* The private snapshot (`cached_head` / `cached_tail`) is on the *owner's*
  line, so updating it causes no cross-core traffic.

### Index caching

The naive SPSC ring loads the *other* side's counter on every operation,
forcing a coherence miss whenever it has changed (i.e. almost always under
load). With caching, the producer re-reads `head_` **only when its cached copy
says the ring is full**, and the consumer re-reads `tail_` **only when its copy
says the ring is empty**. With a mostly non-empty ring, a cross-core load
happens roughly once per `capacity` messages per side rather than once per
message. (Technique popularised by Rigtorp's `SPSCQueue` and Vyukov.)

### Batching

`try_pop_n(dst, max)` drains everything available with **one** acquire load
and **one** release store, then the consumer processes the batch from its own
L1. `MarketDataPipeline` defaults to batches of 64. `try_push_n` provides the
same on the producer side.

## 4. The mutex baseline (fair comparison)

`MutexQueue<T>` uses **the same storage** - a power-of-two ring with
free-running counters - and the same non-blocking `try_push` / `try_pop`
interface, guarded by a `std::mutex`. `MarketDataPipeline<Queue>` is a
template, so both run the identical producer/consumer loops, back-off,
batching, latency sampling and order-book work. The benchmark therefore
isolates *only* the synchronisation strategy.

Per operation, the mutex version pays for:

| Cost | Mutex ring | Lock-free ring |
|---|---|---|
| Atomic RMW (`LOCK CMPXCHG` / `XCHG`) | 2 (lock + unlock) | **0** |
| Shared line written by both threads | mutex word + head + tail, every op | none (each line has one writer) |
| Cross-core coherence misses | ~every op (line ping-pong) | ~once per `capacity` ops (cached indices) |
| Kernel involvement under contention | futex / SRWLock wait + wake | never |
| Progress guarantee | blocking | wait-free (bounded steps) |

Expect the gap to be largest in the **raw queue** benchmark (where queue cost
is all there is) and with **single-item pops**; batching narrows it because
the mutex is then taken once per 64 items. In the **full pipeline** the
order-book work on the consumer is the bottleneck, so the end-to-end speed-up
is smaller than the raw speed-up - `bench_queue` reports both so the numbers
can be quoted honestly. See [PERFORMANCE.md](PERFORMANCE.md).

## 5. Latency measurement

The producer stamps `enqueue_ns = mono_ns()` on every 64th envelope (0
otherwise); the consumer records `mono_ns() - enqueue_ns` *after* the event has
been applied to the book. This is the **enqueue → book-updated** latency
including queueing delay. Sampling keeps the two `steady_clock` reads
(~20 ns each) off 63/64 of messages so the measurement does not distort the
throughput it accompanies. Both threads use the same monotonic clock source,
so cross-thread differences are meaningful.

## 6. Back-pressure and waiting

* A full ring makes `try_push` return false; the producer backs off
  (`Backoff`: 1, 2, 4 … 64 × `PAUSE`, then `yield`). Stalls are counted
  (`producer_stalls`) - a persistently non-zero count means the consumer is the
  bottleneck and the capacity only buys burst absorption.
* An empty ring does the same on the consumer side (`consumer_idle`).
* `PAUSE` matters: it de-pipelines the spin loop (avoiding a memory-order
  machine clear when the line finally changes) and yields resources to the
  sibling hyper-thread.

## 7. API summary

```cpp
SpscRing<T> q(capacity);          // rounded up to a power of two
bool   q.try_push(value);          // producer only
size_t q.try_push_n(src, n);       // producer only
bool   q.try_pop(out);             // consumer only
size_t q.try_pop_n(dst, max);      // consumer only
T*     q.front(); q.pop_front();   // consumer only, zero-copy
size_t q.size_approx();            // either side, a snapshot
```

Violating the single-producer / single-consumer contract is undefined
behaviour; the type does not (and cannot cheaply) detect it.
