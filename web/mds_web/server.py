"""HTTP server: JSON API, Server-Sent Events and the static frontend.

Standard library only (``http.server``), so the backend has no third-party
dependencies to install, audit or keep patched. One thread per connection
(``ThreadingHTTPServer``) is plenty for this workload: heavy work happens in
the C++ engines, and live updates are a few small messages per second.

API summary (all JSON):

  GET    /api/health                   liveness + readiness
  GET    /api/info                     version, limits, parameter schemas, status
  GET    /api/live                     live market status
  POST   /api/live/start   {params}    (re)start the live market
  POST   /api/live/stop
  GET    /api/live/stream              Server-Sent Events: hello/history/tick/status
  GET    /api/jobs[?kind=]             recent jobs
  POST   /api/jobs         {kind, params}
  GET    /api/jobs/<id>                job with full result
  DELETE /api/jobs/<id>                cancel (if active) or delete
  GET    /api/jobs/<id>/files/<name>   download an output file
"""
from __future__ import annotations

import hmac
import json
import mimetypes
import queue
import re
import signal
import sys
import tempfile
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Optional, Tuple
from urllib.parse import parse_qs, urlparse

from . import __version__
from .config import Config
from .jobs import DOWNLOADABLE, JobError, JobManager
from .live import LiveManager
from .validation import ValidationError, describe

MAX_BODY = 64 * 1024
JOB_ID = re.compile(r"^[0-9]{8}-[0-9]{6}-[0-9a-f]{6}$")
SECURITY_HEADERS = {
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
    "X-Frame-Options": "DENY",
    "Content-Security-Policy": "default-src 'self'; img-src 'self' data:; style-src 'self'; "
                               "script-src 'self'; connect-src 'self'; frame-ancestors 'none'",
}


def ensure_writable(path: Path) -> Path:
    """Use `path` for data if we can write there; otherwise fall back to a temp
    directory (e.g. a PaaS volume mounted root-owned while we run as non-root)."""
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / ".write-test"
        probe.write_text("ok")
        probe.unlink()
        return path
    except OSError as e:
        fallback = Path(tempfile.gettempdir()) / "mds-data"
        fallback.mkdir(parents=True, exist_ok=True)
        print(f"  warning: {path} is not writable ({e}); using {fallback} (not persistent)", flush=True)
        return fallback


class App:
    def __init__(self, cfg: Config) -> None:
        self.cfg = cfg
        cfg.data_dir = ensure_writable(cfg.data_dir)
        self.live = LiveManager(cfg)
        self.jobs = JobManager(cfg, live_pause=self.live.pause)
        self.started = time.time()

    def redis_status(self) -> Dict[str, Any]:
        if not self.cfg.redis:
            return {"configured": False}
        try:
            sys.path.insert(0, str(self.cfg.python_pkg_dir))
            from mds.resp import RedisClient  # noqa: E402

            with RedisClient.from_endpoint(self.cfg.redis, timeout=1.0) as r:
                ok = r.execute("PING") in ("PONG", b"PONG")
            return {"configured": True, "ok": ok}
        except Exception as e:  # noqa: BLE001
            return {"configured": True, "ok": False, "error": str(e)[:200]}

    def info(self) -> Dict[str, Any]:
        return {
            "version": __version__,
            "uptime_s": round(time.time() - self.started, 1),
            "auth_required": bool(self.cfg.api_token),
            "engines_missing": self.cfg.missing_binaries(),
            "limits": {"max_events": self.cfg.max_events, "workers": self.cfg.workers,
                       "max_queued": self.cfg.max_queued, "job_timeout_s": self.cfg.job_timeout_s},
            "schemas": {k: describe(v) for k, v in self.jobs.schemas.items()},
            "redis": self.redis_status(),
            "live": self.live.status(),
            "jobs": self.jobs.stats(),
        }


class Handler(BaseHTTPRequestHandler):
    server_version = "mds-web"
    protocol_version = "HTTP/1.1"
    app: App  # set on the class by make_server()

    # ------------------------------------------------------------- plumbing
    def log_message(self, fmt: str, *args: Any) -> None:  # quieter, one line per request
        if "/api/live/stream" in (args[0] if args else ""):
            return
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def _send(self, status: int, body: bytes, ctype: str, extra: Optional[Dict[str, str]] = None) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in {**SECURITY_HEADERS, **(extra or {})}.items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, status: int, data: Any) -> None:
        self._send(status, json.dumps(data, separators=(",", ":")).encode(), "application/json",
                   {"Cache-Control": "no-store"})

    def _error(self, status: int, message: str, details: Optional[Dict[str, str]] = None) -> None:
        body: Dict[str, Any] = {"error": message}
        if details:
            body["details"] = details
        self._json(status, body)

    def _body(self) -> Tuple[bool, Any]:
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_BODY:
            self._error(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "request body too large")
            return False, None
        raw = self.rfile.read(length) if length else b""
        if not raw:
            return True, {}
        try:
            return True, json.loads(raw)
        except json.JSONDecodeError:
            self._error(HTTPStatus.BAD_REQUEST, "body must be valid JSON")
            return False, None

    def _authorized(self) -> bool:
        token = self.app.cfg.api_token
        if not token:
            return True
        given = self.headers.get("X-API-Token") or ""
        auth = self.headers.get("Authorization") or ""
        if auth.lower().startswith("bearer "):
            given = auth[7:]
        if hmac.compare_digest(given.encode(), token.encode()):
            return True
        self._error(HTTPStatus.UNAUTHORIZED, "this server requires an access token for changes")
        return False

    # -------------------------------------------------------------- routing
    def do_HEAD(self) -> None:
        self.do_GET()

    def do_GET(self) -> None:
        url = urlparse(self.path)
        path = url.path
        try:
            if path == "/api/health":
                missing = self.app.cfg.missing_binaries()
                return self._json(HTTPStatus.OK if not missing else HTTPStatus.SERVICE_UNAVAILABLE,
                                  {"status": "ok" if not missing else "degraded", "missing": missing})
            if path == "/api/info":
                return self._json(HTTPStatus.OK, self.app.info())
            if path == "/api/live":
                return self._json(HTTPStatus.OK, self.app.live.status())
            if path == "/api/live/stream":
                return self._stream()
            if path == "/api/jobs":
                kind = parse_qs(url.query).get("kind", [None])[0]
                return self._json(HTTPStatus.OK, {"jobs": self.app.jobs.list(kind)})
            m = re.fullmatch(r"/api/jobs/([^/]+)", path)
            if m:
                job = self.app.jobs.get(m.group(1)) if JOB_ID.match(m.group(1)) else None
                return self._json(HTTPStatus.OK, job) if job else self._error(HTTPStatus.NOT_FOUND, "no such job")
            m = re.fullmatch(r"/api/jobs/([^/]+)/files/([^/]+)", path)
            if m:
                return self._download(m.group(1), m.group(2))
            if path.startswith("/api/"):
                return self._error(HTTPStatus.NOT_FOUND, "unknown endpoint")
            return self._static(path)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if not self._authorized():
            return
        ok, body = self._body()
        if not ok:
            return
        try:
            if path == "/api/jobs":
                if not isinstance(body, dict):
                    return self._error(HTTPStatus.BAD_REQUEST, "expected {kind, params}")
                job = self.app.jobs.submit(str(body.get("kind")), body.get("params"))
                return self._json(HTTPStatus.ACCEPTED, job)
            if path == "/api/live/start":
                return self._json(HTTPStatus.OK, self.app.live.start(body.get("params") if body else None))
            if path == "/api/live/stop":
                return self._json(HTTPStatus.OK, self.app.live.stop())
            return self._error(HTTPStatus.NOT_FOUND, "unknown endpoint")
        except ValidationError as e:
            return self._error(HTTPStatus.UNPROCESSABLE_ENTITY, "invalid parameters", e.errors)
        except (JobError, RuntimeError) as e:
            return self._error(HTTPStatus.CONFLICT, str(e))

    def do_DELETE(self) -> None:
        path = urlparse(self.path).path
        if not self._authorized():
            return
        m = re.fullmatch(r"/api/jobs/([^/]+)", path)
        if m and JOB_ID.match(m.group(1)) and self.app.jobs.delete(m.group(1)):
            return self._json(HTTPStatus.OK, {"deleted": m.group(1)})
        return self._error(HTTPStatus.NOT_FOUND, "no such job")

    # ------------------------------------------------------------ handlers
    def _download(self, job_id: str, name: str) -> None:
        p = self.app.jobs.file_path(job_id, name) if JOB_ID.match(job_id) else None
        if not p:
            return self._error(HTTPStatus.NOT_FOUND, "no such file")
        data = p.read_bytes()
        self._send(HTTPStatus.OK, data, DOWNLOADABLE[name],
                   {"Content-Disposition": f'attachment; filename="{job_id}-{name}"'})

    def _static(self, path: str) -> None:
        root = self.app.cfg.frontend_dir.resolve()
        rel = path.lstrip("/") or "index.html"
        target = (root / rel).resolve()
        if root not in target.parents and target != root:
            return self._error(HTTPStatus.NOT_FOUND, "not found")
        if not target.is_file():
            # Single-page app: unknown non-file routes fall back to index.html.
            if "." in Path(rel).name:
                return self._error(HTTPStatus.NOT_FOUND, "not found")
            target = root / "index.html"
        ctype = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "image/svg+xml"):
            ctype += "; charset=utf-8"
        # Always revalidate (cheap 304s via ETag) so a deploy is picked up on
        # the next page load instead of browsers running stale JavaScript.
        st = target.stat()
        etag = f'"{st.st_mtime_ns:x}-{st.st_size:x}"'
        headers = {"Cache-Control": "no-cache", "ETag": etag}
        if self.headers.get("If-None-Match") == etag:
            self.send_response(HTTPStatus.NOT_MODIFIED)
            for k, v in headers.items():
                self.send_header(k, v)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self._send(HTTPStatus.OK, target.read_bytes(), ctype, headers)

    def _stream(self) -> None:
        try:
            q = self.app.live.subscribe()
        except RuntimeError as e:
            return self._error(HTTPStatus.SERVICE_UNAVAILABLE, str(e))
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")  # disable proxy buffering (nginx)
        self.send_header("Connection", "keep-alive")
        for k, v in SECURITY_HEADERS.items():
            self.send_header(k, v)
        self.end_headers()
        self.close_connection = True
        try:
            self.wfile.write(b"retry: 2000\n\n")
            while True:
                try:
                    msg = q.get(timeout=15)
                    self.wfile.write(msg.encode())
                except queue.Empty:
                    self.wfile.write(b": keep-alive\n\n")  # defeat idle proxy timeouts
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.app.live.unsubscribe(q)


def make_server(cfg: Config) -> Tuple[ThreadingHTTPServer, App]:
    app = App(cfg)
    handler = type("BoundHandler", (Handler,), {"app": app})
    httpd = ThreadingHTTPServer((cfg.host, cfg.port), handler)
    httpd.daemon_threads = True
    return httpd, app


def main() -> None:
    cfg = Config()
    mimetypes.add_type("application/javascript", ".js")
    mimetypes.add_type("image/svg+xml", ".svg")
    httpd, app = make_server(cfg)
    missing = cfg.missing_binaries()
    print(f"mds-web {__version__} on http://{cfg.host}:{cfg.port}", flush=True)
    print(f"  engines: {cfg.bin_dir}" + (f"  (MISSING: {', '.join(missing)})" if missing else ""), flush=True)
    print(f"  data:    {cfg.data_dir}", flush=True)
    print(f"  redis:   {cfg.redis or 'not configured'}   auth: {'token' if cfg.api_token else 'open'}", flush=True)
    if cfg.live_autostart and not missing:
        try:
            app.live.start()
        except RuntimeError as e:
            print(f"  live market not started: {e}", flush=True)

    def shutdown(*_: Any) -> None:
        app.live.shutdown()
        app.jobs.shutdown()
        threading.Thread(target=httpd.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    try:
        httpd.serve_forever()
    finally:
        app.live.shutdown()
        httpd.server_close()


if __name__ == "__main__":
    main()
