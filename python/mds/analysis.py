"""Backtest analytics: equity curve, fills and event-file statistics.

All functions take/return plain Python structures so they work without
pandas; ``to_dataframe`` helpers are provided when pandas is installed.
"""
from __future__ import annotations

import csv
import json
import math
from dataclasses import dataclass, field
from statistics import mean, pstdev
from typing import Dict, List, Optional, Sequence

TRADING_SECONDS_PER_YEAR = 252 * 6.5 * 3600


@dataclass
class EquityPoint:
    ts: int
    equity: float
    mid: float
    position: int


@dataclass
class FillRecord:
    ts: int
    order_id: int
    side: str
    price: float
    qty: int
    remaining: int
    maker: bool
    fee: float


@dataclass
class Summary:
    total_pnl: float
    sharpe: float
    sortino: float
    max_drawdown: float
    max_drawdown_duration_s: float
    win_rate: float
    fills: int
    volume: int
    maker_ratio: float
    avg_fill_size: float
    turnover_notional: float
    extras: Dict[str, float] = field(default_factory=dict)


def load_equity(path: str) -> List[EquityPoint]:
    with open(path, newline="") as f:
        return [EquityPoint(int(r["ts"]), float(r["equity"]), float(r["mid"]), int(r["position"]))
                for r in csv.DictReader(f)]


def load_fills(path: str) -> List[FillRecord]:
    with open(path, newline="") as f:
        return [FillRecord(int(r["ts"]), int(r["order_id"]), r["side"], float(r["price"]),
                           int(r["qty"]), int(r["remaining"]), r["maker"] == "1", float(r["fee"]))
                for r in csv.DictReader(f)]


def load_report(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def max_drawdown(equity: Sequence[float], ts: Optional[Sequence[int]] = None) -> tuple:
    """Return (max drawdown, longest underwater duration in seconds)."""
    if not equity:
        return 0.0, 0.0
    peak, peak_ts = equity[0], (ts[0] if ts else 0)
    mdd, longest = 0.0, 0
    for i, e in enumerate(equity):
        t = ts[i] if ts else i
        if e >= peak:
            peak, peak_ts = e, t
        else:
            mdd = max(mdd, peak - e)
            longest = max(longest, t - peak_ts)
    return mdd, (longest / 1e9 if ts else float(longest))


def sharpe(increments: Sequence[float], interval_s: float) -> float:
    if len(increments) < 2:
        return 0.0
    sd = pstdev(increments) * math.sqrt(len(increments) / (len(increments) - 1))
    return mean(increments) / sd * math.sqrt(TRADING_SECONDS_PER_YEAR / interval_s) if sd > 0 else 0.0


def sortino(increments: Sequence[float], interval_s: float) -> float:
    if len(increments) < 2:
        return 0.0
    dd = math.sqrt(sum(min(x, 0.0) ** 2 for x in increments) / len(increments))
    return mean(increments) / dd * math.sqrt(TRADING_SECONDS_PER_YEAR / interval_s) if dd > 0 else 0.0


def summarize(curve: Sequence[EquityPoint], fills: Sequence[FillRecord] = ()) -> Summary:
    eq = [p.equity for p in curve]
    ts = [p.ts for p in curve]
    inc = [b - a for a, b in zip(eq, eq[1:])]
    interval = (ts[1] - ts[0]) / 1e9 if len(ts) > 1 and ts[1] > ts[0] else 1.0
    mdd, dur = max_drawdown(eq, ts)
    nonzero = [x for x in inc if x != 0]
    volume = sum(f.qty for f in fills)
    return Summary(
        total_pnl=(eq[-1] - eq[0]) if eq else 0.0,
        sharpe=sharpe(inc, interval),
        sortino=sortino(inc, interval),
        max_drawdown=mdd,
        max_drawdown_duration_s=dur,
        win_rate=(sum(1 for x in nonzero if x > 0) / len(nonzero)) if nonzero else 0.0,
        fills=len(fills),
        volume=volume,
        maker_ratio=(sum(f.qty for f in fills if f.maker) / volume) if volume else 0.0,
        avg_fill_size=(volume / len(fills)) if fills else 0.0,
        turnover_notional=sum(f.qty * f.price for f in fills),
    )


def markout(fills: Sequence[FillRecord], curve: Sequence[EquityPoint], horizon_s: float = 1.0) -> float:
    """Average signed mid move ``horizon_s`` after each fill, per share.

    Positive == fills were followed by favourable price moves; strongly
    negative for a market maker == adverse selection.
    """
    if not fills or not curve:
        return 0.0
    import bisect
    ts = [p.ts for p in curve]
    h = int(horizon_s * 1e9)
    total, qty = 0.0, 0
    for f in fills:
        i = bisect.bisect_left(ts, f.ts + h)
        if i >= len(curve):
            continue
        sign = 1 if f.side == "BUY" else -1
        total += sign * (curve[i].mid - f.price) * f.qty
        qty += f.qty
    return total / qty if qty else 0.0


def format_summary(s: Summary) -> str:
    rows = [
        ("Total PnL", f"{s.total_pnl:,.2f}"),
        ("Sharpe (ann.)", f"{s.sharpe:.2f}"),
        ("Sortino (ann.)", f"{s.sortino:.2f}"),
        ("Max drawdown", f"{s.max_drawdown:,.2f} over {s.max_drawdown_duration_s:.1f}s"),
        ("Interval win rate", f"{s.win_rate:.1%}"),
        ("Fills / volume", f"{s.fills:,} / {s.volume:,}"),
        ("Maker ratio", f"{s.maker_ratio:.1%}"),
        ("Avg fill size", f"{s.avg_fill_size:.1f}"),
        ("Turnover", f"{s.turnover_notional:,.0f}"),
    ]
    rows += [(k, f"{v:.5f}") for k, v in s.extras.items()]
    width = max(len(k) for k, _ in rows)
    return "\n".join(f"  {k:<{width}}  {v}" for k, v in rows)


def to_dataframe(records: Sequence[object]):  # pragma: no cover - optional dependency
    import pandas as pd  # type: ignore
    return pd.DataFrame([r.__dict__ for r in records])
