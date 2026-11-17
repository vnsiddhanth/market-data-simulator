# Performance

## 1. Running the benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMDS_NATIVE=ON
```

```bash
cmake --build build --parallel
```

```bash
./build/bench_queue --messages 20000000 --events 2000000 --reps 5 --pin 2,4
```

```bash
./build/bench_orderbook --events 2000000 --reps 5
```

```bash
./build/mds_backtest --strategy mm --events 2000000
```

Or all of them, with machine info, into a Markdown file:

```bash
python python/scripts/run_benchmarks.py --build build --pin 2,4 --out docs/BENCHMARK_RESULTS.md
```

## 2. Methodology

| Principle | How it is applied |
|---|---|
| Compare like with like | `MutexQueue` and `SpscRing` share storage layout, capacity, payload (48 B), API and the *same templated pipeline code*; only synchronisation differs |
| Report the median | each configuration runs `--reps` times; the median is reported (robust to one noisy run) |
| Validate while measuring | the raw benchmark checks strict FIFO order and a checksum on every message, and the variants' checksums must agree; the pipeline benchmark rebuilds a full book |
| Separate layers | **raw** (queue only) vs **pipeline** (queue + order book) vs **order book** (single-threaded) vs **backtest** |
| Keep timers off the hot path | throughput passes have no clock reads per message; latency uses 1-in-64 sampling (pipeline) or a separate pass (order book) |
| Control placement | `--pin P,C` pins producer/consumer; pick two **physical** cores (not SMT siblings) on the same socket |
| Warm, steady state | events are generated before timing starts; the book is pre-reserved |

## 3. What to expect, and why

### Raw queue (`bench_queue`, "raw")

The lock-free ring's per-message cost is a slot copy plus, rarely, a
cross-core load of the peer's counter. The mutex ring's per-message cost
includes two atomic RMWs and a cache-line transfer of the mutex word on
nearly every operation, plus occasional kernel-assisted waits under
contention. Expect:

* **pop-1 (like-for-like)**: the largest ratio - a multiple, not a percentage.
* **batch-64**: both get faster; the mutex benefits more (one lock per 64
  items), so the ratio shrinks.
* Pinned runs on separate physical cores give the most stable numbers;
  unpinned runs on a busy laptop are noisy.

### Pipeline (`bench_queue`, "pipeline")

The consumer now also runs `OrderBook::apply` per event. Once the queue is
no longer the bottleneck, the end-to-end ratio converges towards
`book_cost / (book_cost + queue_cost)`. The lock-free ring also shows a much
tighter latency tail (p99) because it never parks a thread.

Note that `replay` pre-generates events, so the producer runs far ahead of
the consumer and keeps the ring full; the reported enqueue → applied latency
is then dominated by queueing backlog (milliseconds with a 64K ring). For
per-hop latency, use `mds_sim --live` (producer generates on the fly) or a
small `--capacity`.

### Order book (`bench_orderbook`)

Per-op latencies include ~20-40 ns of `steady_clock` overhead. Cancels are
the cheapest operation (hash erase + level arithmetic, no deque surgery);
market orders vary with the number of levels swept. The `compactions`
counter shows how often lazy cancellation had to rebuild a level.

### Backtest (`mds_backtest`)

Throughput is close to the single-threaded order-book rate; strategy
callbacks and fill simulation add a modest constant per event. The design
target is ≥ 10 K ticks/s.

## 4. Quoting results

Quote numbers together with:

* the machine (CPU model, cores, OS) and compiler,
* which benchmark layer (raw vs pipeline),
* pop-1 vs batch, pinned vs unpinned,
* median of how many runs.

Template: *"&lt;ratio&gt;× (median of 5) over a like-for-like mutex ring on raw
SPSC transfer, pop-1, pinned to two physical cores, &lt;CPU&gt;, &lt;compiler&gt;
-O3"*.
`run_benchmarks.py` records all of this automatically.

## 5. Tuning knobs

| Knob | Where | Effect |
|---|---|---|
| `--capacity` | `mds_sim`, `bench_queue` | larger absorbs bursts; beyond L2 size it costs cache misses |
| `--batch` | `mds_sim` | consumer drain size; 32-128 is usually optimal |
| `--pin P,C` | all multi-threaded tools | core placement |
| `MDS_NATIVE=ON` | CMake | `-march=native` |
| `orders_.reserve` | `OrderBook` ctor | avoids rehash stalls |
| `chunk_events`, `pipeline_depth`, `madd_batch_samples` | `TickStoreConfig` | Redis ingest batching |

## 6. Profiling tips

* Linux: `perf stat -e cycles,instructions,cache-misses,LLC-load-misses` on
  `bench_queue --raw-only` shows the coherence-miss difference between the
  two queues directly; `perf c2c` pinpoints false sharing (try removing the
  padding in `SpscRing` to see it appear).
* `perf record -g ./build/bench_orderbook` - expect the hash lookups in
  `orders_` and `std::map` traversal to dominate.
* Windows: VTune "Memory Access" or WPA.
