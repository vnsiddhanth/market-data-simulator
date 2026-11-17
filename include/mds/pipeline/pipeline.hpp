// SPDX-License-Identifier: MIT
//
// Two-stage market-data pipeline:
//
//   [feed thread] --MarketEvent--> Queue --> [book thread] --> OrderBook --> handler
//
// The pipeline is templated on the queue type, so the very same producer and
// consumer loops run on top of SpscRing (lock-free) or MutexQueue (baseline).
// This keeps the comparison honest: the only variable is the queue.
//
// The producer stamps every `latency_sample_every`-th envelope with a
// monotonic timestamp; the consumer computes enqueue -> book-applied latency
// for those samples. Stamping every message would put two clock reads on the
// hot path and distort the very throughput we are trying to measure.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "mds/common/affinity.hpp"
#include "mds/common/clock.hpp"
#include "mds/common/event.hpp"
#include "mds/common/stats.hpp"
#include "mds/orderbook/order_book.hpp"

namespace mds {

struct Envelope {
    MarketEvent event{};
    std::int64_t enqueue_ns{0};  // 0 == not sampled
};

struct PipelineConfig {
    std::size_t queue_capacity{1 << 16};
    std::size_t consumer_batch{64};  // max events drained per try_pop_n
    std::size_t latency_sample_every{64};
    int producer_cpu{-1};  // -1 == do not pin
    int consumer_cpu{-1};
};

struct PipelineResult {
    std::uint64_t events{0};
    std::uint64_t fills{0};
    double seconds{0.0};
    double events_per_sec{0.0};
    Percentiles latency_ns{};
    std::uint64_t producer_stalls{0};  // push attempts that found the queue full
    std::uint64_t consumer_idle{0};    // pop attempts that found the queue empty
    const char* queue_name{""};
};

// Called on the consumer thread after each event is applied to the book.
using BookUpdateHandler =
    std::function<void(const MarketEvent&, const std::vector<Fill>&, const OrderBook&)>;

template <class Queue>
class MarketDataPipeline {
public:
    explicit MarketDataPipeline(PipelineConfig cfg = {}) : cfg_(cfg), queue_(cfg.queue_capacity) {
        if (cfg_.consumer_batch == 0) cfg_.consumer_batch = 1;
        if (cfg_.latency_sample_every == 0) cfg_.latency_sample_every = 64;
    }

    // `source(MarketEvent&) -> bool` is invoked on the producer thread until it
    // returns false. This lets the feed generate events on the fly (as a real
    // feed handler would) instead of replaying a pre-built vector.
    template <class Source>
    PipelineResult run(Source&& source, OrderBook& book, const BookUpdateHandler& handler = {}) {
        std::atomic<bool> producer_done{false};
        std::atomic<bool> start{false};
        std::uint64_t producer_stalls = 0;

        std::thread producer([&] {
            pin_current_thread(cfg_.producer_cpu);
            while (!start.load(std::memory_order_acquire)) cpu_relax();
            Envelope env;
            Backoff backoff;
            std::uint64_t n = 0;
            while (source(env.event)) {
                env.enqueue_ns = (n % cfg_.latency_sample_every == 0) ? mono_ns() : 0;
                while (!queue_.try_push(env)) {
                    ++producer_stalls;
                    backoff.pause();
                }
                backoff.reset();
                ++n;
            }
            producer_done.store(true, std::memory_order_release);
        });

        PipelineResult result;
        std::vector<std::int64_t> latencies;
        latencies.reserve(1 << 16);
        std::vector<Envelope> batch(cfg_.consumer_batch);
        std::vector<Fill> fills;
        fills.reserve(256);

        std::thread consumer([&] {
            pin_current_thread(cfg_.consumer_cpu);
            while (!start.load(std::memory_order_acquire)) cpu_relax();
            Backoff backoff;
            for (;;) {
                std::size_t n = queue_.try_pop_n(batch.data(), batch.size());
                if (n == 0) {
                    if (producer_done.load(std::memory_order_acquire)) {
                        // Re-check after observing `done`: the producer's last
                        // pushes happen-before its release-store of `done`.
                        n = queue_.try_pop_n(batch.data(), batch.size());
                        if (n == 0) break;
                    } else {
                        ++result.consumer_idle;
                        backoff.pause();
                        continue;
                    }
                }
                backoff.reset();
                for (std::size_t i = 0; i < n; ++i) {
                    const Envelope& env = batch[i];
                    fills.clear();
                    book.apply(env.event, fills);
                    result.fills += fills.size();
                    if (handler) handler(env.event, fills, book);
                    if (env.enqueue_ns != 0) latencies.push_back(mono_ns() - env.enqueue_ns);
                }
                result.events += n;
            }
        });

        const auto t0 = mono_ns();
        start.store(true, std::memory_order_release);
        producer.join();
        consumer.join();
        const auto t1 = mono_ns();

        result.seconds = static_cast<double>(t1 - t0) / 1e9;
        result.events_per_sec =
            result.seconds > 0 ? static_cast<double>(result.events) / result.seconds : 0.0;
        result.latency_ns = summarize(latencies);
        result.producer_stalls = producer_stalls;
        result.queue_name = Queue::name();
        return result;
    }

    // Replays a pre-built event vector (named differently from run() so that a
    // non-const vector lvalue never binds to the Source template).
    PipelineResult replay(const std::vector<MarketEvent>& events, OrderBook& book,
                          const BookUpdateHandler& handler = {}) {
        std::size_t i = 0;
        return run(
            [&](MarketEvent& out) {
                if (i == events.size()) return false;
                out = events[i++];
                return true;
            },
            book, handler);
    }

private:
    PipelineConfig cfg_;
    Queue queue_;
};

}  // namespace mds
