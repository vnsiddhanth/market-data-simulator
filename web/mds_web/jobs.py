"""Batch job runner: simulations, backtests and benchmarks.

Each job gets its own directory under ``<data>/jobs/<id>/`` holding
``job.json`` (status + result summary), the tool's raw outputs and a log.
Jobs run in a small worker pool (default 1 so benchmarks aren't disturbed by
each other) and survive server restarts: on start-up finished jobs are
reloaded and anything that was running is marked as interrupted.
"""
from __future__ import annotations

import json
import os
import queue
import secrets
import shutil
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from .config import Config
from .validation import schemas, validate

# Files a client may download from a job directory.
DOWNLOADABLE = {
    "summary.json": "application/json",
    "report.json": "application/json",
    "equity.csv": "text/csv",
    "fills.csv": "text/csv",
    "ticks.csv": "text/csv",
    "ticks.bin": "application/octet-stream",
    "bench_queue.json": "application/json",
    "bench_orderbook.json": "application/json",
    "log.txt": "text/plain",
}

ACTIVE = ("queued", "running")


class JobError(Exception):
    pass


def _now() -> float:
    return round(time.time(), 3)


def _write_json_atomic(path: Path, data: Any) -> None:
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=1))
    os.replace(tmp, path)


def _downsample(rows: List[Any], n: int) -> List[Any]:
    if len(rows) <= n:
        return rows
    step = len(rows) / n
    out = [rows[int(i * step)] for i in range(n)]
    out[-1] = rows[-1]
    return out


class JobManager:
    def __init__(self, cfg: Config, live_pause: Optional[Callable[[bool], None]] = None) -> None:
        self.cfg = cfg
        self.jobs_dir = cfg.data_dir / "jobs"
        self.jobs_dir.mkdir(parents=True, exist_ok=True)
        self.schemas = schemas(cfg.max_events, cfg.max_bench_messages)
        self._jobs: Dict[str, Dict[str, Any]] = {}
        self._procs: Dict[str, subprocess.Popen] = {}
        self._lock = threading.RLock()
        self._queue: "queue.Queue[str]" = queue.Queue()
        self._live_pause = live_pause
        self._stopping = False
        self._load()
        self._workers = [threading.Thread(target=self._worker, name=f"job-worker-{i}", daemon=True)
                         for i in range(max(1, cfg.workers))]
        for w in self._workers:
            w.start()

    # ------------------------------------------------------------------ state
    def _load(self) -> None:
        for d in sorted(self.jobs_dir.iterdir()) if self.jobs_dir.exists() else []:
            f = d / "job.json"
            if not f.exists():
                continue
            try:
                job = json.loads(f.read_text())
            except (OSError, json.JSONDecodeError):
                continue
            if job.get("status") in ACTIVE:
                job["status"] = "failed"
                job["error"] = "interrupted by a server restart"
                job["finished_at"] = _now()
                _write_json_atomic(f, job)
            self._jobs[job["id"]] = job

    def _save(self, job: Dict[str, Any]) -> None:
        _write_json_atomic(self.jobs_dir / job["id"] / "job.json", job)

    def _public(self, job: Dict[str, Any], full: bool) -> Dict[str, Any]:
        out = {k: v for k, v in job.items() if full or k != "result"}
        if not full and job.get("result"):
            out["headline"] = job["result"].get("headline")
        d = self.jobs_dir / job["id"]
        out["files"] = [n for n in DOWNLOADABLE if (d / n).exists()]
        if job["status"] == "queued":
            out["queue_position"] = self._queue_position(job["id"])
        return out

    def _queue_position(self, job_id: str) -> int:
        queued = sorted((j for j in self._jobs.values() if j["status"] == "queued"),
                        key=lambda j: j["created_at"])
        for i, j in enumerate(queued):
            if j["id"] == job_id:
                return i + 1
        return 0

    # ------------------------------------------------------------------- API
    def submit(self, kind: str, params: Any) -> Dict[str, Any]:
        if kind not in ("simulation", "backtest", "benchmark"):
            raise JobError(f"unknown job kind '{kind}'")
        clean = validate(self.schemas[kind], params)
        if kind == "simulation" and clean["redis"] and not self.cfg.redis:
            raise JobError("Redis is not configured on this server (set MDS_REDIS)")
        if kind == "backtest":
            self._check_source(clean["source"])
        # An empty seed means "random", but record the one we pick so every
        # run stays reproducible from its saved parameters.
        if "seed" in clean and clean["seed"] is None and not str(clean.get("source", "synthetic")).startswith(("simulation:", "redis:")):
            clean["seed"] = secrets.randbelow(2**31)
        missing = self.cfg.missing_binaries()
        if missing:
            raise JobError(f"engine binaries not found in {self.cfg.bin_dir}: {', '.join(missing)}")
        with self._lock:
            active = sum(1 for j in self._jobs.values() if j["status"] in ACTIVE)
            if active >= self.cfg.max_queued:
                raise JobError(f"too many jobs in progress ({active}); try again shortly")
            job_id = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
            job = {"id": job_id, "kind": kind, "params": clean, "status": "queued",
                   "created_at": _now(), "started_at": None, "finished_at": None,
                   "error": None, "result": None}
            (self.jobs_dir / job_id).mkdir(parents=True)
            self._jobs[job_id] = job
            self._save(job)
            self._queue.put(job_id)
            self._prune()
            return self._public(job, full=True)

    def _check_source(self, source: str) -> None:
        if source == "synthetic":
            return
        if source.startswith("simulation:"):
            sim_id = source.split(":", 1)[1]
            job = self._jobs.get(sim_id)
            if not job or job["kind"] != "simulation" or job["status"] != "succeeded":
                raise JobError(f"source simulation '{sim_id}' not found or not finished")
            if not (self.jobs_dir / sim_id / "ticks.bin").exists():
                raise JobError("that simulation did not save its events (enable 'save events')")
            return
        if source.startswith("redis:"):
            if not self.cfg.redis:
                raise JobError("Redis is not configured on this server")
            sym = source.split(":", 1)[1]
            if not sym or not all(c.isalnum() or c in "-_" for c in sym):
                raise JobError("invalid Redis symbol")
            return
        raise JobError("source must be 'synthetic', 'simulation:<id>' or 'redis:<symbol>'")

    def list(self, kind: Optional[str] = None, limit: int = 100) -> List[Dict[str, Any]]:
        with self._lock:
            jobs = sorted(self._jobs.values(), key=lambda j: j["created_at"], reverse=True)
            if kind:
                jobs = [j for j in jobs if j["kind"] == kind]
            return [self._public(j, full=False) for j in jobs[:limit]]

    def get(self, job_id: str) -> Optional[Dict[str, Any]]:
        with self._lock:
            job = self._jobs.get(job_id)
            return self._public(job, full=True) if job else None

    def file_path(self, job_id: str, name: str) -> Optional[Path]:
        if name not in DOWNLOADABLE or job_id not in self._jobs:
            return None
        p = self.jobs_dir / job_id / name
        return p if p.exists() else None

    def delete(self, job_id: str) -> bool:
        with self._lock:
            job = self._jobs.get(job_id)
            if not job:
                return False
            if job["status"] == "running":
                proc = self._procs.get(job_id)
                if proc:
                    proc.kill()
                job["status"] = "cancelled"
                job["finished_at"] = _now()
                self._save(job)
                return True
            if job["status"] == "queued":
                job["status"] = "cancelled"
                job["finished_at"] = _now()
                self._save(job)
                return True
            del self._jobs[job_id]
        shutil.rmtree(self.jobs_dir / job_id, ignore_errors=True)
        return True

    def stats(self) -> Dict[str, int]:
        with self._lock:
            out = {"queued": 0, "running": 0, "total": len(self._jobs)}
            for j in self._jobs.values():
                if j["status"] in out:
                    out[j["status"]] += 1
            return out

    def shutdown(self) -> None:
        self._stopping = True
        with self._lock:
            for p in self._procs.values():
                p.kill()

    def _prune(self) -> None:
        done = sorted((j for j in self._jobs.values() if j["status"] not in ACTIVE),
                      key=lambda j: j["created_at"])
        for job in done[: max(0, len(done) - self.cfg.keep_jobs)]:
            del self._jobs[job["id"]]
            shutil.rmtree(self.jobs_dir / job["id"], ignore_errors=True)

    # --------------------------------------------------------------- workers
    def _worker(self) -> None:
        while not self._stopping:
            job_id = self._queue.get()
            with self._lock:
                job = self._jobs.get(job_id)
                if not job or job["status"] != "queued":
                    continue
                job["status"] = "running"
                job["started_at"] = _now()
                self._save(job)
            try:
                result = getattr(self, f"_run_{job['kind']}")(job)
                with self._lock:
                    if job["status"] == "running":
                        job["status"] = "succeeded"
                        job["result"] = result
            except Exception as e:  # noqa: BLE001 - report any failure to the user
                with self._lock:
                    if job["status"] == "running":
                        job["status"] = "failed"
                        job["error"] = str(e)[:2000]
            finally:
                with self._lock:
                    self._procs.pop(job_id, None)
                    job["finished_at"] = _now()
                    self._save(job)

    def _exec(self, job: Dict[str, Any], args: List[str]) -> str:
        d = self.jobs_dir / job["id"]
        with open(d / "log.txt", "a") as log:
            log.write("$ " + " ".join(args) + "\n")
            log.flush()
            proc = subprocess.Popen(args, cwd=d, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            with self._lock:
                self._procs[job["id"]] = proc
            try:
                out, _ = proc.communicate(timeout=self.cfg.job_timeout_s)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate()
                log.write(out or "")
                raise JobError(f"timed out after {self.cfg.job_timeout_s}s")
            log.write(out or "")
        if job["status"] == "cancelled":
            raise JobError("cancelled")
        if proc.returncode != 0:
            tail = (out or "").strip().splitlines()[-5:]
            raise JobError(f"{Path(args[0]).name} exited with code {proc.returncode}: " + " | ".join(tail))
        return out or ""

    @staticmethod
    def _feed_args(p: Dict[str, Any]) -> List[str]:
        args = ["--rate", repr(p["rate"]), "--vol", repr(p["vol"]),
                "--informed", repr(p["informed"]), "--impact", repr(p["impact"])]
        if p.get("seed") is not None:
            args += ["--seed", str(p["seed"])]
        return args

    def _run_simulation(self, job: Dict[str, Any]) -> Dict[str, Any]:
        p = job["params"]
        args = [str(self.cfg.binary("mds_sim")), "--events", str(p["events"]), "--queue", p["queue"],
                "--batch", str(p["batch"]), "--capacity", str(p["capacity"]), "--depth", "0",
                "--json", "summary.json", *self._feed_args(p)]
        if p["save_events"]:
            args += ["--bin", "ticks.bin"]
        if p["redis"]:
            args += ["--redis", self.cfg.redis, "--symbol", job["id"], "--purge"]
        self._exec(job, args)
        summary = json.loads((self.jobs_dir / job["id"] / "summary.json").read_text())
        pipes = summary.get("pipelines", [])
        best = max(pipes, key=lambda r: r["events_per_sec"]) if pipes else None
        headline = (f"{summary['speedup']:.2f}x lock-free speed-up" if summary.get("speedup")
                    else f"{best['events_per_sec'] / 1e6:.1f}M events/s" if best else "")
        return {**summary, "headline": headline,
                "redis_symbol": job["id"] if p["redis"] else None}

    def _run_backtest(self, job: Dict[str, Any]) -> Dict[str, Any]:
        p = job["params"]
        d = self.jobs_dir / job["id"]
        args = [str(self.cfg.binary("mds_backtest")), "--strategy", p["strategy"],
                "--latency-us", repr(p["latency_us"]), "--maker-bps", repr(p["maker_bps"]),
                "--taker-bps", repr(p["taker_bps"]),
                "--report", "report.json", "--equity", "equity.csv", "--fills", "fills.csv"]
        src = p["source"]
        if src.startswith("simulation:"):
            args += ["--input", str(self.jobs_dir / src.split(":", 1)[1] / "ticks.bin")]
        elif src.startswith("redis:"):
            args += ["--redis", self.cfg.redis, "--symbol", src.split(":", 1)[1]]
        else:
            args += ["--events", str(p["events"]), *self._feed_args(p)]
        self._exec(job, args)

        report = json.loads((d / "report.json").read_text())
        sys.path.insert(0, str(self.cfg.python_pkg_dir))
        from mds.analysis import load_equity, load_fills, markout  # noqa: E402

        curve = load_equity(str(d / "equity.csv"))
        fills = load_fills(str(d / "fills.csv"))
        t0 = curve[0].ts if curve else 0
        equity = [[round((pt.ts - t0) / 1e9, 3), round(pt.equity, 4), round(pt.mid, 4), pt.position]
                  for pt in _downsample(curve, 800)]
        volume = sum(f.qty for f in fills)
        m = report["metrics"]
        samples = m.get("equity_samples", 0)
        result = {
            **report,
            "equity": equity,
            "fills_recent": [[round((f.ts - t0) / 1e9, 3), f.side, f.price, f.qty, f.maker, round(f.fee, 4)]
                             for f in fills[-200:]],
            "fill_stats": {
                "count": len(fills),
                "volume": volume,
                "maker_ratio": (sum(f.qty for f in fills if f.maker) / volume) if volume else 0.0,
                "markout_1s": markout(fills, curve, 1.0),
                "markout_5s": markout(fills, curve, 5.0),
            },
            "sharpe_reliable": samples >= 250,
            "source": src,
        }
        result["headline"] = f"PnL {m['total_pnl']:+,.2f} over {m['fills']} fills"
        return result

    def _run_benchmark(self, job: Dict[str, Any]) -> Dict[str, Any]:
        p = job["params"]
        d = self.jobs_dir / job["id"]
        # The live market shares the CPU; pause it so the numbers are clean.
        if self._live_pause:
            self._live_pause(True)
        try:
            self._exec(job, [str(self.cfg.binary("bench_queue")), "--messages", str(p["messages"]),
                             "--events", str(p["events"]), "--reps", str(p["reps"]), "--json", "bench_queue.json"])
            self._exec(job, [str(self.cfg.binary("bench_orderbook")), "--events", str(p["events"]),
                             "--reps", str(p["reps"]), "--json", "bench_orderbook.json"])
        finally:
            if self._live_pause:
                self._live_pause(False)
        q = json.loads((d / "bench_queue.json").read_text())
        ob = json.loads((d / "bench_orderbook.json").read_text())
        raw1 = next((r for r in q["rows"] if r["variant"] == "spsc pop 1"), None)
        return {"queue": q, "orderbook": ob,
                "headline": f"{raw1['vs_mutex']:.1f}x raw lock-free speed-up" if raw1 else ""}

