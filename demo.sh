#!/usr/bin/env bash
# End-to-end demo: simulate -> pipeline -> Redis -> query -> backtest -> analyse.
#
#   ./demo.sh                 # uses ./build, starts the in-repo fake Redis on :6399
#   BUILD=~/mds-build ./demo.sh
#   REDIS=127.0.0.1:6379 ./demo.sh   # use a real Redis Stack instead (docker compose)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$ROOT/build}"
REDIS="${REDIS:-}"
OUT="${OUT:-$ROOT/out}"
EVENTS="${EVENTS:-1000000}"
mkdir -p "$OUT"

bold() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
pause() { sleep "${PAUSE:-2}"; }

if [ ! -x "$BUILD/mds_sim" ]; then
    bold "Building into $BUILD"
    cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" --parallel
fi

FAKE_PID=""
cleanup() { [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2>/dev/null || true; }
trap cleanup EXIT
if [ -z "$REDIS" ]; then
    REDIS=127.0.0.1:6399
    bold "Starting the in-repo fake Redis (RedisTimeSeries semantics) on $REDIS"
    python3 "$ROOT/python/scripts/fake_redis.py" --port 6399 &
    FAKE_PID=$!
    sleep 1
fi

bold "1/6  Generate $EVENTS events, run them through the lock-free AND mutex pipelines"
"$BUILD/mds_sim" --events "$EVENTS" --queue both --depth 8 --pin 2,4 --bin "$OUT/ticks.bin"
pause

bold "2/6  Persist to Redis: time-series + binary event chunks (pipelined)"
"$BUILD/mds_sim" --events 300000 --queue spsc --depth 0 --redis "$REDIS" --symbol DEMO --purge | grep -E 'generate|redis'
pause

bold "3/6  Query Redis: 10-second average mid, server-side aggregation"
"$BUILD/mds_query" --redis "$REDIS" --symbol DEMO --field mid --agg avg --bucket-ms 10000 --limit 10
pause

bold "4/6  Inspect Redis from Python: OHLC bars + event-log verification"
python3 "$ROOT/python/scripts/redis_inspect.py" --redis "$REDIS" --symbol DEMO --bars 10000 --rows 6
pause

bold "5/6  Backtest both strategies on the events stored in Redis"
"$BUILD/mds_backtest" --strategy mm --redis "$REDIS" --symbol DEMO \
    --report "$OUT/mm.json" --equity "$OUT/mm_equity.csv" --fills "$OUT/mm_fills.csv"
echo
"$BUILD/mds_backtest" --strategy obi --redis "$REDIS" --symbol DEMO \
    --report "$OUT/obi.json" --equity "$OUT/obi_equity.csv" --fills "$OUT/obi_fills.csv"
pause

bold "6/6  Independent Python cross-check of the market maker's metrics"
python3 "$ROOT/python/scripts/analyze_backtest.py" --equity "$OUT/mm_equity.csv" \
    --fills "$OUT/mm_fills.csv" --report "$OUT/mm.json" --plot "$OUT/mm.png"

bold "Done. Outputs in $OUT"
ls -lh "$OUT"
