# Changelog

## 2.0.0

- **Web app**: live market dashboard (order book ladder, price chart with strategy fills, trade tape, latency, live PnL), Simulate / Backtest / Benchmarks / History / About pages, presets and inline explanations, light/dark themes, mobile layout.
- **`mds_live`**: real-time engine pacing the synthetic exchange to the wall clock through the SPSC ring, with live paper trading and JSON Lines snapshots.
- **Backend** (`web/mds_web`, stdlib only): validated job API with worker pool, persistence, cancellation and downloads; live market manager with SSE fan-out, auto-restart and pause-during-benchmarks; optional access token; strict CSP and security headers.
- Backtester incremental API (`begin` / `step` / `finish`) and bounded-memory live mode.
- `--json` outputs for `mds_sim`, `bench_queue` and `bench_orderbook`.
- Production `Dockerfile`, `compose.yaml` (with Redis Stack), `render.yaml`, `fly.toml`; CI builds and smoke-tests the container.

## 1.1.0

- Synthetic feed: informed takers (trade with the next fair-value innovation) and permanent price impact on deliberate aggressors; defaults changed to 2 000 events/s and 1.5 ticks/sqrt(s). Market making is no longer risk-free in simulation.
- `mds_backtest`: `--rate`, `--vol`, `--informed`, `--impact`; Sharpe output reports its sample count and flags fewer than 250 samples.
- Imbalance taker recalibrated (5 levels, entry 0.6, exit 0.2).
- Benchmarks re-run on the final code; results in `docs/BENCHMARK_RESULTS.md`.

## 1.0.0

- Price-time-priority `OrderBook` (`std::map` levels, `std::deque` FIFO, lazy tombstone cancels).
- Wait-free `SpscRing` with cached indices, cache-line isolation and batch APIs; `MutexQueue` baseline.
- `MarketDataPipeline<Queue>` with latency sampling and back-pressure accounting.
- Deterministic `SyntheticFeed` (Poisson arrivals, Brownian fair value, shadow exchange book); CSV and MDSEVT01 binary I/O.
- Event-driven `Backtester` with order latency, exact FIFO queue-position fill model, fees, metrics; market-maker and imbalance strategies.
- Dependency-free RESP2 client; `TickStore` with RedisTimeSeries series, binary event chunks and sorted-set fallback.
- Apps: `mds_sim`, `mds_backtest`, `mds_query`; benchmarks: `bench_queue`, `bench_orderbook`.
- Python tooling: codec, RESP client, store mirror, reference book, analytics, fake Redis server.
- CI: GCC/Clang/MSVC/macOS, ASan/TSan, Redis Stack + vanilla Redis integration.
