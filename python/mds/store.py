"""Python mirror of the C++ ``TickStore`` key schema (read + write).

Key layout (``{SYM}`` is a Redis Cluster hash tag)::

    mds:{SYM}:ts:<field>    RedisTimeSeries series (or ZSET fallback)
    mds:{SYM}:chunk:<n>     packed MarketEvent records
    mds:{SYM}:chunks        ZSET  score=first_ts_ms  member=n
    mds:{SYM}:meta          HASH  events, chunks, first_ts, last_ts, schema, backend
"""
from __future__ import annotations

from collections import OrderedDict
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

from .book import OrderBook
from .codec import EVENT_SIZE, MarketEvent, PRICE_SCALE, decode_chunk, encode_chunk
from .resp import RedisClient, RedisReplyError, as_str

FIELDS = ("bid", "ask", "mid", "spread", "imbalance", "trade_px", "trade_qty")
Sample = Tuple[int, float]


def aggregate(samples: Sequence[Sample], agg: str, bucket_ms: int) -> List[Sample]:
    """Client-side bucketing identical to RedisTimeSeries AGGREGATION (epoch-aligned)."""
    if not agg or agg == "none" or bucket_ms <= 0:
        return list(samples)
    buckets: "OrderedDict[int, List[float]]" = OrderedDict()
    for ts, v in samples:
        buckets.setdefault(ts - ts % bucket_ms, []).append(v)
    fn = {
        "avg": lambda xs: sum(xs) / len(xs),
        "min": min,
        "max": max,
        "first": lambda xs: xs[0],
        "last": lambda xs: xs[-1],
        "sum": sum,
        "count": lambda xs: float(len(xs)),
        "range": lambda xs: max(xs) - min(xs),
    }[agg]
    return [(b, float(fn(xs))) for b, xs in buckets.items()]


class TickStore:
    def __init__(self, redis: RedisClient, symbol: str = "SIM", prefix: str = "mds",
                 force_zset: bool = False, chunk_events: int = 4096) -> None:
        self.redis = redis
        self.symbol = symbol
        self.prefix = prefix
        self.chunk_events = chunk_events
        self.use_ts = False
        if not force_zset:
            probe = redis.execute("TS.INFO", self.key("ts:__probe__"), raise_errors=False)
            self.use_ts = not (isinstance(probe, RedisReplyError) and "unknown command" in str(probe).lower())

    def key(self, suffix: str) -> str:
        return f"{self.prefix}:{{{self.symbol}}}:{suffix}"

    # -- read path ---------------------------------------------------------------
    def meta(self) -> Dict[str, str]:
        flat = self.redis.execute("HGETALL", self.key("meta")) or []
        return {as_str(flat[i]): as_str(flat[i + 1]) for i in range(0, len(flat), 2)}

    def range(self, field: str, from_ms: int = 0, to_ms: int = 2**62,
              agg: str = "", bucket_ms: int = 0) -> List[Sample]:
        k = self.key(f"ts:{field}")
        if self.use_ts:
            args: List = ["TS.RANGE", k, from_ms, to_ms]
            if agg and agg != "none" and bucket_ms > 0:
                args += ["AGGREGATION", agg, bucket_ms]
            return [(int(ts), float(v)) for ts, v in self.redis.execute(*args)]
        members = self.redis.execute("ZRANGEBYSCORE", k, from_ms, to_ms)
        raw = []
        for m in members:
            ts, _, v = as_str(m).partition(":")
            raw.append((int(ts), float(v)))
        return aggregate(raw, agg, bucket_ms)

    def load_events(self, from_ns: int = 0, to_ns: int = 2**63 - 1) -> List[MarketEvent]:
        from_ms, to_ms = from_ns // 1_000_000, to_ns // 1_000_000
        self.redis.append("ZREVRANGEBYSCORE", self.key("chunks"), f"({from_ms}", "-inf", "LIMIT", 0, 1)
        self.redis.append("ZRANGEBYSCORE", self.key("chunks"), from_ms, to_ms)
        ids = sorted({int(x) for reply in self.redis.flush() for x in reply})
        events: List[MarketEvent] = []
        for i in range(0, len(ids), 64):
            blobs = self.redis.execute("MGET", *[self.key(f"chunk:{c}") for c in ids[i:i + 64]])
            for blob in blobs:
                if blob:
                    events.extend(e for e in decode_chunk(blob) if from_ns <= e.ts <= to_ns)
        return events

    # -- write path (used by scripts/ingest_events.py and the tests) ---------------
    def init(self, retention_ms: int = 0) -> None:
        if self.use_ts:
            for f in FIELDS:
                args: List = ["TS.CREATE", self.key(f"ts:{f}")]
                if retention_ms:
                    args += ["RETENTION", retention_ms]
                args += ["DUPLICATE_POLICY", "SUM" if f == "trade_qty" else "LAST",
                         "LABELS", "symbol", self.symbol, "field", f, "source", "mds"]
                self.redis.append(*args)
            for r in self.redis.flush():
                if isinstance(r, RedisReplyError) and "already exists" not in str(r).lower():
                    raise r
        self.redis.execute("HSET", self.key("meta"), "schema", "MDSEVT01", "event_bytes", EVENT_SIZE,
                           "backend", "redistimeseries" if self.use_ts else "zset")

    def ingest(self, events: Iterable[MarketEvent], pipeline_depth: int = 256) -> Dict[str, int]:
        """Replay events through a reference book and write series + chunks,
        with the same per-millisecond down-sampling rules as the C++ store."""
        book = OrderBook()
        stats = {"events": 0, "chunks": 0, "samples": 0}
        existing = self.redis.execute("HGET", self.key("meta"), "chunks")
        next_chunk = int(existing) if existing else 0
        chunk: List[MarketEvent] = []
        snap: Optional[Tuple[int, Tuple[float, ...]]] = None
        trade: Optional[List] = None  # [ms, last_px, qty]
        first_ts: Optional[int] = None

        def emit(field: str, ms: int, value: float) -> None:
            k = self.key(f"ts:{field}")
            v = repr(float(value))
            if self.use_ts:
                self.redis.append("TS.ADD", k, ms, v)
            else:
                self.redis.append("ZADD", k, ms, f"{ms}:{v}")
            stats["samples"] += 1

        def emit_snap(s: Tuple[int, Tuple[float, ...]]) -> None:
            for f, v in zip(FIELDS[:5], s[1]):
                emit(f, s[0], v)

        def seal() -> None:
            nonlocal next_chunk, chunk
            if not chunk:
                return
            cid = next_chunk
            next_chunk += 1
            self.redis.append("SET", self.key(f"chunk:{cid}"), encode_chunk(chunk))
            self.redis.append("ZADD", self.key("chunks"), chunk[0].ts // 1_000_000, cid)
            self.redis.append("HINCRBY", self.key("meta"), "events", len(chunk))
            self.redis.append("HSETNX", self.key("meta"), "first_ts", first_ts)
            self.redis.append("HSET", self.key("meta"), "chunks", next_chunk, "last_ts", chunk[-1].ts)
            stats["chunks"] += 1
            chunk = []

        def maybe_flush() -> None:
            if len(self.redis._pending) >= pipeline_depth:  # noqa: SLF001 - same package
                self._check(self.redis.flush())

        for ev in events:
            first_ts = ev.ts if first_ts is None else first_ts
            for f in book.apply(ev):
                ms = f.ts // 1_000_000
                if trade and trade[0] != ms:
                    emit("trade_px", trade[0], trade[1])
                    emit("trade_qty", trade[0], trade[2])
                    trade = None
                if trade is None:
                    trade = [ms, 0.0, 0.0]
                trade[1] = f.price / PRICE_SCALE
                trade[2] += f.qty
            b, a = book.best_bid(), book.best_ask()
            if b is not None and a is not None:
                ms = ev.ts // 1_000_000
                if snap and snap[0] != ms:
                    emit_snap(snap)
                bid, ask = b / PRICE_SCALE, a / PRICE_SCALE
                snap = (ms, (bid, ask, (bid + ask) / 2, ask - bid, book.imbalance(5)))
            chunk.append(ev)
            stats["events"] += 1
            if len(chunk) >= self.chunk_events:
                seal()
            maybe_flush()

        if snap:
            emit_snap(snap)
        if trade:
            emit("trade_px", trade[0], trade[1])
            emit("trade_qty", trade[0], trade[2])
        seal()
        self._check(self.redis.flush())
        return stats

    def purge(self) -> None:
        chunks = self.redis.execute("ZRANGE", self.key("chunks"), 0, -1) or []
        keys = [self.key("chunks"), self.key("meta")] + [self.key(f"ts:{f}") for f in FIELDS]
        keys += [self.key(f"chunk:{as_str(c)}") for c in chunks]
        self.redis.execute("DEL", *keys)

    @staticmethod
    def _check(replies: List) -> None:
        for r in replies:
            if isinstance(r, RedisReplyError):
                raise r
