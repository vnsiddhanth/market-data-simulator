# Production image for the Market Data Simulator web app.
#
#   docker build -t mds-web .
#   docker run --rm -p 8080:8080 mds-web          ->  http://localhost:8080
#
# Stage 1 compiles the C++ engines; stage 2 is a slim Python runtime with the
# engines, the stdlib-only backend and the static frontend. No pip installs.

# ---------------------------------------------------------------- build ----
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake ninja-build \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY apps apps
COPY bench bench
COPY tests tests
# Static libstdc++/libgcc so the binaries run on any glibc-based runtime image.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
 && cmake --build build \
 && ctest --test-dir build --output-on-failure \
 && mkdir -p /out \
 && cp build/mds_sim build/mds_backtest build/mds_query build/mds_live \
       build/mds_itch build/mds_t2t build/mds_feed \
       build/bench_queue build/bench_orderbook /out/

# -------------------------------------------------------------- runtime ----
FROM python:3.12-slim
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    MDS_HOST=0.0.0.0 \
    MDS_PORT=8080 \
    MDS_BIN_DIR=/app/bin \
    MDS_DATA_DIR=/data \
    MDS_FRONTEND_DIR=/app/web/frontend \
    MDS_PYTHON_DIR=/app/python
RUN useradd --create-home --uid 10001 mds \
 && mkdir -p /data && chown mds:mds /data
WORKDIR /app
COPY --from=build /out/ /app/bin/
COPY python/mds /app/python/mds
COPY web/mds_web /app/web/mds_web
COPY web/frontend /app/web/frontend
USER mds
VOLUME ["/data"]
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD python -c "import os,urllib.request; urllib.request.urlopen('http://127.0.0.1:%s/api/health' % (os.environ.get('MDS_PORT') or os.environ.get('PORT') or '8080'), timeout=2)"
WORKDIR /app/web
CMD ["python", "-m", "mds_web"]
