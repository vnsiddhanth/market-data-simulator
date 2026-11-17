# Building

## Requirements

| Tool | Version |
|---|---|
| C++ compiler | GCC ≥ 9, Clang ≥ 10, MSVC 2019 16.8+ (C++17, 64-bit lock-free atomics) |
| CMake | ≥ 3.16 |
| Python (optional) | ≥ 3.9, stdlib only; `pandas` / `matplotlib` optional |
| Redis (optional) | Redis Stack 7.x for RedisTimeSeries, or any Redis for the fallback |

## Linux / macOS

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build --parallel
```

```bash
ctest --test-dir build --output-on-failure
```

## Windows (Visual Studio)

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

```bash
cmake --build build --config Release --parallel
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

Binaries land in `build/Release/`. Winsock (`ws2_32`) is linked automatically.

## Docker

```bash
docker build -f docker/Dockerfile -t mds .
```

```bash
docker run --rm mds bench_queue --messages 10000000
```

The image build runs the full C++ and Python test suites.

## CMake options

| Option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | `-O3` for GCC/Clang, `/O2 /Ob3` for MSVC |
| `MDS_BUILD_TESTS` | `ON` | builds `mds_tests`, registers one CTest per suite |
| `MDS_BUILD_BENCH` | `ON` | builds `bench_queue`, `bench_orderbook` |
| `MDS_NATIVE` | `OFF` | `-march=native` |
| `MDS_WERROR` | `OFF` | warnings as errors |
| `MDS_SANITIZE` | empty | `address`, `undefined` or `thread` |

## Sanitizers

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ -DMDS_SANITIZE=thread
```

```bash
cmake --build build-tsan --parallel && ctest --test-dir build-tsan --output-on-failure
```

ThreadSanitizer validates the SPSC ring and pipeline; AddressSanitizer covers
the book's iterator/reference handling and the RESP parser.

## Running a subset of tests

```bash
./build/mds_tests order_book
```

```bash
./build/mds_tests spsc_ring.concurrent
```

The filter is a substring match on `suite.name`.

## Python

```bash
cd python && python -m unittest discover -s tests -v
```

Optional extras for plotting / DataFrames:

```bash
pip install -r python/requirements.txt
```

## Continuous integration

`.github/workflows/ci.yml` runs:

* GCC and Clang on Ubuntu, Apple Clang on macOS, MSVC on Windows;
* ASan and TSan builds;
* smoke runs of every binary;
* a Redis integration job against **Redis Stack** (RedisTimeSeries) and
  **vanilla Redis** (sorted-set fallback) service containers;
* the Python suite, plus the C++ binaries against the in-repo fake Redis.
