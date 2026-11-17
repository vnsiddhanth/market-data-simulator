# Backtesting

`include/mds/backtest/*.hpp` · `src/backtester.cpp` · `src/strategies.cpp`

## 1. Event loop

For each historical `MarketEvent` in timestamp order:

| Step | What happens | Why here |
|---|---|---|
| 1 | `now = event.ts` | simulated clock is event time |
| 2 | **Release due actions**: strategy orders whose `arrive_ts <= now` reach the exchange; cancels whose `cancel_ts <= now` take effect | latency is applied *before* the event, so an order can only interact with flow that arrives after it |
| 3 | **Queue walk**: the event is walked through the strategy's resting orders against the *pre-event* book (fills, queue-position updates) | the book still shows exactly the liquidity the aggressor saw |
| 4 | `OrderBook::apply(event)` | reconstruct the market |
| 5 | `Strategy::on_market_data(ctx, book, event)` | strategy sees the post-event book |
| 6 | Sample equity on a fixed simulated-time grid | uniform intervals for Sharpe |

The first `warmup_events` (default 1 000) only build the book; the strategy
starts once there is a two-sided market.

## 2. Latency model

Every `send_limit` / `send_market` / `cancel` is stamped with
`now + latency` and sits in flight until the simulated clock reaches it.
Consequences that the engine models:

* **Stale quotes.** A market maker's cancel may arrive after the market has
  already traded through its quote; it gets filled (adverse selection).
* **Cancel overtaking.** If a cancel's effective time is ≤ the order's arrival
  time, the order never reaches the book.
* **Taker slippage.** A market order executes against the book as it is at
  arrival, not as it was when the signal fired.

Latency is configurable (`--latency-us`); sweeping it is the simplest way to
measure a strategy's sensitivity to infrastructure speed.

## 3. Fill model

Strategy orders are **virtual**: they never enter the replayed book, so the
historical flow is unchanged (the standard "no market impact" assumption,
reasonable for size small relative to displayed depth).

### Joining the queue

On arrival, a non-marketable limit order joins the back of its price level:

```
queue_ahead = displayed quantity at that price
joined_seq  = book.next_sequence()   // every live order with seq < joined_seq is ahead of us
```

A marketable limit (or a market order) first walks the opposite side of the
displayed book, taking liquidity level by level at each level's price and
paying the taker fee; any limit remainder then rests.

### Exact queue-position tracking

Most simple backtesters either fill a resting order as soon as the price
trades (too optimistic) or never (too pessimistic), or guess with a
probability. Because this engine reconstructs the full order-by-order book,
it can track queue position **exactly** under FIFO:

| Market event | Effect on our `queue_ahead` |
|---|---|
| Cancel of an order at our price with `seq < joined_seq` | − its remaining qty (it was ahead of us) |
| Cancel of an order with `seq >= joined_seq` | none (it was behind us) |
| Modify size-down of an order ahead | − the reduction (it keeps priority) |
| Modify size-up of an order ahead | − its full qty (it moves behind us) |
| Aggressive flow | see the queue walk below |

### The queue walk

For an aggressive event with quantity `Q` (an `Add` that crosses, or a
`Market`), and each of our working orders on the passive side whose price the
aggressor reaches, in our own price priority:

```
better   = displayed qty at prices strictly better than ours
ahead    = min(queue_ahead, displayed qty at our price)
reaching = Q - better - (qty already given to our better-priced orders)

if reaching <= 0:      nothing reaches our level
elif reaching <= ahead: queue_ahead = ahead - reaching          # queue shrinks
else:                   fill min(reaching - ahead, remaining)   # we trade
                        queue_ahead = 0
```

This one rule reproduces every case a FIFO matching engine would produce:

* trades that only eat into the queue ahead of us,
* aggressors that exhaust the displayed liquidity ahead of us and reach us,
* sweeps that **trade through** our price (they necessarily consumed
  everything at our level first),
* aggressors that hit **only us** because our quote improved the displayed
  best (`better = ahead = 0`).

All maker fills execute at **our** limit price and earn the maker fee/rebate.

### Known biases (documented, deliberate)

* **No impact.** Our fills don't remove liquidity from the replayed book;
  aggressors that hit us still also trade with the displayed orders behind us.
  Conservative for queue position, optimistic for large size.
* **Taker walk ignores our earlier takes** within the same event.
* **Hidden liquidity** in historical data (if any) is invisible to the model.
* **Single venue**, no auction phases, no self-trade prevention.

## 4. Accounting

`Portfolio` uses **average-cost** accounting:

* adding to a position updates the weighted average cost;
* reducing realises `(px − avg_cost) × closed_qty × direction`;
* flipping through zero realises the closed part and opens the rest at the
  fill price.

Fees: `fee = price × qty × bps / 10 000`, maker default −0.2 bps (rebate),
taker +0.3 bps. Cash includes fees, so

```
equity = cash + position × mid = realized + unrealized − fees
```

which is asserted in `tests/test_backtest.cpp` (`strategies_run_end_to_end`).

## 5. Metrics

Equity is sampled every `equity_sample_interval_ns` of **simulated** time
(default 1 s), marked at the mid.

| Metric | Definition |
|---|---|
| Total PnL | `equity_last − equity_first` |
| Sharpe (annualised) | `mean(ΔE) / stdev(ΔE) × √(periods per year)`, periods per year = 252 × 6.5 h / interval |
| Sortino (annualised) | `mean(ΔE) / sqrt(mean(min(ΔE,0)²)) × √(periods per year)` |
| Max drawdown | largest peak-to-trough fall of equity, currency units |
| Drawdown duration | longest time spent below a previous peak |
| Interval win rate | share of non-zero equity increments that are positive |
| PnL per share | total PnL / traded volume |

ΔE are PnL increments rather than returns because a market-making book has no
well-defined committed capital. The Python `analysis.py` recomputes these from
the CSV outputs independently (`analyze_backtest.py --report` prints a
side-by-side check) and adds **mark-outs**: average signed mid move 1 s / 5 s
after each fill - the standard measure of adverse selection.

## 6. Reference strategies

### InventoryMarketMaker

A simplified Avellaneda-Stoikov quoter:

```
reservation = mid − (position / quote_size) × skew_ticks_per_lot × tick
bid = floor_to_tick(reservation − half_spread)    capped at best_ask − tick   (post-only)
ask = ceil_to_tick (reservation + half_spread)    floored at best_bid + tick
```

* Inventory skew pushes quotes away from the side that would increase
  inventory, mean-reverting position.
* Quotes are only replaced when the desired price changes, and at most every
  `min_requote_ns` (500 µs), to avoid cancel/replace churn that would reset
  queue position.
* Hard position limit suppresses the side that would breach it.

### ImbalanceTaker

Trades in the direction of top-5-level book imbalance
`(B − A) / (B + A)`:

* enter with a market order when `|imbalance| > 0.6`, scaling in up to
  `max_position`. The threshold is calibrated on the default synthetic feed,
  where `|imbalance(5)|` has p50 ≈ 0.18, p90 ≈ 0.50 and p99 ≈ 0.71, so it
  enters on roughly the strongest 7% of book states. Lower thresholds fire so
  often that the strategy only churns and pays the spread;
* exit when the signal fades below `0.2` (or flips) or after `max_hold`
  (2 s);
* one order in flight at a time and a 2 ms cool-down.

It exists mainly to exercise the taker path: latency slippage, depth walking
and taker fees. On synthetic data, neither strategy is expected to have real
alpha - the point is that their PnL is *explainable* from the fill model.

## 7. Synthetic market realism

An early version of the feed made market making look risk-free: a Sharpe in
the thousands, a 100% interval win rate and zero drawdown. Inspecting the
fills showed why. Every fill came from a sweep through the book, and the
price always snapped back, so the opposite quote filled at a profit
milliseconds later. In that market nobody trading against the maker knew
anything, and trades had no lasting effect on price. Real markets have both,
and they are what costs a liquidity provider money. The feed now models them:

| Mechanism | Parameter (default) | Effect |
|---|---|---|
| Informed takers | `informed_fraction` (0.6) | that share of aggressive orders trades in the direction of the *next* fair-value move, so quotes they hit are followed by an adverse price move |
| Permanent price impact | `impact_ticks_per_lot` (0.15) | each lot traded by a deliberate aggressor moves fair value in its direction (Kyle's lambda) |
| Realistic pace | `events_per_second` (2 000), `volatility_ticks_per_sqrt_sec` (1.5) | about 17 simulated minutes per 2M events, so equity curves have ~1 000 one-second samples |

Two feedback loops had to be avoided:

* Informed takers originally chased the gap between fair value and the mid.
  Impact widens that gap, so the takers kept buying and the price ran away.
  They now key off the *next* innovation, which the order flow cannot
  influence.
* Passive orders placed around a moved fair value cross stale resting orders.
  If that volume also carried impact, it would push fair value further, so
  impact applies only to market orders and deliberately aggressive adds.

With these in place, results across seeds 7, 8 and 9 are stable. The market
maker's trading PnL is slightly negative (−$130 to −$150) with small negative
mark-outs, it profits only through rebates (≈ +$1 000) and it has drawdowns of
$65-$140. Sweeping `--impact` from 0 to 0.2 moves it continuously from
risk-free to loss-making, which is the expected shape.

`mds_backtest` exposes `--rate`, `--vol`, `--informed` and `--impact` to
explore this, and prints the number of equity samples behind each Sharpe
ratio. Under 250 samples it marks the figure as not meaningful: annualising
a handful of samples multiplies sampling noise by √(periods per year).

## 8. Throughput

The backtester is single-threaded and allocation-free on the hot path apart
from fill reports. Per event it does one `OrderBook::apply`, a scan of the
strategy's (few) open orders and a virtual call into the strategy, so its
throughput tracks the order book's single-threaded rate. The design target is
**10 K+ ticks/s**; the measured rate is printed by every `mds_backtest` run
(`events/s`) and the test suite fails if a 150 K-event replay runs below that
target.

## 9. Writing a strategy

```cpp
class MyStrategy final : public mds::Strategy {
public:
    std::string name() const override { return "my_strategy"; }
    void on_market_data(mds::StrategyContext& ctx, const mds::OrderBook& book,
                        const mds::MarketEvent& ev) override {
        if (ctx.open_orders() == 0 && book.imbalance(3) > 0.6)
            ctx.send_limit(mds::Side::Buy, *book.best_bid(), 100);
    }
    void on_fill(mds::StrategyContext& ctx, const mds::ExecutionReport& r) override { /* ... */ }
};
```

Register it in `make_strategy` (`src/strategies.cpp`) to make it available as
`mds_backtest --strategy <name>`.
