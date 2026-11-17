// SPDX-License-Identifier: MIT
//
// bench_queue - lock-free SPSC ring vs mutex-protected ring.
//
// Two levels of measurement:
//
//   raw       producer pushes N 48-byte envelopes, consumer pops and checksums
//             them. Isolates the cost of the queue itself.
//   pipeline  the full MarketDataPipeline: same feed, same OrderBook work on
//             the consumer, only the queue type differs.
//
// Each configuration is repeated --reps times and the MEDIAN is reported
// (robust to a noisy neighbour / frequency scaling on one run). Queue
// capacity, payload, batch size and thread pinning are identical across the
// variants being compared.
//
//   bench_queue --messages 20000000 --reps 5 --pin 2,4
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "mds/common/affinity.hpp"
#include "mds/common/cli.hpp"
#include "mds/common/clock.hpp"
#include "mds/common/json.hpp"
#include "mds/feed/synthetic_feed.hpp"
#include "mds/pipeline/mutex_queue.hpp"
#include "mds/pipeline/pipeline.hpp"
#include "mds/pipeline/spsc_ring.hpp"

using namespace mds;

namespace {

struct RawResult {
    double msgs_per_sec{0};
    std::uint64_t checksum{0};
};

template <class Queue>
RawResult raw_run(std::size_t messages, std::size_t capacity, std::size_t batch, int pcpu,
                  int ccpu) {
    Queue q(capacity);
    std::atomic<bool> go{false};
    std::uint64_t checksum = 0;
    bool order_ok = true;

    std::thread consumer([&] {
        pin_current_thread(ccpu);
        while (!go.load(std::memory_order_acquire)) cpu_relax();
        std::vector<Envelope> buf(batch);
        std::uint64_t expected = 0;
        while (expected < messages) {
            std::size_t n;
            if (batch > 1) {
                n = q.try_pop_n(buf.data(), batch);
            } else {
                n = q.try_pop(buf[0]) ? 1 : 0;
            }
            if (n == 0) {
                cpu_relax();
                continue;
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (buf[i].event.id != expected) order_ok = false;  // FIFO check
                checksum += buf[i].event.id ^ static_cast<std::uint64_t>(buf[i].event.qty);
                ++expected;
            }
        }
    });

    pin_current_thread(pcpu);
    Envelope env;
    env.event.qty = 100;
    const auto t0 = mono_ns();
    go.store(true, std::memory_order_release);
    for (std::uint64_t i = 0; i < messages; ++i) {
        env.event.id = i;
        while (!q.try_push(env)) cpu_relax();
    }
    consumer.join();
    const auto t1 = mono_ns();
    pin_current_thread(-1);
    if (!order_ok) {
        std::fprintf(stderr, "FIFO violation in %s\n", Queue::name());
        std::exit(2);
    }
    return RawResult{static_cast<double>(messages) / (static_cast<double>(t1 - t0) / 1e9), checksum};
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

template <class Queue>
double raw_median(int reps, std::size_t messages, std::size_t capacity, std::size_t batch,
                  int pcpu, int ccpu, std::uint64_t& checksum) {
    std::vector<double> r;
    for (int i = 0; i < reps; ++i) {
        const auto res = raw_run<Queue>(messages, capacity, batch, pcpu, ccpu);
        r.push_back(res.msgs_per_sec);
        checksum = res.checksum;
    }
    return median(r);
}

template <class Queue>
PipelineResult pipeline_median(int reps, const PipelineConfig& pc,
                               const std::vector<MarketEvent>& events) {
    std::vector<PipelineResult> results;
    for (int i = 0; i < reps; ++i) {
        OrderBook book;
        MarketDataPipeline<Queue> p(pc);
        results.push_back(p.replay(events, book));
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.events_per_sec < b.events_per_sec;
    });
    return results[results.size() / 2];
}

}  // namespace

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    const auto messages = static_cast<std::size_t>(args.i64("messages", 20'000'000));
    const auto pipeline_events = static_cast<std::size_t>(args.i64("events", 2'000'000));
    const auto capacity = static_cast<std::size_t>(args.i64("capacity", 1 << 16));
    const int reps = static_cast<int>(args.i64("reps", 5));
    int pcpu = -1, ccpu = -1;
    if (args.has("pin")) {
        const std::string pin = args.str("pin");
        const auto comma = pin.find(',');
        pcpu = std::stoi(pin.substr(0, comma));
        ccpu = std::stoi(pin.substr(comma + 1));
    }
    const bool skip_raw = args.has("pipeline-only");
    const bool skip_pipeline = args.has("raw-only");

    struct Row {
        const char* layer;
        std::string variant;
        double rate;
        double vs_mutex;
        double p50_ns;
        double p99_ns;
    };
    std::vector<Row> rows;

    std::printf("bench_queue: payload=%zu B capacity=%zu reps=%d (median) pin=%d,%d hw_threads=%u\n",
                sizeof(Envelope), capacity, reps, pcpu, ccpu,
                std::thread::hardware_concurrency());

    if (!skip_raw) {
        std::printf("\n[raw queue] %zu messages\n", messages);
        std::printf("  %-28s %14s %10s\n", "variant", "msgs/s", "vs mutex");
        std::uint64_t c1 = 0, c2 = 0, c3 = 0, c4 = 0;
        const double mutex1 =
            raw_median<MutexQueue<Envelope>>(reps, messages, capacity, 1, pcpu, ccpu, c1);
        const double mutex64 =
            raw_median<MutexQueue<Envelope>>(reps, messages, capacity, 64, pcpu, ccpu, c2);
        const double spsc1 =
            raw_median<SpscRing<Envelope>>(reps, messages, capacity, 1, pcpu, ccpu, c3);
        const double spsc64 =
            raw_median<SpscRing<Envelope>>(reps, messages, capacity, 64, pcpu, ccpu, c4);
        if (c1 != c2 || c2 != c3 || c3 != c4) {
            std::fprintf(stderr, "checksum mismatch between variants\n");
            return 2;
        }
        std::printf("  %-28s %14.0f %9.2fx\n", "mutex   (pop 1)", mutex1, 1.0);
        std::printf("  %-28s %14.0f %9.2fx\n", "mutex   (pop batch 64)", mutex64, mutex64 / mutex1);
        std::printf("  %-28s %14.0f %9.2fx\n", "spsc    (pop 1)", spsc1, spsc1 / mutex1);
        std::printf("  %-28s %14.0f %9.2fx\n", "spsc    (pop batch 64)", spsc64, spsc64 / mutex1);
        rows.push_back({"raw", "mutex pop 1", mutex1, 1.0, 0, 0});
        rows.push_back({"raw", "mutex pop batch 64", mutex64, 1.0, 0, 0});
        rows.push_back({"raw", "spsc pop 1", spsc1, spsc1 / mutex1, 0, 0});
        rows.push_back({"raw", "spsc pop batch 64", spsc64, spsc64 / mutex64, 0, 0});
        std::printf("  like-for-like: spsc/mutex pop1 = %.2fx, batch64 = %.2fx\n", spsc1 / mutex1,
                    spsc64 / mutex64);
    }

    if (!skip_pipeline) {
        std::printf("\n[pipeline: feed -> queue -> order book] %zu events\n", pipeline_events);
        SyntheticFeedConfig fc;
        fc.seed = 1234;
        SyntheticFeed feed(fc);
        const auto events = feed.generate(pipeline_events);

        PipelineConfig pc;
        pc.queue_capacity = capacity;
        pc.producer_cpu = pcpu;
        pc.consumer_cpu = ccpu;
        std::printf("  %-28s %14s %10s %10s %10s\n", "variant", "events/s", "p50 ns", "p99 ns",
                    "vs mutex");
        for (std::size_t batch : {std::size_t{1}, std::size_t{64}}) {
            pc.consumer_batch = batch;
            const auto m = pipeline_median<MutexQueue<Envelope>>(reps, pc, events);
            const auto s = pipeline_median<SpscRing<Envelope>>(reps, pc, events);
            std::printf("  mutex   (batch %-3zu)          %14.0f %10.0f %10.0f %9.2fx\n", batch,
                        m.events_per_sec, m.latency_ns.p50, m.latency_ns.p99, 1.0);
            std::printf("  spsc    (batch %-3zu)          %14.0f %10.0f %10.0f %9.2fx\n", batch,
                        s.events_per_sec, s.latency_ns.p50, s.latency_ns.p99,
                        s.events_per_sec / m.events_per_sec);
            rows.push_back({"pipeline", "mutex batch " + std::to_string(batch), m.events_per_sec, 1.0,
                            m.latency_ns.p50, m.latency_ns.p99});
            rows.push_back({"pipeline", "spsc batch " + std::to_string(batch), s.events_per_sec,
                            s.events_per_sec / m.events_per_sec, s.latency_ns.p50, s.latency_ns.p99});
        }
    }

    if (args.has("json")) {
        JsonWriter j(args.str("json"));
        j.begin_object();
        j.kv("hw_threads", static_cast<std::uint64_t>(std::thread::hardware_concurrency()));
        j.kv("reps", reps).kv("messages", static_cast<std::uint64_t>(messages));
        j.kv("events", static_cast<std::uint64_t>(pipeline_events));
        j.kv("payload_bytes", static_cast<std::uint64_t>(sizeof(Envelope)));
        j.key("rows").begin_array();
        for (const auto& r : rows) {
            j.begin_object().kv("layer", r.layer).kv("variant", r.variant).kv("rate", r.rate, 1);
            j.kv("vs_mutex", r.vs_mutex, 3).kv("p50_ns", r.p50_ns, 0).kv("p99_ns", r.p99_ns, 0);
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "bench_queue: %s\n", e.what());
    return 1;
}
