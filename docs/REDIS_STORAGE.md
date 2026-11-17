# Redis Storage

`include/mds/storage/resp.hpp` · `redis_client.hpp` · `tick_store.hpp`

## 1. Two readers, two representations

| Reader | Needs | Representation |
|---|---|---|
| Analyst / dashboard | "mid every second for the last hour", OHLC bars, volume per minute, filter by symbol | **RedisTimeSeries** series, one per field, down-sampled to 1 ms, server-side aggregation |
| Backtester | every order event, in order, byte-exact | **Binary event chunks**: 4 096 packed 40-byte `MarketEvent`s per key, indexed by a sorted set |

Trying to serve both from one structure fails one of them: per-tick keys or
stream entries carry ~50-100 bytes of overhead per 40-byte event, while a
time-series store cannot hold the order-level detail (ids, cancels, modifies)
needed to rebuild queues.

## 2. Key schema

All keys for a symbol share the `{SYM}` **hash tag**, so in Redis Cluster
they hash to the same slot and multi-key commands (`MGET`, `TS.MADD`, `DEL`)
remain legal.

| Key | Type | Content |
|---|---|---|
| `mds:{SYM}:ts:bid` | TimeSeries | best bid, last value per ms |
| `mds:{SYM}:ts:ask` | TimeSeries | best ask |
| `mds:{SYM}:ts:mid` | TimeSeries | (bid + ask) / 2 |
| `mds:{SYM}:ts:spread` | TimeSeries | ask − bid |
| `mds:{SYM}:ts:imbalance` | TimeSeries | top-5 level imbalance ∈ [−1, 1] |
| `mds:{SYM}:ts:trade_px` | TimeSeries | last trade price per ms |
| `mds:{SYM}:ts:trade_qty` | TimeSeries | traded volume per ms (`DUPLICATE_POLICY SUM`) |
| `mds:{SYM}:chunk:<n>` | String (binary) | `n`-th chunk of packed events |
| `mds:{SYM}:chunks` | Sorted set | member `n`, score = first event ts in **ms** |
| `mds:{SYM}:meta` | Hash | `events`, `chunks`, `first_ts`, `last_ts` (ns), `schema=MDSEVT01`, `event_bytes=40`, `backend` |

Series are created with labels `symbol=<SYM> field=<field> source=mds`, which
enables cross-symbol queries such as:

```
TS.MRANGE - + AGGREGATION avg 60000 FILTER field=mid source=mds
```

### Why scores are milliseconds

Sorted-set scores are IEEE doubles with 53 bits of mantissa. Epoch
nanoseconds (~1.7 × 10¹⁸) exceed 2⁵³ ≈ 9 × 10¹⁵ and would lose precision;
epoch milliseconds (~1.7 × 10¹²) are exact. Exact ns timestamps live inside
the chunk payload, and `load_events` filters on them after decoding.

## 3. Write path

### Down-sampling rules

RedisTimeSeries timestamps are milliseconds. A busy book updates many times
per ms, so the store keeps a **pending snapshot** and only emits it when the
millisecond changes - i.e. each series holds the **last** book state in each
ms. Trades are accumulated per ms: last price and **summed** quantity. The
series are additionally created with `DUPLICATE_POLICY LAST` (book fields) and
`SUM` (`trade_qty`), so a later flush that revisits a millisecond is merged
correctly by the server instead of rejected.

### Batching and pipelining

* Samples are accumulated into **`TS.MADD`** commands of up to 512
  `(key, ts, value)` triples - one command writes all seven series.
* Commands are **pipelined**: `RedisClient::append` only encodes into an
  output buffer; `flush()` sends the whole buffer and then reads all replies.
  The store flushes every 256 commands.

Round trips dominate a naive client (≥ 50 µs each over loopback, more over a
network). With batching + pipelining, ingesting 1 M events takes a few
hundred round trips instead of millions; `mds_sim --redis` prints the exact
count (`round trips`) and bytes sent.

### Event chunks

Events are appended to an in-memory buffer; every 4 096 events the store
pipelines:

```
SET      mds:{SYM}:chunk:<n>  <163 840 bytes>
ZADD     mds:{SYM}:chunks     <first_ts_ms> <n>
HINCRBY  mds:{SYM}:meta events 4096
HSETNX   mds:{SYM}:meta first_ts <ns>
HSET     mds:{SYM}:meta chunks <n+1> last_ts <ns>
```

Chunk numbering resumes from `meta.chunks`, so multiple ingestion sessions
append rather than overwrite.

## 4. Read path

### Series

```
TS.RANGE mds:{SYM}:ts:mid <from_ms> <to_ms> [AGGREGATION avg|min|max|first|last|sum|count <bucket_ms>]
```

Aggregation runs **inside Redis** - a day of 1 ms mid samples becomes a few
hundred 1-minute bars before it touches the network. `mds_query` and
`python/scripts/redis_inspect.py` build OHLC bars from
`first / max / min / last` and volume from `sum` on `trade_qty`.

### Event replay

```
ZREVRANGEBYSCORE chunks (<from_ms -inf LIMIT 0 1     # chunk that starts before the window
ZRANGEBYSCORE    chunks  <from_ms> <to_ms>           # chunks that start inside it
MGET chunk:<a> chunk:<b> ...                         # 64 keys per call
```

The two index queries are pipelined (one round trip), then chunk bodies are
fetched with `MGET`, decoded with a `memcpy` per event and filtered to the
exact ns window. Loading is bounded by network bandwidth, not by command
overhead.

## 5. Fallback: vanilla Redis

`TickStore::init()` probes with `TS.INFO <key>`. If the reply is an
"unknown command" error, the store switches to sorted sets:

| Operation | RedisTimeSeries | Fallback |
|---|---|---|
| write sample | `TS.MADD key ts value ...` | `ZADD key <ts_ms> "<ts_ms>:<value>"` |
| range | `TS.RANGE` | `ZRANGEBYSCORE` |
| aggregation | server-side | client-side `TickStore::aggregate`, epoch-aligned buckets, same semantics |

The member embeds the timestamp so equal values at different times remain
distinct members. `python/tests/test_redis_roundtrip.py` asserts that both
backends return **identical** raw and aggregated data for the same ingest.

## 6. The RESP client

* **RESP2 encoder** produces `*N\r\n$len\r\narg\r\n...` - binary safe, so
  chunk payloads with arbitrary bytes are sent verbatim.
* **Incremental decoder** (`RespParser`) parses from a buffer that may hold a
  partial reply; it returns `nullopt` until a full value is present, so socket
  reads of any size work. It is tested byte-by-byte.
* **Socket**: `getaddrinfo` (IPv4/IPv6), connect/recv/send timeouts,
  `TCP_NODELAY` (Nagle + delayed ACK would add up to 40 ms per small request),
  `MSG_NOSIGNAL` on Linux, Winsock on Windows.
* Error replies are values, not exceptions, so a pipeline with one failing
  command still returns all replies.

No hiredis dependency: the project needs exactly single commands and
pipelines, and ~250 lines of owned code keeps the build to a compiler and
CMake.

## 7. Operations

* **Persistence**: the compose file enables AOF (`appendfsync everysec`).
* **Retention**: `TickStoreConfig::retention_ms` sets `RETENTION` on series;
  chunks can be aged out with `ZRANGEBYSCORE ... -inf <cutoff>` + `DEL`.
* **Memory estimate**: events ≈ 40 B × N (+ ~0.03 B/event of key overhead at
  4 096 events/chunk); series ≈ 7 × samples × ~16 B (RedisTimeSeries
  compressed chunks are typically smaller).
* **Inspect**: `python python/scripts/redis_inspect.py --redis host:port`
  prints meta, per-series counts, OHLC bars and verifies the event log decodes
  and replays.
* **Testing without Redis**: `python/scripts/fake_redis.py` implements every
  command used (including TS.* with duplicate policies and aggregation) over a
  real TCP socket; CI runs the C++ binaries against it.
