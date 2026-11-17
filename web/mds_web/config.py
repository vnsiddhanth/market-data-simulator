"""Runtime configuration, read once from environment variables.

Every setting has a safe default so ``python -m mds_web`` works out of the box
from a source checkout; the Docker image overrides paths.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parents[2]


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except ValueError:
        return default


def _env_bool(name: str, default: bool) -> bool:
    v = os.environ.get(name)
    if v is None:
        return default
    return v.strip().lower() in ("1", "true", "yes", "on")


def _default_bin_dir() -> Path:
    for cand in (REPO_ROOT / "build", REPO_ROOT / "build" / "Release", Path.home() / "mds-build"):
        if (cand / "mds_sim").exists() or (cand / "mds_sim.exe").exists():
            return cand
    return REPO_ROOT / "build"


@dataclass
class Config:
    host: str = field(default_factory=lambda: os.environ.get("MDS_HOST", "127.0.0.1"))
    # MDS_PORT wins; PORT is what most PaaS hosts (Render, Railway, Fly, Heroku) inject.
    port: int = field(default_factory=lambda: _env_int("MDS_PORT", _env_int("PORT", 8080)))
    bin_dir: Path = field(default_factory=lambda: Path(os.environ.get("MDS_BIN_DIR", "") or _default_bin_dir()))
    data_dir: Path = field(default_factory=lambda: Path(os.environ.get("MDS_DATA_DIR", str(REPO_ROOT / "web" / "data"))))
    frontend_dir: Path = field(default_factory=lambda: Path(os.environ.get("MDS_FRONTEND_DIR", str(REPO_ROOT / "web" / "frontend"))))
    python_pkg_dir: Path = field(default_factory=lambda: Path(os.environ.get("MDS_PYTHON_DIR", str(REPO_ROOT / "python"))))
    redis: Optional[str] = field(default_factory=lambda: os.environ.get("MDS_REDIS") or None)
    # When set, every state-changing request must carry this token.
    api_token: Optional[str] = field(default_factory=lambda: os.environ.get("MDS_API_TOKEN") or None)

    max_events: int = field(default_factory=lambda: _env_int("MDS_MAX_EVENTS", 3_000_000))
    max_bench_messages: int = field(default_factory=lambda: _env_int("MDS_MAX_BENCH_MESSAGES", 20_000_000))
    workers: int = field(default_factory=lambda: _env_int("MDS_WORKERS", 1))
    max_queued: int = field(default_factory=lambda: _env_int("MDS_MAX_QUEUED", 20))
    job_timeout_s: int = field(default_factory=lambda: _env_int("MDS_JOB_TIMEOUT_S", 600))
    keep_jobs: int = field(default_factory=lambda: _env_int("MDS_KEEP_JOBS", 100))

    live_autostart: bool = field(default_factory=lambda: _env_bool("MDS_LIVE_AUTOSTART", True))
    live_max_clients: int = field(default_factory=lambda: _env_int("MDS_LIVE_MAX_CLIENTS", 200))

    def binary(self, name: str) -> Path:
        p = self.bin_dir / name
        if not p.exists() and (self.bin_dir / (name + ".exe")).exists():
            return self.bin_dir / (name + ".exe")
        return p

    def missing_binaries(self) -> list:
        names = ("mds_sim", "mds_backtest", "mds_live", "bench_queue", "bench_orderbook")
        return [n for n in names if not self.binary(n).exists()]
