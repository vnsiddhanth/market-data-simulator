# Python tooling

Stdlib-only helpers around the C++ simulator. Every module works on a stock
Python 3.9+; `pandas` and `matplotlib` are used only if installed.

## Package `mds`

| Module | Purpose |
|---|---|
| `codec.py` | `MarketEvent` codec, byte-for-byte compatible with the C++ struct (`<qQqqIBBH`, 40 bytes); `.bin` (MDSEVT01) and CSV readers |
| `resp.py` | Minimal RESP2 client with pipelining (`append` / `flush`) |
| `store.py` | Mirror of the C++ `TickStore` schema: `range()` with server- or client-side aggregation, `load_events()`, `ingest()` |
| `book.py` | Independent reference order book (price-time priority) for cross-checking |
| `analysis.py` | Equity-curve metrics (Sharpe, Sortino, drawdown), fill statistics, mark-outs |

## Scripts

| Script | Use |
|---|---|
| `scripts/fake_redis.py` | In-memory RESP server with Strings/Hashes/Sorted sets and TS.* commands; `--no-ts` emulates vanilla Redis |
| `scripts/ingest_events.py` | Load a `.bin` / `.csv` event file into Redis with the C++ schema |
| `scripts/redis_inspect.py` | Show meta, series counts, OHLC bars, and verify the event log |
| `scripts/verify_events.py` | Replay an event file through the Python book and print depth |
| `scripts/analyze_backtest.py` | Recompute and cross-check `mds_backtest` metrics, mark-outs, plots |
| `scripts/run_benchmarks.py` | Run all C++ benchmarks and write `docs/BENCHMARK_RESULTS.md` |

## Example session

Run from the repository root (create the output folder first with `mkdir out`).

```bash
python python/scripts/fake_redis.py --port 6399
```

```bash
./build/mds_sim --events 300000 --queue spsc --redis 127.0.0.1:6399 --bin out/ticks.bin
```

```bash
python python/scripts/redis_inspect.py --redis 127.0.0.1:6399 --bars 1000
```

```bash
python python/scripts/verify_events.py out/ticks.bin
```

```bash
./build/mds_backtest --strategy mm --redis 127.0.0.1:6399 --report out/mm.json --equity out/mm_eq.csv --fills out/mm_fills.csv
```

```bash
python python/scripts/analyze_backtest.py --equity out/mm_eq.csv --fills out/mm_fills.csv --report out/mm.json --plot out/mm.png
```

## Tests

```bash
cd python && python -m unittest discover -s tests -v
```

`test_redis_roundtrip.py` starts two fake servers (with and without
TimeSeries) on free ports and verifies lossless event replay, time-window
queries, series values against the reference book, volume conservation and
that both backends return identical aggregates.
