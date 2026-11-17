# Changelog

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
