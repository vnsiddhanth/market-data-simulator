"""MarketEvent codec, byte-compatible with ``include/mds/common/event.hpp``.

C++ layout (40 bytes, little-endian, no implicit padding)::

    int64   ts        offset  0
    uint64  id        offset  8
    int64   price     offset 16   fixed point, 1 unit = 1e-4
    int64   qty       offset 24
    uint32  symbol    offset 32
    uint8   type      offset 36   0 Add, 1 Cancel, 2 Modify, 3 Market
    uint8   side      offset 37   0 Buy, 1 Sell
    uint16  flags     offset 38
"""
from __future__ import annotations

import csv
import struct
from dataclasses import dataclass
from enum import IntEnum
from typing import Iterable, Iterator, List

PRICE_SCALE = 10_000
EVENT_STRUCT = struct.Struct("<qQqqIBBH")
EVENT_SIZE = EVENT_STRUCT.size
BINARY_MAGIC = b"MDSEVT01"

assert EVENT_SIZE == 40, "must match sizeof(mds::MarketEvent)"


class EventType(IntEnum):
    ADD = 0
    CANCEL = 1
    MODIFY = 2
    MARKET = 3


class Side(IntEnum):
    BUY = 0
    SELL = 1


@dataclass(frozen=True)
class MarketEvent:
    ts: int
    id: int
    price: int  # fixed point
    qty: int
    symbol: int = 0
    type: EventType = EventType.ADD
    side: Side = Side.BUY
    flags: int = 0

    @property
    def price_float(self) -> float:
        return self.price / PRICE_SCALE

    def pack(self) -> bytes:
        return EVENT_STRUCT.pack(self.ts, self.id, self.price, self.qty, self.symbol,
                                 int(self.type), int(self.side), self.flags)

    @classmethod
    def unpack(cls, raw: bytes, offset: int = 0) -> "MarketEvent":
        ts, oid, px, qty, sym, typ, side, flags = EVENT_STRUCT.unpack_from(raw, offset)
        return cls(ts, oid, px, qty, sym, EventType(typ), Side(side), flags)


def decode_chunk(blob: bytes) -> List[MarketEvent]:
    """Decode one Redis event chunk (N packed records)."""
    if len(blob) % EVENT_SIZE:
        raise ValueError(f"chunk length {len(blob)} is not a multiple of {EVENT_SIZE}")
    return [MarketEvent(t, i, p, q, s, EventType(ty), Side(sd), f)
            for t, i, p, q, s, ty, sd, f in EVENT_STRUCT.iter_unpack(blob)]


def encode_chunk(events: Iterable[MarketEvent]) -> bytes:
    return b"".join(e.pack() for e in events)


def read_binary(path: str) -> List[MarketEvent]:
    """Read an MDSEVT01 file written by ``mds_sim --bin``."""
    with open(path, "rb") as f:
        if f.read(8) != BINARY_MAGIC:
            raise ValueError(f"{path}: not an MDSEVT01 file")
        (count,) = struct.unpack("<Q", f.read(8))
        body = f.read(count * EVENT_SIZE)
    if len(body) != count * EVENT_SIZE:
        raise ValueError(f"{path}: truncated body")
    return decode_chunk(body)


def write_binary(path: str, events: List[MarketEvent]) -> None:
    with open(path, "wb") as f:
        f.write(BINARY_MAGIC)
        f.write(struct.pack("<Q", len(events)))
        f.write(encode_chunk(events))


_TYPE_NAMES = {e.name: e for e in EventType}


def iter_csv(path: str) -> Iterator[MarketEvent]:
    """Read the CSV written by ``mds_sim --csv`` (price in currency units)."""
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            yield MarketEvent(
                ts=int(row["ts"]),
                id=int(row["id"]),
                price=round(float(row["price"]) * PRICE_SCALE),
                qty=int(row["qty"]),
                symbol=int(row["symbol"]),
                type=_TYPE_NAMES[row["type"]],
                side=Side.SELL if row["side"] == "SELL" else Side.BUY,
            )
