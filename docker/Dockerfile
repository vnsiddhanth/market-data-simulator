# Reproducible Linux build + test image.
#   docker build -f docker/Dockerfile -t mds .
#   docker run --rm mds bench_queue --messages 10000000
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build python3 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build \
 && ctest --test-dir build --output-on-failure \
 && (cd python && python3 -m unittest discover -s tests)

FROM ubuntu:24.04
COPY --from=build /src/build/mds_sim /src/build/mds_backtest /src/build/mds_query \
                  /src/build/bench_queue /src/build/bench_orderbook /usr/local/bin/
ENTRYPOINT []
CMD ["mds_sim", "--events", "1000000"]
