"""Live market manager.

Runs one ``mds_live`` process (the real-time C++ engine) and fans its JSON
Lines output out to every connected browser via Server-Sent Events. All
viewers watch the same market, like a real exchange feed.

* Each subscriber gets a bounded queue; a slow client that falls behind has
  its oldest updates dropped instead of slowing everyone else down.
* New subscribers receive the current config plus a short history (mid
  price, strategy PnL, recent trades) so charts are populated immediately.
* If the engine exits unexpectedly it is restarted with back-off.
* Benchmarks pause the market (the engine is stopped, then restarted with
  the same settings) so they get a quiet machine.
"""
from __future__ import annotations

import json
import queue
import subprocess
import threading
import time
from collections import deque
from typing import Any, Deque, Dict, List, Optional

from .config import Config
from .validation import schemas, validate

HISTORY_POINTS = 1200  # ~5 minutes at the default 250 ms snapshot interval


class LiveManager:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        self.schema = schemas(cfg.max_events, cfg.max_bench_messages)["live"]
        self.params: Dict[str, Any] = validate(self.schema, {})
        self._lock = threading.RLock()
        self._proc: Optional[subprocess.Popen] = None
        self._reader: Optional[threading.Thread] = None
        self._subscribers: List["queue.Queue[str]"] = []
        self._hello: Optional[Dict[str, Any]] = None
        self._last_tick: Optional[Dict[str, Any]] = None
        self._history: Deque[List[float]] = deque(maxlen=HISTORY_POINTS)
        self._fills: Deque[list] = deque(maxlen=200)
        self._wanted = False          # should the engine be running?
        self._paused = False          # temporarily stopped for a benchmark
        self._restarts = 0
        self._last_start = 0.0
        self._error: Optional[str] = None
        self._generation = 0          # bumps on every (re)start; stale readers exit

    # ---------------------------------------------------------------- control
    def start(self, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        with self._lock:
            if params is not None:
                self.params = validate(self.schema, params)
            if time.time() - self._last_start < 1.0:
                raise RuntimeError("the market was restarted less than a second ago")
            self._wanted = True
            self._restarts = 0
            self._spawn()
            return self.status()

    def stop(self) -> Dict[str, Any]:
        with self._lock:
            self._wanted = False
            self._kill()
            self._broadcast("status", self.status())
            return self.status()

    def pause(self, paused: bool) -> None:
        with self._lock:
            self._paused = paused
            if paused:
                self._kill()
            elif self._wanted:
                self._spawn()
            self._broadcast("status", self.status())

    def shutdown(self) -> None:
        with self._lock:
            self._wanted = False
            self._kill()

    def _spawn(self) -> None:
        self._kill()
        if self._paused:
            return
        p = self.params
        args = [str(self.cfg.binary("mds_live")), "--strategy", p["strategy"], "--rate", repr(p["rate"]),
                "--speed", repr(p["speed"]), "--snapshot-ms", str(p["snapshot_ms"]),
                "--latency-us", repr(p["latency_us"]), "--vol", repr(p["vol"]),
                "--informed", repr(p["informed"]), "--impact", repr(p["impact"]), "--levels", "12"]
        if p.get("seed") is not None:
            args += ["--seed", str(p["seed"])]
        if p.get("spin"):
            args.append("--spin")
            if self.cfg.live_pin:
                args += ["--pin", self.cfg.live_pin]
        try:
            self._proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                          text=True, bufsize=1)
        except OSError as e:
            self._error = f"cannot start engine: {e}"
            self._wanted = False
            return
        self._error = None
        self._last_start = time.time()
        self._generation += 1
        self._history.clear()
        self._fills.clear()
        self._last_tick = None
        self._reader = threading.Thread(target=self._read, args=(self._proc, self._generation),
                                        name="live-reader", daemon=True)
        self._reader.start()

    def _kill(self) -> None:
        proc, self._proc = self._proc, None
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()

    # ---------------------------------------------------------------- reading
    def _read(self, proc: subprocess.Popen, generation: int) -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            if generation != self._generation:
                return
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = msg.get("type")
            with self._lock:
                if generation != self._generation:
                    return  # superseded by a restart while we waited for the lock
                if kind == "hello":
                    self._hello = msg
                    self._broadcast("hello", {"config": msg, "params": self.params})
                elif kind == "tick":
                    self._on_tick(msg)
        # Process ended: collect its error output and release the pipes.
        err = (proc.stderr.read() if proc.stderr else "").strip()
        for pipe in (proc.stdout, proc.stderr):
            if pipe:
                pipe.close()
        proc.wait()
        with self._lock:
            if generation != self._generation:
                return
            self._proc = None
            if self._wanted and not self._paused:
                self._restarts += 1
                self._error = err or f"engine exited with code {proc.returncode}"
                delay = min(30, 2 ** self._restarts)
                self._broadcast("status", self.status())
                threading.Timer(delay, self._retry, args=(generation,)).start()

    def _retry(self, generation: int) -> None:
        with self._lock:
            if self._wanted and not self._paused and generation == self._generation and self._proc is None:
                self._spawn()

    def _on_tick(self, msg: Dict[str, Any]) -> None:
        self._last_tick = msg
        book = msg.get("book", {})
        strat = msg.get("strategy", {})
        if "mid" in book:
            self._history.append([msg["sim_ts"] / 1e6, book["mid"], strat.get("pnl", 0.0),
                                  strat.get("position", 0)])
        for f in strat.get("fills", []):
            self._fills.append(f)
        self._broadcast("tick", msg)

    # ------------------------------------------------------------ subscribers
    def subscribe(self) -> "queue.Queue[str]":
        with self._lock:
            if len(self._subscribers) >= self.cfg.live_max_clients:
                raise RuntimeError("too many live viewers")
            q: "queue.Queue[str]" = queue.Queue(maxsize=64)
            self._subscribers.append(q)
            # Prime the new client.
            q.put(self._sse("status", self.status()))
            if self._hello:
                q.put(self._sse("hello", {"config": self._hello, "params": self.params}))
            q.put(self._sse("history", {"points": list(self._history), "fills": list(self._fills)}))
            if self._last_tick:
                q.put(self._sse("tick", self._last_tick))
            return q

    def unsubscribe(self, q: "queue.Queue[str]") -> None:
        with self._lock:
            if q in self._subscribers:
                self._subscribers.remove(q)

    @staticmethod
    def _sse(event: str, data: Any) -> str:
        return f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n"

    def _broadcast(self, event: str, data: Any) -> None:
        payload = self._sse(event, data)
        for q in list(self._subscribers):
            try:
                q.put_nowait(payload)
            except queue.Full:
                # Slow client: drop its oldest message rather than block the feed.
                try:
                    q.get_nowait()
                    q.put_nowait(payload)
                except (queue.Empty, queue.Full):
                    pass

    # ---------------------------------------------------------------- status
    def status(self) -> Dict[str, Any]:
        running = self._proc is not None and self._proc.poll() is None
        state = "paused" if self._paused and self._wanted else "running" if running else (
            "restarting" if self._wanted else "stopped")
        return {"state": state, "params": self.params, "viewers": len(self._subscribers),
                "error": self._error, "restarts": self._restarts,
                "uptime_s": round(time.time() - self._last_start, 1) if running else 0,
                "schema_defaults": {k: v[1] for k, v in self.schema.items()}}
