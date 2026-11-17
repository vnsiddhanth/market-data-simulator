# Getting Started

This guide explains **what the Market Data Simulator is**, **what goes in and
what comes out**, and **exactly how to run it**, step by step, on Windows
(via WSL), Linux and macOS. No prior trading knowledge is assumed.

- [1. What this app is](#1-what-this-app-is)
- [2. Key ideas in 2 minutes](#2-key-ideas-in-2-minutes)
- [3. How the pieces fit together](#3-how-the-pieces-fit-together)
- [4. Inputs](#4-inputs)
- [5. Outputs](#5-outputs)
- [6. Install and build](#6-install-and-build)
- [7. Run it: the one-command demo](#7-run-it-the-one-command-demo)
- [8. Run it: each tool on its own](#8-run-it-each-tool-on-its-own)
- [9. Using a real Redis](#9-using-a-real-redis)
- [10. Running the tests and benchmarks](#10-running-the-tests-and-benchmarks)
- [11. Troubleshooting](#11-troubleshooting)

---

## 1. What this app is

Electronic stock exchanges match buyers and sellers using an **order book**,
and they broadcast every change to that book as **market data**: a stream of
messages like *"order 42: buy 100 shares at $99.98"*, *"order 17 cancelled"*.
Trading firms consume this stream, rebuild the order book in their own
systems, store the history, and test trading ideas against it before risking
real money.

This project is a small, complete version of that whole chain:

| Stage | What it does here | Real-world equivalent |
|---|---|---|
| **Simulate** | Generates a realistic stream of orders, cancels and trades | The exchange's market-data feed |
| **Transport** | Passes messages between two CPU threads with a lock-free queue | A feed handler moving packets to a strategy thread |
| **Rebuild** | Maintains the full order book from the stream | Every trading firm's book builder |
| **Store** | Writes price series and the raw stream to Redis | A tick database |
| **Backtest** | Replays history through a trading strategy and simulates its fills and profit | A research backtesting platform |
| **Analyse** | Python tools that inspect the data and double-check the results | Quant research notebooks |

It is built for **learning and demonstrating low-latency C++ engineering**:
data structures for order books, lock-free multithreading, memory ordering,
binary storage formats, and an honest fill simulation. It does **not**
connect to a real exchange and does **not** trade real money.

## 2. Key ideas in 2 minutes

| Term | Meaning |
|---|---|
| **Order** | An instruction to buy or sell a quantity at a price, e.g. *buy 100 @ $99.98* |
| **Limit order** | Has a price limit. If it can't trade immediately it *rests* in the book and waits |
| **Market order** | Trades immediately at the best available prices; never rests |
| **Bid / Ask** | The best (highest) price someone will buy at / the best (lowest) price someone will sell at |
| **Spread** | Ask − bid. The cost of crossing from one side to the other |
| **Mid** | (bid + ask) / 2 - the usual "current price" |
| **Order book** | All resting orders, grouped by price level, each level a first-come-first-served queue |
| **Price-time priority** | Better price trades first; at the same price, earlier orders trade first |
| **Fill / trade** | When an incoming order matches a resting one |
| **Tick** | One market-data event (or the minimum price increment, $0.01 here) |
| **Imbalance** | (bid size − ask size) / (bid size + ask size), from −1 to +1 |
| **Backtest** | Running a strategy over historical data to see how it would have done |
| **Maker / taker** | A maker's resting order gets hit (earns a small rebate); a taker crosses the spread (pays a fee) |
| **Mark-out** | How the price moved shortly after a fill; negative = you were "picked off" |
| **Lock-free queue** | A way for two threads to exchange data without a mutex, using atomic CPU instructions |

## 3. How the pieces fit together

```
 ┌───────────────┐   MarketEvent   ┌──────────────┐   ┌───────────┐   ┌──────────────┐
 │ SyntheticFeed │ ──────────────► │ SpscRing     │──►│ OrderBook │──►│ TickStore    │──► Redis
 │ (producer     │   40 bytes      │ (lock-free   │   │ (consumer │   │ (time series │
 │  thread)      │                 │  queue)      │   │  thread)  │   │  + chunks)   │
 └───────────────┘                 └──────────────┘   └───────────┘   └──────┬───────┘
                                                                              │ load_events
 ┌─────────────────────┐  report.json / equity.csv / fills.csv   ┌──────────▼───────┐
 │ Python analysis     │ ◄─────────────────────────────────────── │ Backtester       │
 │ (cross-check, plots)│                                          │ + Strategy       │
 └─────────────────────┘                                          └──────────────────┘
```

Programs you will run:

| Program | Language | Purpose |
|---|---|---|
| `mds_sim` | C++ | Generate data, run the multi-threaded pipeline, optionally save to files / Redis |
| `mds_backtest` | C++ | Run a strategy over data from the simulator, a file, or Redis |
| `mds_query` | C++ | Read aggregated price series back from Redis |
| `bench_queue`, `bench_orderbook` | C++ | Performance benchmarks |
| `mds_tests` | C++ | Unit, stress and property tests |
| `fake_redis.py` | Python | A small Redis-compatible server so you don't need to install Redis |
| `redis_inspect.py`, `verify_events.py`, `analyze_backtest.py`, `ingest_events.py` | Python | Inspect, verify, analyse and import data |
| `demo.sh` | Bash | Runs everything above in order |

## 4. Inputs

### 4.1 Command-line options (main input)

Every program is configured with `--option value` flags; `--help` lists them.

**`mds_sim`**

| Option | Default | Meaning |
|---|---|---|
| `--events N` | 1 000 000 | How many market events to generate |
| `--seed S` | 42 | Random seed. Same seed → identical data, every time |
| `--rate R` | 2 000 | Average events per *simulated* second |
| `--queue Q` | `both` | `spsc` (lock-free), `mutex`, or `both` to compare |
| `--capacity C` | 65 536 | Queue size (rounded up to a power of 2) |
| `--batch B` | 64 | How many events the consumer takes per pop |
| `--pin P,C` | off | Pin producer/consumer threads to CPU cores P and C |
| `--live` | off | Generate events on the producer thread instead of up front |
| `--depth D` | 5 | Price levels of the final book to print |
| `--csv PATH` / `--bin PATH` | off | Save the events to a CSV or binary file |
| `--redis HOST:PORT` | off | Save to Redis |
| `--symbol SYM` | `SIM` | Name to store the data under in Redis |
| `--purge` | off | Delete existing Redis data for the symbol first |
| `--zset` | off | Force the plain-Redis storage mode |

**`mds_backtest`**

| Option | Default | Meaning |
|---|---|---|
| `--strategy S` | `mm` | `mm` = market maker, `obi` = order-book-imbalance taker |
| `--events N`, `--seed S` | 1 000 000, 7 | Synthetic data when no input is given |
| `--input PATH` | - | Read events from a `.csv` or `.bin` file |
| `--redis HOST:PORT`, `--symbol SYM` | - | Read events from Redis |
| `--from NS`, `--to NS` | everything | Time window (nanoseconds since 1970) when reading from Redis |
| `--latency-us U` | 50 | Delay before the strategy's orders reach the exchange |
| `--maker-bps B`, `--taker-bps B` | −0.2, 0.3 | Fees in basis points (negative = rebate) |
| `--sample-ms M` | 1 000 | How often equity is sampled for metrics |
| `--rate`, `--vol`, `--informed`, `--impact` | see BACKTESTING.md | Synthetic market realism knobs |
| `--report`, `--equity`, `--fills` | off | Output file paths (see §5) |

**`mds_query`**: `--redis`, `--symbol`, `--field` (`bid ask mid spread
imbalance trade_px trade_qty`), `--from-ms`, `--to-ms`, `--agg`
(`avg min max first last sum count`), `--bucket-ms`, `--limit`.

### 4.2 Data files (optional input)

You can feed your own data to `mds_backtest --input` or
`python/scripts/ingest_events.py`.

**CSV format** - one event per line:

```csv
ts,symbol,type,side,id,price,qty
1700000000000703566,0,ADD,BUY,1,99.9800,200
1700000000001190044,0,ADD,SELL,2,100.0100,300
1700000000001523101,0,MARKET,BUY,3,0.0000,100
1700000000002001377,0,CANCEL,BUY,1,0.0000,0
1700000000002104411,0,MODIFY,BUY,2,0.0000,100
```

| Column | Meaning |
|---|---|
| `ts` | Event time, nanoseconds since 1970-01-01 UTC |
| `symbol` | Instrument number (0 for single-instrument data) |
| `type` | `ADD` (new limit order), `CANCEL`, `MODIFY` (new quantity), `MARKET` |
| `side` | `BUY` or `SELL` (ignored for CANCEL / MODIFY) |
| `id` | Order id. CANCEL and MODIFY refer to the id of an earlier ADD |
| `price` | Limit price in dollars (0 for CANCEL / MODIFY / MARKET) |
| `qty` | Shares; for MODIFY it is the *new* quantity |

**Binary format (`.bin`, "MDSEVT01")** - 8-byte magic `MDSEVT01`, an 8-byte
little-endian count, then 40-byte records. Much smaller and faster than CSV.
The record layout is in [ARCHITECTURE.md §4](ARCHITECTURE.md#4-data-model).

### 4.3 Redis (optional input)

`mds_backtest --redis` and `mds_query` read whatever `mds_sim --redis` or
`ingest_events.py` stored. See [REDIS_STORAGE.md](REDIS_STORAGE.md) for the
key layout.

## 5. Outputs

### 5.1 Console output

**`mds_sim`** (real output, 1M events):

```text
[generate] 1000000 events in 0.349 s (2867209 events/s), simulated span 500.51 s
[pipeline] capacity=65536 batch=64 mode=replay
  spsc-lockfree    17709334 events/s  (0.056 s)  fills=271468  latency ns p50=3532190 ...
                 consistency vs exchange book: OK
  mutex            11010999 events/s  (0.091 s)  fills=271468  latency ns p50=4935867 ...
                 consistency vs exchange book: OK
  speed-up (lock-free / mutex): 1.61x

       bid qty        bid | ask        ask qty
          1200   103.9900 | 104.0000   700
          2500   103.9800 | 104.0100   4900
          6500   103.9700 | 104.0200   9000
  orders=206  bid levels=16  ask levels=11  imbalance(5)=-0.171
```

| Line | Meaning |
|---|---|
| `[generate]` | How fast data was generated and how much simulated time it covers |
| `spsc-lockfree` / `mutex` | Events per second through each queue type, number of trades, and queue latency percentiles |
| `consistency ... OK` | The book rebuilt on the consumer thread exactly matches the exchange's own book |
| `speed-up` | Lock-free throughput ÷ mutex throughput for this run |
| The table | The final order book: quantity and price at the best levels on each side |
| `[redis]` (with `--redis`) | Events, chunks and samples written, time taken, network round trips |

**`mds_backtest`** (real output):

```text
[backtest] strategy=inventory_market_maker  300000 events in ... s  -> ... events/s
  simulated span     150.0 s
  orders / cancels   8595 / 7752
  fills / volume     846 / 84600   max |pos| 600
  total PnL          176.64  (realized 6.00, unrealized 0.00, fees -170.64)
  PnL per share      0.00209
  Sharpe / Sortino   970.05 / 1616.90 (annualized from 151 samples - TOO FEW, not meaningful)
  max drawdown       14.39 over 16.0 s
  interval win rate  78.8%
```

| Line | Meaning |
|---|---|
| `events/s` | Backtest speed |
| `orders / cancels` | How many orders the strategy sent and cancelled |
| `fills / volume` | Trades the strategy got and total shares; `max |pos|` = largest position held |
| `total PnL` | Profit in dollars = realized (closed trades) + unrealized (open position) − fees. Negative fees = rebates earned |
| `Sharpe / Sortino` | Risk-adjusted return, annualized; flagged when based on too little data |
| `max drawdown` | Largest drop from a previous equity high |
| `interval win rate` | Share of 1-second intervals where equity went up |

### 5.2 Files

| File | Written by | Contents |
|---|---|---|
| `ticks.csv` | `mds_sim --csv` | Every event, CSV format of §4.2 |
| `ticks.bin` | `mds_sim --bin` | Every event, binary MDSEVT01 format |
| `report.json` | `mds_backtest --report` | Summary metrics (example below) |
| `equity.csv` | `mds_backtest --equity` | `ts,equity,mid,position` once per sample interval |
| `fills.csv` | `mds_backtest --fills` | `ts,order_id,side,price,qty,remaining,maker,fee` for every fill |
| `docs/BENCHMARK_RESULTS.md` | `run_benchmarks.py` | Machine info + raw benchmark output |
| `mm.png` | `analyze_backtest.py --plot` | Equity, position and price chart (needs matplotlib) |

Example `report.json` (abridged; the real file has a few more fields):

```json
{
  "strategy": "inventory_market_maker",
  "events": 300000,
  "orders_sent": 8595,
  "metrics": {
    "total_pnl": 176.6416,
    "realized_pnl": 6.0000,
    "fees": -170.6416,
    "sharpe_annualized": 970.0544,
    "max_drawdown": 14.3927,
    "fills": 846,
    "volume": 84600,
    "max_abs_position": 600
  }
}
```

### 5.3 Redis

After `mds_sim --redis ... --symbol DEMO`:

| Key | What |
|---|---|
| `mds:{DEMO}:ts:bid`, `ask`, `mid`, `spread`, `imbalance` | Book state, one value per millisecond |
| `mds:{DEMO}:ts:trade_px`, `trade_qty` | Last trade price and total volume per millisecond |
| `mds:{DEMO}:chunk:0`, `chunk:1`, ... | The raw events, 4 096 per key, binary |
| `mds:{DEMO}:chunks` | Index of chunks by time |
| `mds:{DEMO}:meta` | Event count, time range, format |

## 6. Install and build

You need a C++17 compiler, CMake 3.16+ and Python 3.9+. Python packages are
**not** required.

### Windows (recommended: WSL)

1. Install WSL with Ubuntu (PowerShell as administrator), then restart:

   ```bash
   wsl --install -d Ubuntu
   ```

2. Install the compiler and build tools inside Ubuntu (asks for your Ubuntu password):

   ```bash
   wsl -d Ubuntu -- bash -c "sudo apt-get update && sudo apt-get install -y g++ cmake ninja-build python3 git"
   ```

3. Open Ubuntu and clone the repository:

   ```bash
   git clone https://github.com/vnsiddhanth/market-data-simulator.git && cd market-data-simulator
   ```

4. Build (about a minute):

   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
   ```

Native Windows with Visual Studio 2022 also works; see
[BUILDING.md](BUILDING.md#windows-visual-studio). The demo script needs a
Bash shell (WSL or Git Bash).

### Linux (Ubuntu / Debian)

```bash
sudo apt-get install -y g++ cmake ninja-build python3 git
```

```bash
git clone https://github.com/vnsiddhanth/market-data-simulator.git && cd market-data-simulator
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

### macOS

```bash
xcode-select --install && brew install cmake ninja
```

```bash
git clone https://github.com/vnsiddhanth/market-data-simulator.git && cd market-data-simulator
```

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

### Check the build

```bash
ctest --test-dir build --output-on-failure
```

You should see `100% tests passed, 0 tests failed out of 10`.

## 7. Run it: the one-command demo

```bash
./demo.sh
```

It starts the bundled fake Redis, then runs six steps, pausing briefly
between them:

| Step | What you see |
|---|---|
| 1 | 1M events through the lock-free and mutex pipelines, speed comparison, final order book |
| 2 | 300K events saved to Redis (time series + binary chunks) |
| 3 | 10-second average prices queried back from Redis |
| 4 | Python inspection: OHLC price bars, volume, and a replay check of the stored events |
| 5 | Both strategies backtested on the data in Redis |
| 6 | Python recomputes the market maker's metrics and confirms they match the C++ |

Everything is written to `out/`. Useful variations:

```bash
EVENTS=3000000 PAUSE=0 ./demo.sh
```

```bash
REDIS=127.0.0.1:6379 ./demo.sh
```

(The second uses a real Redis; see §9.)

## 8. Run it: each tool on its own

Create the output folder first:

```bash
mkdir -p out
```

**A. Compare lock-free vs mutex and print the order book**

```bash
./build/mds_sim --events 1000000 --queue both --depth 10
```

**B. Save data to files**

```bash
./build/mds_sim --events 500000 --queue spsc --csv out/ticks.csv --bin out/ticks.bin
```

```bash
head -5 out/ticks.csv
```

**C. Independently verify the file with the Python order book**

```bash
python3 python/scripts/verify_events.py out/ticks.bin --depth 5
```

The book it prints matches the one `mds_sim` printed for the same events.

**D. Backtest from the file, with reports**

```bash
./build/mds_backtest --strategy mm --input out/ticks.bin --report out/mm.json --equity out/mm_equity.csv --fills out/mm_fills.csv
```

**E. Analyse the backtest in Python (and plot, if matplotlib is installed)**

```bash
python3 python/scripts/analyze_backtest.py --equity out/mm_equity.csv --fills out/mm_fills.csv --report out/mm.json --plot out/mm.png
```

**F. Try "what if" scenarios**

```bash
./build/mds_backtest --strategy mm --latency-us 500
```

```bash
./build/mds_backtest --strategy mm --impact 0
```

```bash
./build/mds_backtest --strategy obi --taker-bps 0
```

Slower orders, a market with no price impact (market making becomes
unrealistically easy), and free taker trading, respectively.

**G. Redis round trip** (start the fake server in a second terminal first)

```bash
python3 python/scripts/fake_redis.py --port 6399
```

```bash
./build/mds_sim --events 300000 --queue spsc --redis 127.0.0.1:6399 --symbol DEMO --purge
```

```bash
./build/mds_query --redis 127.0.0.1:6399 --symbol DEMO --field mid --agg avg --bucket-ms 10000
```

```bash
python3 python/scripts/redis_inspect.py --redis 127.0.0.1:6399 --symbol DEMO --bars 10000
```

```bash
./build/mds_backtest --strategy mm --redis 127.0.0.1:6399 --symbol DEMO
```

**H. Import your own CSV into Redis**

```bash
python3 python/scripts/ingest_events.py my_events.csv --redis 127.0.0.1:6399 --symbol MYDATA --purge
```

## 9. Using a real Redis

The fake server is enough for everything above. To use real Redis with the
RedisTimeSeries module, install Docker and run:

```bash
docker compose -f docker/docker-compose.yml up -d
```

Then use `--redis 127.0.0.1:6379`. A browser UI (RedisInsight) is at
http://localhost:8001. Plain Redis without the module also works: the app
detects it and switches to sorted sets automatically.

## 10. Running the tests and benchmarks

```bash
ctest --test-dir build --output-on-failure
```

```bash
cd python && python3 -m unittest discover -s tests -v && cd ..
```

```bash
python3 python/scripts/run_benchmarks.py --build build --pin 2,4 --out docs/BENCHMARK_RESULTS.md
```

The benchmark takes about two minutes. `--pin 2,4` should name two different
physical cores on your machine; drop it if unsure.

## 11. Troubleshooting

| Problem | Fix |
|---|---|
| `cmake: command not found` / no compiler | Install the build tools (§6) |
| `mds_backtest: cannot open out/...` | Create the folder: `mkdir -p out` |
| `redis: cannot connect to 127.0.0.1:6399` | Start `fake_redis.py` (or Docker Redis) first, and use the matching port |
| `Address already in use` from fake Redis | Another server is on that port; use `--port 6400` and update `--redis` |
| `demo.sh: Permission denied` | `chmod +x demo.sh` or run `bash demo.sh` |
| `bad interpreter` / `\r` errors on Windows | The file got Windows line endings; run `sed -i 's/\r$//' demo.sh` |
| Benchmark numbers vary between runs | Normal on laptops (turbo, background load). Close other apps, use `--pin`, compare medians |
| `matplotlib not installed - skipping plot` | Optional: `pip install matplotlib` |
| Sharpe shows `TOO FEW, not meaningful` | Run more events (e.g. `--events 2000000`) so there are ≥ 250 samples |
