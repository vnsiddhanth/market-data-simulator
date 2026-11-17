# Deploying the web app

The web app is **one Docker image**. It contains the compiled C++ engines, a
dependency-free Python backend and the static frontend, so anything that
runs a container can host it. Redis is optional.

- [1. Run locally without Docker](#1-run-locally-without-docker)
- [2. Run with Docker](#2-run-with-docker)
- [3. Full stack with Redis (Docker Compose)](#3-full-stack-with-redis-docker-compose)
- [4. Deploy to the cloud](#4-deploy-to-the-cloud)
- [5. Configuration reference](#5-configuration-reference)
- [6. Security](#6-security)
- [7. Operations](#7-operations)

---

## 1. Run locally without Docker

Build the engines (see [GETTING_STARTED.md](GETTING_STARTED.md#6-install-and-build)), then:

```bash
cd web && python3 -m mds_web
```

Open http://127.0.0.1:8080. The server finds the engines in `build/`
automatically. Set `MDS_BIN_DIR` if you built them elsewhere, and
`MDS_HOST=0.0.0.0` to accept connections from other machines.

## 2. Run with Docker

```bash
docker build -t mds-web .
```

```bash
docker run --rm -p 8080:8080 -v mds-data:/data mds-web
```

Open http://localhost:8080. The image build compiles the engines and runs
the C++ test suite, so a broken build never produces an image. The
container runs as a non-root user and has a built-in health check
(`/api/health`).

To require a token for changes (recommended on any public server):

```bash
docker run -d -p 8080:8080 -v mds-data:/data -e MDS_API_TOKEN=choose-a-long-secret --restart unless-stopped mds-web
```

## 3. Full stack with Redis (Docker Compose)

```bash
docker compose up -d --build
```

This starts the web app plus Redis Stack (with the RedisTimeSeries module).
Simulations get a **Store in Redis** option and backtests can read from
Redis. Data persists in named volumes across restarts. Set a token like
this:

```bash
MDS_API_TOKEN=choose-a-long-secret docker compose up -d --build
```

## 4. Deploy to the cloud

The app needs **one always-on instance** (the live market runs
continuously) with ideally **2 vCPUs** (the engine uses two threads) and
512 MB-1 GB of RAM. Scale-to-zero platforms will stop the live market when
idle.

### Render

1. Push the repository to GitHub (already done for this project).
2. In Render choose **New → Blueprint** and select the repository. Render reads [`render.yaml`](../render.yaml).
3. Deploy. Render generates `MDS_API_TOKEN`; find it under the service's **Environment** tab and give it to people who should be able to start runs.

### Fly.io

```bash
fly launch --copy-config --no-deploy
```

```bash
fly volumes create mds_data --size 1
```

```bash
fly secrets set MDS_API_TOKEN=choose-a-long-secret
```

```bash
fly deploy
```

[`fly.toml`](../fly.toml) keeps one machine running and checks `/api/health`.

### Railway / Google Cloud Run / Azure Container Apps / any container host

Point the platform at the repository's `Dockerfile`. The app listens on
`$PORT` when the platform provides one (otherwise 8080). Add a volume at
`/data` if you want run history to survive restarts. On Cloud Run, set
**minimum instances = 1** and **CPU always allocated** so the live market
keeps running.

### A plain VM (with HTTPS)

Run the container (or Compose) on the VM and put a reverse proxy in front
for TLS. With Caddy, which handles certificates automatically:

```text
simulator.example.com {
    reverse_proxy 127.0.0.1:8080
}
```

The live page uses Server-Sent Events. Caddy streams them correctly out of
the box. With nginx, disable buffering for the stream:

```nginx
location /api/live/stream {
    proxy_pass http://127.0.0.1:8080;
    proxy_buffering off;
    proxy_read_timeout 1h;
}
location / { proxy_pass http://127.0.0.1:8080; }
```

## 5. Configuration reference

All settings are environment variables.

| Variable | Default | Meaning |
|---|---|---|
| `MDS_HOST` | `127.0.0.1` (`0.0.0.0` in Docker) | Interface to listen on |
| `MDS_PORT` / `PORT` | `8080` | Port (`MDS_PORT` wins over `PORT`) |
| `MDS_API_TOKEN` | *(empty)* | If set, starting/cancelling runs and controlling the live market require `Authorization: Bearer <token>`. Viewing stays public |
| `MDS_REDIS` | *(empty)* | `host:port` of a Redis server; enables Redis storage options |
| `MDS_BIN_DIR` | auto (`build/`) | Directory containing the C++ engines |
| `MDS_DATA_DIR` | `web/data` (`/data` in Docker) | Where run results are stored |
| `MDS_MAX_EVENTS` | `3000000` | Largest simulation/backtest a user may request |
| `MDS_MAX_BENCH_MESSAGES` | `20000000` | Largest raw-queue benchmark |
| `MDS_WORKERS` | `1` | Runs executed in parallel. Keep 1 for clean benchmark numbers |
| `MDS_MAX_QUEUED` | `20` | Queued + running jobs allowed at once |
| `MDS_JOB_TIMEOUT_S` | `600` | A run is killed after this many seconds |
| `MDS_KEEP_JOBS` | `100` | Finished runs kept before the oldest are deleted |
| `MDS_LIVE_AUTOSTART` | `1` | Start the live market when the server starts |
| `MDS_LIVE_MAX_CLIENTS` | `200` | Concurrent live viewers |

## 6. Security

What the server does by default:

- **Input validation.** Every parameter is checked against a schema
  (type, range, allowed values). Unknown parameters are rejected.
- **No shell.** Engines are started with argument lists built from
  validated values, so user input cannot inject commands or flags.
- **Bounded work.** Limits on events per run, queued jobs, job runtime,
  retained results, request body size (64 KB) and live viewers.
- **Path safety.** Static files are served only from the frontend folder,
  downloads only for a fixed set of file names, and job ids must match a
  strict pattern.
- **Browser hardening.** A strict Content-Security-Policy (no inline
  scripts or styles, same-origin only), `nosniff`, no framing and no
  referrer. The frontend never inserts server data as HTML.
- **Optional write protection.** With `MDS_API_TOKEN` set, all changes
  require the token (compared in constant time). Read-only viewing stays
  open.
- **Least privilege.** The container runs as an unprivileged user.

For a public deployment, set `MDS_API_TOKEN`, serve over HTTPS (any PaaS
or a reverse proxy does this), and keep `MDS_MAX_EVENTS` modest if the
machine is small.

## 7. Operations

| Task | How |
|---|---|
| Health check | `GET /api/health` → `200 {"status":"ok"}`, or `503` if engines are missing |
| Status and limits | `GET /api/info` |
| Logs | stdout/stderr (`docker logs <container>`); one line per request (the live stream is not logged) |
| Per-run logs | the **log.txt** download on each run, or `<data>/jobs/<id>/log.txt` |
| Backups | copy the `/data` volume; each run is a self-contained folder |
| Upgrades | rebuild and restart the container; finished runs are kept, runs in progress are marked "interrupted" |
| Live market crashed | it restarts automatically with back-off; the error is shown on the Live page |
| Clean benchmark numbers | the live market pauses automatically during benchmarks; keep `MDS_WORKERS=1` |
