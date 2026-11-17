# The HFT path

This document covers the parts of the project built the way high-frequency
trading systems are built: real exchange protocols, a cache-friendly order
book, a pinned busy-spinning tick-to-trade pipeline, pre-trade risk
controls, and a UDP market-data feed with gap recovery. Each part has a
tool you can run, tests that prove it correct, and measured numbers.

| Component | Where | Run it |
|---|---|---|
| Flat price-ladder order book | `orderbook/flat_order_book.hpp` | `bench_orderbook --book both` |
| Nasdaq TotalView-ITCH 5.0 codec | `itch/itch.hpp`, `itch/book_builder.hpp` | `mds_itch --generate` / `--replay` |
| Tick-to-trade pipeline | `apps/mds_t2t.cpp` | `mds_t2t --pin 2,4,6` |
| OUCH 4.2 order entry | `hft/ouch.hpp` | (used by `mds_t2t`) |
| Pre-trade risk gate | `hft/risk.hpp` | (used by `mds_t2t`; unit tests) |
| MoldUDP64 over UDP, gap recovery | `net/moldudp64.hpp` | `mds_feed --demo --drop 0.01` |
| TSC clock | `common/tsc.hpp` | (used by all of the above) |
| Low-latency live engine | `apps/mds_live.cpp --spin` | web app → Market settings → Low-latency mode |

## 1. Flat order book

The original `OrderBook` (std::map + std::deque) is a clean general design.
`FlatOrderBook` is what you would ship on a latency-critical path:

| | `OrderBook` | `FlatOrderBook` |
|---|---|---|
| Price level lookup | red-black tree walk, O(log L) | `(price - base) / tick`: one subtract and divide, O(1) |
| Best bid/ask after the top level empties | `map::begin()` | **bitmap scan**: TZCNT/LZCNT inspects 64 levels per instruction |
| Order storage | `unordered_map` node per order (heap allocation) | **pre-allocated pool** of 48-byte nodes with a free list |
| Time priority | `std::deque` + lazy tombstones + compaction | **intrusive doubly-linked list** by 32-bit index: exact O(1) unlink |
| Id → order | `std::unordered_map` | **open-addressing hash**, flat array, backward-shift deletion |
| Allocations on the hot path | some | **none** once capacity is reserved |
| Price range | unbounded | a window of ticks around the market, **re-centred** automatically |

It also supports the operations an exchange feed describes directly:
`execute()` (ITCH 'E'/'C'), `reduce()` ('X') and `replace()` ('U').

**Correctness.** A differential test compares it against the map book over
50,000 random operations (including market drifts that force re-centring),
fill-for-fill. A second test replays 300,000 synthetic-feed events through
both and checks every price level. `validate()` checks the linked lists,
bitmap, counters and best-price indices.

**Measured** (2M events, i5-1235U laptop, WSL2, GCC 15, median of 5):

| | map book | flat book |
|---|---:|---:|
| Throughput | 12.6 M events/s | **24.1 M events/s (1.9×)** |
| add p50 / p99 | 84 / 340 ns | 52 / 265 ns |
| cancel p50 / p99 | 89 / 254 ns | **31 / 81 ns** |
| modify p50 / p99 | 118 / 382 ns | 48 / 94 ns |
| market p50 / p99 | 106 / 495 ns | 75 / 277 ns |

## 2. Nasdaq TotalView-ITCH 5.0

ITCH is Nasdaq's order-by-order binary market-data feed. `itch.hpp`
implements the real message layouts (big-endian, 11-byte common header,
6-byte nanosecond timestamps, 4-implied-decimal prices) and the BinaryFILE
framing of Nasdaq's sample files (2-byte length prefix):

| Type | Message | Book effect |
|---|---|---|
| `A` / `F` | Add Order (without / with MPID) | new resting order |
| `E` | Order Executed | resting order traded (reduces or removes it) |
| `C` | Order Executed With Price | same, at a different price |
| `X` | Order Cancel | resting order loses shares |
| `D` | Order Delete | resting order removed |
| `U` | Order Replace | cancel-replace with a new reference, price and size (loses priority) |
| `P` | Trade (non-cross) | hidden-liquidity print, no book change |
| `R`, `S` | Stock Directory, System Event | symbol mapping, session state |

* **Parser:** zero-copy (decodes straight from the buffer), with no
  allocation and a template-dispatched handler.
* **`BookBuilder`:** one `FlatOrderBook` per stock locate, with optional
  symbol filtering.
* **`SyntheticItchEncoder`:** publishes the simulator's event stream
  exactly as an exchange would. Aggressive orders appear only as
  executions of the resting orders they hit, size-ups become cancel-replace
  'U' messages with fresh reference numbers, and so on.
* **`FileReader`:** streams files of any size with bounded memory.

**Correctness.** Simulated exchange → ITCH bytes → feed handler gives a
book identical, level by level, to the exchange's own, with zero unknown
order references.

**Measured** (5.5M ITCH messages, 175 MB):

| | |
|---|---:|
| Decode only | **38 M msg/s, 1.2 GB/s** |
| Decode + book building | **7.4 M msg/s** |

### Replaying real Nasdaq data

Nasdaq publishes full-day TotalView-ITCH 5.0 sample files (several GB
each) at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/. Download one, gunzip
it, then:

```bash
./build/mds_itch --replay 01302019.NASDAQ_ITCH50 --top 15
```

```bash
./build/mds_itch --replay 01302019.NASDAQ_ITCH50 --symbol AAPL --depth 10
```

All-symbol mode gives every symbol a compact 2,048-tick ladder; orders priced
further away are counted as `rejects` per symbol. With `--symbol` the ladder
is 65,536 ticks deep. `--parse-only` measures pure decode speed.

## 3. Tick-to-trade

`mds_t2t` runs the canonical HFT hot path on three threads:

```
[feed / NIC thread]  ── ITCH packet, one 64-byte cache line per slot ──►  SPSC ring
                                                                              │
[strategy thread]    ITCH decode → FlatOrderBook → signal → risk gate → OUCH encode
                                                                              │
                                                    order slot ──► SPSC ring ▼
[gateway thread]     transmit (in memory, or a real UDP send with --udp), stamp tx
```

* Threads **busy-spin** and can be **pinned** (`--pin F,S,G`).
* Packets are paced in **CPU cycles** (no sleeps, no syscalls).
* Every stage is stamped with **RDTSC**, calibrated to nanoseconds.
* The strategy re-quotes on every change of the inside quote. Each order
  passes the **pre-trade risk gate**, and is encoded as an **OUCH 4.2
  Enter Order** (49 bytes, Nasdaq's binary order-entry protocol).

**Measured** (1.1M ITCH packets at 500k msg/s, pinned):

| Stage | p50 |
|---|---:|
| **Tick-to-trade (packet in → order out)** | **0.85-1.3 µs** |
| rx → book updated (cross-core hop + ITCH decode + book) | 0.43-0.65 µs |
| book → decision + risk checks + OUCH encode | **45-80 ns** |
| decision → gateway (cross-core hop) | 0.33-0.50 µs |

**About the tails.** On this laptop the p99 runs into milliseconds. The
tool proves why: the feed thread watches its own loop and records every
time it was off the CPU for more than 20 µs. Typical runs show hundreds to
thousands of such gaps, worst 30-300 ms, caused by the WSL2 virtual
machine and the hybrid P/E-core CPU. A busy-spinning thread that is
descheduled backs up every packet behind it. On bare-metal Linux with
isolated cores those gaps disappear, and with them the tail:

```text
isolcpus=2,4,6 nohz_full=2,4,6 rcu_nocbs=2,4,6     # kernel command line
```

```bash
./build/mds_t2t --pin 2,4,6 --rate 500000
```

Production systems go further: kernel-bypass NICs (DPDK, Solarflare
ef_vi/Onload, Exablaze) skip the kernel network stack, and some put the
whole path in FPGA logic.

## 4. Pre-trade risk gate

Every order passes `RiskGate::check()` before it can leave. These are the
controls required by SEC Rule 15c3-5 (the market access rule):

| Check | Default |
|---|---|
| Max order size | 5,000 shares |
| Max order notional | $1,000,000 |
| Price collar vs reference (mid) | ±50 ticks |
| Max absolute position after the order | 10,000 shares |
| Max open orders | 200 |
| Order rate (token bucket on the TSC clock) | 20,000/s, burst 500 |
| Kill switch | trips after 1,000 consecutive rejects (or manually) |

The checks are a handful of compares with no allocation and no locks, and
are included in the 45-80 ns decision stage above. Rejects are counted by
reason.

## 5. MoldUDP64 feed over UDP

Exchanges multicast ITCH over UDP in **MoldUDP64** packets:
- a 20-byte header (session, sequence number of the first message, message count);
- then length-prefixed messages;
- heartbeats (count 0) and end of session (count 0xFFFF).

UDP can lose packets, so `net/moldudp64.hpp` implements both sides of the
protocol:

* **Publisher:**
  - packs messages into packets under a 1,500-byte MTU;
  - stamps each message's ITCH timestamp with the wall clock at send time;
  - keeps every message;
  - runs a **re-request server**.
* **Subscriber (feed handler):**
  - tracks the expected sequence and detects every gap;
  - **holds later messages back**: applying them out of order would corrupt
    the book with executions for orders it never saw;
  - **requests each missing range** from the server, retrying each one
    independently after 20 ms;
  - releases held messages in order as the gaps fill;
  - ignores duplicates.
* **Wire-to-book latency:** receiver clock minus the exchange timestamp in
  each message. That's how firms measure feed latency.

```bash
./build/mds_feed --demo --events 500000 --rate 300000 --drop 0.02
```

Or as two processes (use `--host 239.1.1.1` / `--group 239.1.1.1` for multicast):

```bash
./build/mds_feed --subscribe --port 31001 --request-host 127.0.0.1 --request-port 31002
```

```bash
./build/mds_feed --publish --host 127.0.0.1 --port 31001 --request-port 31002 --events 1000000
```

**Measured** (553k messages over UDP loopback):

| | no loss | 2% packet loss |
|---|---:|---:|
| Gaps detected / recovered | 0 | **9,633 / all** |
| Messages re-sent | 0 | 10,540 |
| Book after the session | identical to exchange | **identical to exchange** |
| Wire-to-book p50 | 3.7 µs | 430 µs (messages wait behind gaps) |

## 6. Low-latency live mode

The web app's live engine (`mds_live`) normally sleeps when idle, so a quiet
market costs almost no CPU. **Low-latency mode** (`--spin`, a checkbox in
the web app's Market settings) busy-spins the consumer like the rest of the
HFT path:

| Live engine | Thread hand-off p50 | Tick-to-trade p50 / p99 |
|---|---:|---:|
| Power-saving | ~75 µs | ~65-82 µs / ~190 µs |
| **Low-latency (busy-spin)** | **0.8-1.6 µs** | **1.4-3.7 µs / 3-13 µs** |

`MDS_LIVE_PIN=P,C` pins the engine's threads when the server runs on
dedicated hardware.

## 7. What is still not production HFT

Being precise about the remaining gap is part of the design:

| Production HFT | Here |
|---|---|
| Kernel-bypass NIC (DPDK / ef_vi), hardware timestamps | kernel UDP sockets, TSC timestamps |
| Isolated, tuned bare-metal cores | runs anywhere (WSL2 laptop for these numbers) |
| Live exchange sessions (SoupBinTCP login, OUCH acks/fills) | OUCH messages are encoded and sent, but no exchange answers |
| Real-time exchange data | synthetic ITCH, or historical Nasdaq ITCH files |
| FPGA order books / tick-to-trade in ~100s of ns | software, ~1 µs median |
| Multi-venue routing, auctions, hidden orders | single venue, continuous trading |
