"""Minimal RESP2 Redis client (stdlib only) with pipelining.

Mirrors ``include/mds/storage/redis_client.hpp``. If you already use
``redis-py`` you can swap it in; this exists so the tooling has zero
third-party dependencies.
"""
from __future__ import annotations

import socket
from typing import Any, List, Optional, Sequence, Union

Arg = Union[str, bytes, int, float]


class RedisReplyError(Exception):
    """An error reply (``-ERR ...``) returned by the server."""


def _to_bytes(a: Arg) -> bytes:
    if isinstance(a, bytes):
        return a
    if isinstance(a, float):
        return repr(a).encode()
    return str(a).encode()


def encode_command(args: Sequence[Arg]) -> bytes:
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        b = _to_bytes(a)
        out.append(b"$%d\r\n%s\r\n" % (len(b), b))
    return b"".join(out)


class _Reader:
    def __init__(self, sock: socket.socket) -> None:
        self._sock = sock
        self._buf = bytearray()

    def _fill(self) -> None:
        chunk = self._sock.recv(65536)
        if not chunk:
            raise ConnectionError("redis connection closed")
        self._buf += chunk

    def _line(self) -> bytes:
        while True:
            idx = self._buf.find(b"\r\n")
            if idx >= 0:
                line = bytes(self._buf[:idx])
                del self._buf[: idx + 2]
                return line
            self._fill()

    def _exact(self, n: int) -> bytes:
        while len(self._buf) < n + 2:
            self._fill()
        data = bytes(self._buf[:n])
        del self._buf[: n + 2]
        return data

    def read(self) -> Any:
        line = self._line()
        tag, rest = line[:1], line[1:]
        if tag == b"+":
            return rest.decode()
        if tag == b"-":
            return RedisReplyError(rest.decode())
        if tag == b":":
            return int(rest)
        if tag == b"$":
            n = int(rest)
            return None if n < 0 else self._exact(n)
        if tag == b"*":
            n = int(rest)
            return None if n < 0 else [self.read() for _ in range(n)]
        raise ValueError(f"RESP: unexpected type byte {tag!r}")


class RedisClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 6379, timeout: float = 5.0) -> None:
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._reader = _Reader(self._sock)
        self._pending: List[bytes] = []

    @classmethod
    def from_endpoint(cls, endpoint: str, **kw: Any) -> "RedisClient":
        host, _, port = endpoint.rpartition(":")
        if not host:
            return cls(endpoint, 6379, **kw)
        return cls(host, int(port), **kw)

    def close(self) -> None:
        self._sock.close()

    def __enter__(self) -> "RedisClient":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def execute(self, *args: Arg, raise_errors: bool = True) -> Any:
        self._sock.sendall(encode_command(args))
        reply = self._reader.read()
        if raise_errors and isinstance(reply, RedisReplyError):
            raise reply
        return reply

    # -- pipelining --------------------------------------------------------
    def append(self, *args: Arg) -> None:
        self._pending.append(encode_command(args))

    def flush(self) -> List[Any]:
        if not self._pending:
            return []
        n = len(self._pending)
        self._sock.sendall(b"".join(self._pending))
        self._pending.clear()
        return [self._reader.read() for _ in range(n)]


def as_str(v: Optional[Union[bytes, str]]) -> Optional[str]:
    if v is None or isinstance(v, str):
        return v
    return v.decode()
