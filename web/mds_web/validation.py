"""Request validation: every user-supplied parameter is checked against a
schema (type, range, allowed values) before it reaches a command line.

Commands are always built as argument lists (never a shell string), and
only validated, re-serialised values are passed, so user input cannot inject
extra flags or shell syntax.
"""
from __future__ import annotations

from typing import Any, Dict, Tuple


class ValidationError(ValueError):
    def __init__(self, errors: Dict[str, str]):
        super().__init__("; ".join(f"{k}: {v}" for k, v in errors.items()))
        self.errors = errors


# name -> (type, default, min, max)  or  (type "enum", default, choices)
Num = Tuple[type, Any, float, float]

FEED = {
    "seed": (int, None, 0, 2**63 - 1),
    "rate": (float, 2000.0, 50, 50_000),
    "vol": (float, 1.5, 0.0, 20.0),
    "informed": (float, 0.6, 0.0, 1.0),
    "impact": (float, 0.15, 0.0, 1.0),
}


def schemas(max_events: int, max_bench: int) -> Dict[str, Dict[str, tuple]]:
    return {
        "simulation": {
            "events": (int, 500_000, 1_000, max_events),
            "queue": ("enum", "both", ("both", "spsc", "mutex")),
            "batch": (int, 64, 1, 4096),
            "capacity": (int, 65_536, 16, 1 << 22),
            "save_events": (bool, True),
            "redis": (bool, False),
            **FEED,
        },
        "backtest": {
            "strategy": ("enum", "mm", ("mm", "obi")),
            "source": (str, "synthetic"),
            "events": (int, 1_000_000, 2_000, max_events),
            "latency_us": (float, 50.0, 0.0, 100_000.0),
            "maker_bps": (float, -0.2, -10.0, 10.0),
            "taker_bps": (float, 0.3, -10.0, 50.0),
            **FEED,
        },
        "benchmark": {
            "messages": (int, 5_000_000, 100_000, max_bench),
            "events": (int, 500_000, 10_000, max(10_000, min(max_events, 3_000_000))),
            "reps": (int, 3, 1, 7),
        },
        "live": {
            "strategy": ("enum", "mm", ("mm", "obi", "none")),
            "speed": (float, 1.0, 0.1, 20.0),
            "snapshot_ms": (int, 250, 100, 2000),
            "latency_us": (float, 50.0, 0.0, 100_000.0),
            **{**FEED, "rate": (float, 2000.0, 50, 20_000)},
        },
    }


def validate(schema: Dict[str, tuple], params: Any) -> Dict[str, Any]:
    if params is None:
        params = {}
    if not isinstance(params, dict):
        raise ValidationError({"params": "must be an object"})
    errors: Dict[str, str] = {}
    out: Dict[str, Any] = {}
    unknown = set(params) - set(schema)
    for k in sorted(unknown):
        errors[k] = "unknown parameter"
    for name, spec in schema.items():
        kind = spec[0]
        raw = params.get(name, None)
        if raw is None or raw == "":
            out[name] = spec[1]
            continue
        try:
            if kind == "enum":
                if raw not in spec[2]:
                    raise ValueError(f"must be one of {', '.join(spec[2])}")
                out[name] = raw
            elif kind is bool:
                if isinstance(raw, bool):
                    out[name] = raw
                elif str(raw).lower() in ("1", "true", "yes", "on"):
                    out[name] = True
                elif str(raw).lower() in ("0", "false", "no", "off"):
                    out[name] = False
                else:
                    raise ValueError("must be true or false")
            elif kind is str:
                v = str(raw)
                if len(v) > 100:
                    raise ValueError("too long")
                out[name] = v
            else:
                if isinstance(raw, bool):
                    raise ValueError("must be a number")
                v = kind(float(raw)) if kind is int else float(raw)
                if kind is int and float(raw) != int(float(raw)):
                    raise ValueError("must be a whole number")
                lo, hi = spec[2], spec[3]
                if not (lo <= v <= hi):
                    raise ValueError(f"must be between {lo:g} and {hi:g}")
                out[name] = v
        except (TypeError, ValueError, OverflowError) as e:
            msg = str(e)
            errors[name] = msg if msg and "could not convert" not in msg else "invalid value"
    if errors:
        raise ValidationError(errors)
    return out


def describe(schema: Dict[str, tuple]) -> Dict[str, Any]:
    """JSON-friendly schema for the frontend (defaults, ranges, choices)."""
    out = {}
    for name, spec in schema.items():
        kind = spec[0]
        if kind == "enum":
            out[name] = {"type": "enum", "default": spec[1], "choices": list(spec[2])}
        elif kind is bool:
            out[name] = {"type": "bool", "default": spec[1]}
        elif kind is str:
            out[name] = {"type": "string", "default": spec[1]}
        else:
            out[name] = {"type": "int" if kind is int else "float", "default": spec[1],
                         "min": spec[2], "max": spec[3]}
    return out
