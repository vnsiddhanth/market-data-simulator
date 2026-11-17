#!/usr/bin/env python3
"""In-memory RESP2 server implementing the Redis subset used by this project.

Lets the C++ binaries, the Python tooling and CI exercise the full Redis path
without a real server:

    python python/scripts/fake_redis.py --port 6399            # with TS.* commands
    python python/scripts/fake_redis.py --port 6399 --no-ts    # vanilla-Redis behaviour

Supported: PING ECHO SET GET MGET DEL EXISTS KEYS DBSIZE FLUSHDB
           HSET HSETNX HGET HGETALL HINCRBY
           ZADD ZCARD ZRANGE ZRANGEBYSCORE ZREVRANGEBYSCORE
           TS.CREATE TS.INFO TS.ADD TS.MADD TS.RANGE  (RedisTimeSeries semantics
           for DUPLICATE_POLICY LAST/SUM/FIRST/MIN/MAX/BLOCK and epoch-aligned
           AGGREGATION avg/min/max/first/last/sum/count/range)

This is a test double, not a database: single process, no persistence, no
expiry, no auth. It must never be exposed beyond localhost.
"""
from __future__ import annotations

import argparse
import asyncio
import fnmatch
import sys
from bisect import bisect_left, bisect_right, insort
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, __file__.rsplit("scripts", 1)[0])  # make `mds` importable
from mds.store import aggregate  # noqa: E402


class Err(Exception):
    pass


class TimeSeries:
    def __init__(self, dup_policy: str = "BLOCK", labels: Optional[Dict[str, str]] = None) -> None:
        self.dup = dup_policy.upper()
        self.labels = labels or {}
        self.ts: List[int] = []
        self.vals: Dict[int, float] = {}

    def add(self, t: int, v: float) -> int:
        if t in self.vals:
            old = self.vals[t]
            if self.dup == "BLOCK":
                raise Err("ERR TSDB: Error at upsert, update is not supported when DUPLICATE_POLICY is set to BLOCK mode")
            v = {"LAST": v, "FIRST": old, "SUM": old + v, "MIN": min(old, v), "MAX": max(old, v)}[self.dup]
        else:
            insort(self.ts, t)
        self.vals[t] = v
        return t

    def range(self, lo: int, hi: int) -> List[Tuple[int, float]]:
        i, j = bisect_left(self.ts, lo), bisect_right(self.ts, hi)
        return [(t, self.vals[t]) for t in self.ts[i:j]]


def fmt_num(v: float) -> str:
    return str(int(v)) if v == int(v) and abs(v) < 1e15 else repr(v)


def parse_bound(s: str, is_min: bool) -> Tuple[float, bool]:
    """ZRANGEBYSCORE bound -> (value, exclusive)."""
    if s in ("-inf", "+inf", "inf"):
        return (float("-inf") if s == "-inf" else float("inf")), False
    if s.startswith("("):
        return float(s[1:]), True
    return float(s), False


class Store:
    def __init__(self, timeseries: bool) -> None:
        self.timeseries = timeseries
        self.data: Dict[str, object] = {}

    # ---------------------------------------------------------------- helpers
    def _get(self, key: str, typ: type):
        v = self.data.get(key)
        if v is not None and not isinstance(v, typ):
            raise Err("WRONGTYPE Operation against a key holding the wrong kind of value")
        return v

    def _zset(self, key: str, create: bool = False) -> Dict[str, float]:
        z = self._get(key, ZSet)
        if z is None and create:
            z = self.data[key] = ZSet()
        return z if z is not None else ZSet()

    def _hash(self, key: str, create: bool = False) -> Dict[str, bytes]:
        h = self._get(key, HashDict)
        if h is None and create:
            h = self.data[key] = HashDict()
        return h if h is not None else HashDict()

    @staticmethod
    def _sorted(z: Dict[str, float]) -> List[Tuple[str, float]]:
        return sorted(z.items(), key=lambda kv: (kv[1], kv[0]))

    # --------------------------------------------------------------- dispatch
    def execute(self, argv: List[bytes]):
        if not argv:
            raise Err("ERR empty command")
        cmd = argv[0].decode().upper()
        args = argv[1:]
        if cmd.startswith("TS.") and not self.timeseries:
            raise Err(f"ERR unknown command '{cmd}', with args beginning with: ")
        fn = getattr(self, "cmd_" + cmd.replace(".", "_"), None)
        if fn is None:
            raise Err(f"ERR unknown command '{cmd}', with args beginning with: ")
        return fn(args)

    # ---------------------------------------------------------------- generic
    def cmd_PING(self, a):
        return Status("PONG") if not a else a[0]

    def cmd_ECHO(self, a):
        return a[0]

    def cmd_FLUSHDB(self, a):
        self.data.clear()
        return Status("OK")

    def cmd_DBSIZE(self, a):
        return len(self.data)

    def cmd_DEL(self, a):
        return sum(1 for k in a if self.data.pop(k.decode(), None) is not None)

    def cmd_EXISTS(self, a):
        return sum(1 for k in a if k.decode() in self.data)

    def cmd_KEYS(self, a):
        pat = a[0].decode()
        return [k.encode() for k in self.data if fnmatch.fnmatchcase(k, pat)]

    def cmd_MODULE(self, a):
        if self.timeseries:
            return [[b"name", b"timeseries", b"ver", 11200]]
        return []

    # ---------------------------------------------------------------- strings
    def cmd_SET(self, a):
        self.data[a[0].decode()] = bytes(a[1])
        return Status("OK")

    def cmd_GET(self, a):
        return self._get(a[0].decode(), bytes)

    def cmd_MGET(self, a):
        out = []
        for k in a:
            v = self.data.get(k.decode())
            out.append(v if isinstance(v, bytes) else None)
        return out

    # ----------------------------------------------------------------- hashes
    def cmd_HSET(self, a):
        h = self._hash(a[0].decode(), create=True)
        added = 0
        for i in range(1, len(a), 2):
            f = a[i].decode()
            added += f not in h
            h[f] = bytes(a[i + 1])
        return added

    def cmd_HSETNX(self, a):
        h = self._hash(a[0].decode(), create=True)
        f = a[1].decode()
        if f in h:
            return 0
        h[f] = bytes(a[2])
        return 1

    def cmd_HGET(self, a):
        return self._hash(a[0].decode()).get(a[1].decode())

    def cmd_HGETALL(self, a):
        out = []
        for k, v in self._hash(a[0].decode()).items():
            out += [k.encode(), v]
        return out

    def cmd_HINCRBY(self, a):
        h = self._hash(a[0].decode(), create=True)
        f = a[1].decode()
        v = int(h.get(f, b"0")) + int(a[2])
        h[f] = str(v).encode()
        return v

    # ----------------------------------------------------------- sorted sets
    def cmd_ZADD(self, a):
        z = self._zset(a[0].decode(), create=True)
        added = 0
        for i in range(1, len(a), 2):
            m = a[i + 1].decode()
            added += m not in z
            z[m] = float(a[i])
        return added

    def cmd_ZCARD(self, a):
        return len(self._zset(a[0].decode()))

    def cmd_ZRANGE(self, a):
        items = self._sorted(self._zset(a[0].decode()))
        start, stop = int(a[1]), int(a[2])
        n = len(items)
        start = max(start + n if start < 0 else start, 0)
        stop = stop + n if stop < 0 else stop
        return [m.encode() for m, _ in items[start:stop + 1]]

    def _by_score(self, a, reverse: bool):
        key = a[0].decode()
        hi_s, lo_s = (a[1].decode(), a[2].decode()) if reverse else (a[2].decode(), a[1].decode())
        lo, lo_ex = parse_bound(lo_s, True)
        hi, hi_ex = parse_bound(hi_s, False)
        items = [(m, s) for m, s in self._sorted(self._zset(key))
                 if (s > lo if lo_ex else s >= lo) and (s < hi if hi_ex else s <= hi)]
        if reverse:
            items.reverse()
        rest = [x.decode().upper() for x in a[3:]]
        if "LIMIT" in rest:
            i = rest.index("LIMIT")
            off, cnt = int(rest[i + 1]), int(rest[i + 2])
            items = items[off:] if cnt < 0 else items[off:off + cnt]
        return [m.encode() for m, _ in items]

    def cmd_ZRANGEBYSCORE(self, a):
        return self._by_score(a, reverse=False)

    def cmd_ZREVRANGEBYSCORE(self, a):
        return self._by_score(a, reverse=True)

    # ------------------------------------------------------------ timeseries
    def _series(self, key: str) -> TimeSeries:
        s = self._get(key, TimeSeries)
        if s is None:
            raise Err("ERR TSDB: the key does not exist")
        return s

    def cmd_TS_CREATE(self, a):
        key = a[0].decode()
        if key in self.data:
            raise Err("ERR TSDB: key already exists")
        opts = [x.decode() for x in a[1:]]
        dup, labels = "BLOCK", {}
        i = 0
        while i < len(opts):
            o = opts[i].upper()
            if o == "DUPLICATE_POLICY":
                dup = opts[i + 1]
                i += 2
            elif o == "RETENTION":
                i += 2
            elif o == "LABELS":
                rest = opts[i + 1:]
                labels = dict(zip(rest[::2], rest[1::2]))
                break
            else:
                i += 1
        self.data[key] = TimeSeries(dup, labels)
        return Status("OK")

    def cmd_TS_INFO(self, a):
        s = self._series(a[0].decode())
        return [b"totalSamples", len(s.ts), b"duplicatePolicy", s.dup.lower().encode(),
                b"labels", [[k.encode(), v.encode()] for k, v in s.labels.items()]]

    def cmd_TS_ADD(self, a):
        key = a[0].decode()
        if key not in self.data:
            self.data[key] = TimeSeries("BLOCK")
        return self._series(key).add(int(a[1]), float(a[2]))

    def cmd_TS_MADD(self, a):
        out = []
        for i in range(0, len(a), 3):
            try:
                out.append(self._series(a[i].decode()).add(int(a[i + 1]), float(a[i + 2])))
            except Err as e:
                out.append(e)
        return out

    def cmd_TS_RANGE(self, a):
        s = self._series(a[0].decode())
        lo = 0 if a[1] == b"-" else int(a[1])
        hi = 2**63 - 1 if a[2] == b"+" else int(a[2])
        pts = s.range(lo, hi)
        rest = [x.decode() for x in a[3:]]
        upper = [x.upper() for x in rest]
        if "AGGREGATION" in upper:
            i = upper.index("AGGREGATION")
            pts = aggregate(pts, rest[i + 1].lower(), int(rest[i + 2]))
        return [[t, Status(fmt_num(v))] for t, v in pts]


class HashDict(dict):
    pass


class ZSet(dict):
    pass


class Status(str):
    pass


def encode(v) -> bytes:
    if isinstance(v, Status):
        return b"+" + v.encode() + b"\r\n"
    if isinstance(v, Err):
        return b"-" + str(v).encode() + b"\r\n"
    if v is None:
        return b"$-1\r\n"
    if isinstance(v, bool):
        return b":%d\r\n" % int(v)
    if isinstance(v, int):
        return b":%d\r\n" % v
    if isinstance(v, (bytes, bytearray)):
        return b"$%d\r\n%s\r\n" % (len(v), v)
    if isinstance(v, str):
        b = v.encode()
        return b"$%d\r\n%s\r\n" % (len(b), b)
    if isinstance(v, list):
        return b"*%d\r\n" % len(v) + b"".join(encode(x) for x in v)
    raise TypeError(type(v))


async def read_command(reader: asyncio.StreamReader) -> Optional[List[bytes]]:
    line = await reader.readline()
    if not line:
        return None
    if not line.startswith(b"*"):
        return line.strip().split()  # inline command (e.g. from telnet / redis-cli)
    argv = []
    for _ in range(int(line[1:].strip())):
        hdr = await reader.readline()
        n = int(hdr[1:].strip())
        data = await reader.readexactly(n + 2)
        argv.append(data[:-2])
    return argv


def make_handler(store: Store):
    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            while True:
                argv = await read_command(reader)
                if argv is None:
                    break
                try:
                    reply = store.execute(argv)
                except Err as e:
                    reply = e
                except Exception as e:  # malformed args etc.
                    reply = Err(f"ERR {type(e).__name__}: {e}")
                writer.write(encode(reply))
                if reader._buffer == b"":  # noqa: SLF001 - flush once the pipeline is drained
                    await writer.drain()
            await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            writer.close()

    return handle


async def serve(host: str, port: int, timeseries: bool, ready: Optional[asyncio.Event] = None) -> None:
    server = await asyncio.start_server(make_handler(Store(timeseries)), host, port)
    if ready is not None:
        ready.set()
    async with server:
        await server.serve_forever()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6399)
    ap.add_argument("--no-ts", action="store_true", help="behave like vanilla Redis (no TS.* commands)")
    args = ap.parse_args()
    print(f"fake redis on {args.host}:{args.port} timeseries={'off' if args.no_ts else 'on'}", flush=True)
    try:
        asyncio.run(serve(args.host, args.port, not args.no_ts))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
