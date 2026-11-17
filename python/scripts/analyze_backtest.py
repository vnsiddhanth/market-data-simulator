#!/usr/bin/env python3
"""Analyse the output of ``mds_backtest``.

    mds_backtest --strategy mm --report out/mm.json --equity out/mm_equity.csv --fills out/mm_fills.csv
    python python/scripts/analyze_backtest.py --equity out/mm_equity.csv --fills out/mm_fills.csv \
        --report out/mm.json --plot out/mm.png

Prints an independent recomputation of the headline metrics (a cross-check of
the C++ numbers), fill quality (maker ratio, 1s/5s mark-outs) and, if
matplotlib is installed, writes an equity / position / mid-price chart.
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from mds.analysis import format_summary, load_equity, load_fills, load_report, markout, summarize  # noqa: E402


def plot(curve, out_path: str) -> bool:
    try:
        import matplotlib  # type: ignore

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt  # type: ignore
    except ImportError:
        print("  (matplotlib not installed - skipping plot)")
        return False
    t0 = curve[0].ts
    x = [(p.ts - t0) / 1e9 for p in curve]
    fig, axes = plt.subplots(3, 1, figsize=(11, 8), sharex=True)
    axes[0].plot(x, [p.equity for p in curve], lw=1.2)
    axes[0].set_ylabel("equity")
    axes[0].grid(alpha=0.3)
    axes[1].step(x, [p.position for p in curve], where="post", lw=1.0)
    axes[1].set_ylabel("position")
    axes[1].grid(alpha=0.3)
    axes[2].plot(x, [p.mid for p in curve], lw=0.9)
    axes[2].set_ylabel("mid")
    axes[2].set_xlabel("simulated seconds")
    axes[2].grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    print(f"  plot -> {out_path}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--equity", required=True)
    ap.add_argument("--fills")
    ap.add_argument("--report")
    ap.add_argument("--plot")
    args = ap.parse_args()

    curve = load_equity(args.equity)
    fills = load_fills(args.fills) if args.fills else []
    s = summarize(curve, fills)
    if fills:
        s.extras["markout_1s_per_share"] = markout(fills, curve, 1.0)
        s.extras["markout_5s_per_share"] = markout(fills, curve, 5.0)
    print("Recomputed from equity/fills:")
    print(format_summary(s))

    if args.report:
        rep = load_report(args.report)["metrics"]
        print("\nCross-check vs C++ report:")
        for name, mine, theirs in [
            ("total_pnl", s.total_pnl, rep["total_pnl"]),
            ("max_drawdown", s.max_drawdown, rep["max_drawdown"]),
            ("sharpe", s.sharpe, rep["sharpe_annualized"]),
        ]:
            ok = abs(mine - theirs) <= 1e-3 * max(1.0, abs(theirs))
            print(f"  {name:<14} python={mine:>14.4f}  c++={theirs:>14.4f}  {'OK' if ok else 'DIFF'}")
    if args.plot:
        plot(curve, args.plot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
