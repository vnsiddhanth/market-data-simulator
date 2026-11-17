cmake_minimum_required(VERSION 3.16)
project(market_data_simulator
        VERSION 1.0.0
        DESCRIPTION "Limit order book simulator, lock-free market-data pipeline and backtester"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MDS_BUILD_TESTS  "Build the unit tests"                         ON)
option(MDS_BUILD_BENCH  "Build the benchmarks"                         ON)
option(MDS_NATIVE       "Optimise for the build machine (-march=native)" OFF)
option(MDS_WERROR       "Treat warnings as errors"                     OFF)
set(MDS_SANITIZE "" CACHE STRING "Sanitizer: '', address, undefined, thread")

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warnings / optimisation flags shared by every target
# ---------------------------------------------------------------------------
add_library(mds_options INTERFACE)
if(MSVC)
  target_compile_options(mds_options INTERFACE /W4 /permissive- /Zc:__cplusplus
                         $<$<CONFIG:Release>:/O2 /Ob3>)
  target_compile_definitions(mds_options INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  if(MDS_WERROR)
    target_compile_options(mds_options INTERFACE /WX)
  endif()
else()
  target_compile_options(mds_options INTERFACE
      -Wall -Wextra -Wpedantic
      $<$<CONFIG:Release>:-O3>)
  if(MDS_WERROR)
    target_compile_options(mds_options INTERFACE -Werror)
  endif()
  if(MDS_NATIVE)
    target_compile_options(mds_options INTERFACE -march=native)
  endif()
  if(MDS_SANITIZE)
    target_compile_options(mds_options INTERFACE -fsanitize=${MDS_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(mds_options INTERFACE -fsanitize=${MDS_SANITIZE})
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(mds_core STATIC
    src/order_book.cpp
    src/synthetic_feed.cpp
    src/backtester.cpp
    src/metrics.cpp
    src/strategies.cpp
    src/resp.cpp
    src/redis_client.cpp
    src/tick_store.cpp)
target_include_directories(mds_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mds_core PUBLIC Threads::Threads PRIVATE mds_options)
if(WIN32)
  target_link_libraries(mds_core PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
foreach(app mds_sim mds_backtest mds_query mds_live)
  add_executable(${app} apps/${app}.cpp)
  target_link_libraries(${app} PRIVATE mds_core mds_options)
endforeach()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(MDS_BUILD_BENCH)
  foreach(b bench_queue bench_orderbook)
    add_executable(${b} bench/${b}.cpp)
    target_link_libraries(${b} PRIVATE mds_core mds_options)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MDS_BUILD_TESTS)
  enable_testing()
  add_executable(mds_tests
      tests/test_main.cpp
      tests/test_order_book.cpp
      tests/test_pipeline.cpp
      tests/test_feed_storage.cpp
      tests/test_backtest.cpp)
  target_link_libraries(mds_tests PRIVATE mds_core mds_options)
  foreach(suite order_book spsc_ring mutex_queue pipeline feed resp tick_store portfolio metrics backtester)
    add_test(NAME ${suite} COMMAND mds_tests ${suite}.)
  endforeach()
endif()
