"""End-to-end tests for the web backend.

Starts the real HTTP server in-process on a free port. Tests that need the
C++ engines are skipped when they are not built (set MDS_BIN_DIR to point at
them).

    cd web && python -m unittest discover -s tests -v
"""
from __future__ import annotations

import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mds_web.config import Config  # noqa: E402
from mds_web.server import make_server  # noqa: E402


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, **overrides):
        self.tmp = tempfile.TemporaryDirectory()
        cfg = Config()
        cfg.host, cfg.port = "127.0.0.1", free_port()
        cfg.data_dir = Path(self.tmp.name)
        cfg.live_autostart = False
        for k, v in overrides.items():
            setattr(cfg, k, v)
        self.cfg = cfg
        self.httpd, self.app = make_server(cfg)
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{cfg.port}"

    def close(self):
        self.app.live.shutdown()
        self.app.jobs.shutdown()
        self.httpd.shutdown()
        self.httpd.server_close()
        self.tmp.cleanup()

    def request(self, method, path, body=None, headers=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=headers or {})
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                raw = r.read()
                return r.status, dict(r.headers), raw
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def json(self, method, path, body=None, headers=None):
        status, _, raw = self.request(method, path, body, headers)
        return status, json.loads(raw) if raw else None

    def wait_job(self, job_id, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            _, job = self.json("GET", f"/api/jobs/{job_id}")
            if job["status"] not in ("queued", "running"):
                return job
            time.sleep(0.3)
        raise AssertionError("job did not finish")


ENGINES = not Config().missing_binaries()


class BasicTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = Server()

    @classmethod
    def tearDownClass(cls):
        cls.s.close()

    def test_info_and_schemas(self):
        status, info = self.s.json("GET", "/api/info")
        self.assertEqual(status, 200)
        self.assertIn("simulation", info["schemas"])
        self.assertEqual(info["schemas"]["backtest"]["strategy"]["choices"], ["mm", "obi"])
        self.assertFalse(info["auth_required"])

    def test_validation_errors_are_per_field(self):
        status, body = self.s.json("POST", "/api/jobs", {"kind": "simulation", "params": {"events": -1, "nope": 1, "queue": "x"}})
        self.assertEqual(status, 422)
        self.assertEqual(set(body["details"]), {"events", "nope", "queue"})

    def test_rejects_non_numeric_and_bool_numbers(self):
        status, body = self.s.json("POST", "/api/jobs", {"kind": "backtest", "params": {"latency_us": "fast", "events": True}})
        self.assertEqual(status, 422)
        self.assertIn("latency_us", body["details"])
        self.assertIn("events", body["details"])

    def test_unknown_kind_and_bad_json(self):
        self.assertEqual(self.s.json("POST", "/api/jobs", {"kind": "rm -rf"})[0], 409)
        req = urllib.request.Request(self.s.base + "/api/jobs", data=b"{not json", method="POST")
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req)
        self.assertEqual(cm.exception.code, 400)

    def test_static_spa_and_security_headers(self):
        status, headers, body = self.s.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b"Market Data Simulator", body)
        self.assertIn("default-src 'self'", headers["Content-Security-Policy"])
        self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
        self.assertEqual(self.s.request("GET", "/some/client/route")[0], 200)   # SPA fallback
        self.assertEqual(self.s.request("GET", "/missing.js")[0], 404)

    def test_etag_revalidation(self):
        _, headers, _ = self.s.request("GET", "/app.js")
        status, _, body = self.s.request("GET", "/app.js", headers={"If-None-Match": headers["ETag"]})
        self.assertEqual(status, 304)
        self.assertEqual(body, b"")

    def test_path_traversal_blocked(self):
        for path in ("/../mds_web/server.py", "/%2e%2e/mds_web/config.py", "/api/jobs/../../etc/passwd"):
            self.assertIn(self.s.request("GET", path)[0], (400, 404))

    def test_job_ids_are_validated(self):
        self.assertEqual(self.s.json("GET", "/api/jobs/../../x")[0], 404)
        self.assertEqual(self.s.request("GET", "/api/jobs/20260101-000000-abcdef/files/../../job.json")[0], 404)


class AuthTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = Server(api_token="s3cret")

    @classmethod
    def tearDownClass(cls):
        cls.s.close()

    def test_reads_open_writes_need_token(self):
        self.assertEqual(self.s.json("GET", "/api/info")[0], 200)
        self.assertEqual(self.s.json("POST", "/api/live/stop")[0], 401)
        self.assertEqual(self.s.json("POST", "/api/live/stop", headers={"Authorization": "Bearer wrong"})[0], 401)
        self.assertEqual(self.s.json("POST", "/api/live/stop", headers={"Authorization": "Bearer s3cret"})[0], 200)
        self.assertEqual(self.s.json("DELETE", "/api/jobs/20260101-000000-abcdef")[0], 401)


@unittest.skipUnless(ENGINES, "C++ engines not built (set MDS_BIN_DIR)")
class EngineTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.s = Server()

    @classmethod
    def tearDownClass(cls):
        cls.s.close()

    def test_simulation_then_backtest_on_its_data(self):
        status, job = self.s.json("POST", "/api/jobs", {"kind": "simulation", "params": {"events": 50_000}})
        self.assertEqual(status, 202)
        self.assertIsNotNone(job["params"]["seed"])  # random seed is recorded
        sim = self.s.wait_job(job["id"])
        self.assertEqual(sim["status"], "succeeded", sim.get("error"))
        r = sim["result"]
        self.assertEqual({p["queue"] for p in r["pipelines"]}, {"spsc-lockfree", "mutex"})
        self.assertTrue(all(p["consistent"] for p in r["pipelines"]))
        self.assertIn("ticks.bin", sim["files"])

        status, bt = self.s.json("POST", "/api/jobs", {"kind": "backtest", "params": {"strategy": "mm", "source": f"simulation:{sim['id']}"}})
        self.assertEqual(status, 202)
        bt = self.s.wait_job(bt["id"])
        self.assertEqual(bt["status"], "succeeded", bt.get("error"))
        res = bt["result"]
        self.assertEqual(res["events"], 50_000)
        self.assertGreater(len(res["equity"]), 2)
        self.assertIn("markout_1s", res["fill_stats"])

        status, headers, body = self.s.request("GET", f"/api/jobs/{bt['id']}/files/fills.csv")
        self.assertEqual(status, 200)
        self.assertTrue(body.startswith(b"ts,order_id,side"))
        self.assertIn("attachment", headers["Content-Disposition"])

    def test_bad_source_rejected(self):
        status, body = self.s.json("POST", "/api/jobs", {"kind": "backtest", "params": {"source": "simulation:nope"}})
        self.assertEqual(status, 409)

    def test_cancel_and_delete(self):
        _, job = self.s.json("POST", "/api/jobs", {"kind": "simulation", "params": {"events": 3_000_000}})
        time.sleep(0.5)
        self.assertEqual(self.s.json("DELETE", f"/api/jobs/{job['id']}")[0], 200)
        self.assertEqual(self.s.wait_job(job["id"])["status"], "cancelled")
        self.assertEqual(self.s.json("DELETE", f"/api/jobs/{job['id']}")[0], 200)
        self.assertEqual(self.s.json("GET", f"/api/jobs/{job['id']}")[0], 404)

    def test_live_stream_delivers_ticks(self):
        status, st = self.s.json("POST", "/api/live/start", {"params": {"snapshot_ms": 100, "strategy": "mm"}})
        self.assertEqual(status, 200)
        events = []
        with urllib.request.urlopen(self.s.base + "/api/live/stream", timeout=10) as r:
            self.assertEqual(r.headers["Content-Type"], "text/event-stream")
            deadline = time.time() + 6
            while time.time() < deadline and events.count("tick") < 3:
                line = r.readline().decode()
                if line.startswith("event: "):
                    events.append(line[7:].strip())
                if line.startswith("data: ") and events and events[-1] == "tick":
                    tick = json.loads(line[6:])
        self.assertIn("hello", events)
        self.assertGreaterEqual(events.count("tick"), 3)
        self.assertIn("bids", tick["book"])
        self.assertIn("p50", tick["latency_us"])
        self.assertEqual(self.s.json("POST", "/api/live/stop")[1]["state"], "stopped")

    def test_live_params_validated(self):
        status, body = self.s.json("POST", "/api/live/start", {"params": {"speed": 1e9}})
        self.assertEqual(status, 422)
        self.assertIn("speed", body["details"])


if __name__ == "__main__":
    unittest.main()
