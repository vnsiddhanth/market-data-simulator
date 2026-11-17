// SPDX-License-Identifier: MIT
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "mds/feed/synthetic_feed.hpp"
#include "mds/pipeline/mutex_queue.hpp"
#include "mds/pipeline/pipeline.hpp"
#include "mds/pipeline/spsc_ring.hpp"
#include "test_framework.hpp"

using namespace mds;

TEST(spsc_ring, capacity_rounds_to_power_of_two) {
    SpscRing<int> q(1000);
    CHECK_EQ(q.capacity(), 1024u);
}

TEST(spsc_ring, single_thread_fifo_full_empty) {
    SpscRing<int> q(4);
    int v = 0;
    CHECK(!q.try_pop(v));
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));  // full: all 4 slots usable, none sacrificed
    CHECK_EQ(q.size_approx(), 4u);
    for (int i = 0; i < 4; ++i) {
        CHECK(q.try_pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(!q.try_pop(v));
    CHECK(q.empty_approx());
}

TEST(spsc_ring, wraparound_many_times) {
    SpscRing<std::uint64_t> q(8);
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < 10'000; ++i) {
        CHECK(q.try_push(i));
        CHECK(q.try_pop(v));
        CHECK_EQ(v, i);
    }
}

TEST(spsc_ring, batch_push_pop) {
    SpscRing<int> q(8);
    int in[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK_EQ(q.try_push_n(in, 10), 8u);  // only 8 fit
    int out[16];
    CHECK_EQ(q.try_pop_n(out, 3), 3u);
    CHECK_EQ(out[2], 2);
    CHECK_EQ(q.try_push_n(in + 8, 2), 2u);
    CHECK_EQ(q.try_pop_n(out, 16), 7u);
    CHECK_EQ(out[0], 3);
    CHECK_EQ(out[6], 9);
}

TEST(spsc_ring, front_pop_front_zero_copy) {
    SpscRing<int> q(4);
    CHECK(q.front() == nullptr);
    CHECK(q.try_push(7));
    int* p = q.front();
    CHECK(p != nullptr);
    CHECK_EQ(*p, 7);
    q.pop_front();
    CHECK(q.front() == nullptr);
}

namespace {

// Two threads, tiny ring (forces constant full/empty transitions and index
// cache refreshes), strict sequence + checksum validation.
template <class Queue>
void concurrent_stress(std::size_t capacity, std::size_t batch) {
    constexpr std::uint64_t kN = 2'000'000;
    Queue q(capacity);
    std::atomic<bool> ok{true};
    std::uint64_t sum = 0;
    std::thread consumer([&] {
        std::vector<std::uint64_t> buf(batch);
        std::uint64_t expected = 0;
        while (expected < kN) {
            const std::size_t n = q.try_pop_n(buf.data(), batch);
            for (std::size_t i = 0; i < n; ++i) {
                if (buf[i] != expected) ok = false;
                sum += buf[i];
                ++expected;
            }
        }
    });
    for (std::uint64_t i = 0; i < kN; ++i) {
        while (!q.try_push(i)) {
        }
    }
    consumer.join();
    CHECK(ok.load());
    CHECK_EQ(sum, kN * (kN - 1) / 2);
}

}  // namespace

TEST(spsc_ring, concurrent_stress_small_ring) { concurrent_stress<SpscRing<std::uint64_t>>(16, 1); }
TEST(spsc_ring, concurrent_stress_batched) { concurrent_stress<SpscRing<std::uint64_t>>(1024, 32); }
TEST(mutex_queue, concurrent_stress) { concurrent_stress<MutexQueue<std::uint64_t>>(64, 8); }

namespace {

bool books_equal(const OrderBook& a, const OrderBook& b) {
    if (a.order_count() != b.order_count()) return false;
    for (Side s : {Side::Buy, Side::Sell}) {
        const auto da = a.depth(s, 100000);
        const auto db = b.depth(s, 100000);
        if (da.size() != db.size()) return false;
        for (std::size_t i = 0; i < da.size(); ++i) {
            if (da[i].price != db[i].price || da[i].qty != db[i].qty) return false;
        }
    }
    return true;
}

}  // namespace

TEST(pipeline, downstream_book_matches_exchange_spsc) {
    SyntheticFeedConfig fc;
    fc.seed = 11;
    SyntheticFeed feed(fc);
    const auto events = feed.generate(200'000);

    PipelineConfig pc;
    pc.queue_capacity = 256;  // small ring: exercise back-pressure
    MarketDataPipeline<SpscRing<Envelope>> p(pc);
    OrderBook book;
    std::uint64_t handler_calls = 0;
    const auto r = p.replay(events, book, [&](const MarketEvent&, const std::vector<Fill>&,
                                             const OrderBook&) { ++handler_calls; });
    CHECK_EQ(r.events, events.size());
    CHECK_EQ(handler_calls, events.size());
    CHECK(books_equal(book, feed.exchange_book()));
    CHECK(book.validate());
    CHECK(r.fills > 0);
    CHECK(r.latency_ns.samples > 0);
}

TEST(pipeline, mutex_and_spsc_produce_identical_books) {
    SyntheticFeedConfig fc;
    fc.seed = 5;
    SyntheticFeed feed(fc);
    const auto events = feed.generate(100'000);
    OrderBook a, b;
    MarketDataPipeline<SpscRing<Envelope>>().replay(events, a);
    MarketDataPipeline<MutexQueue<Envelope>>().replay(events, b);
    CHECK(books_equal(a, b));
}

TEST(pipeline, live_generation_on_producer_thread) {
    SyntheticFeedConfig fc;
    fc.seed = 3;
    fc.max_events = 50'000;
    SyntheticFeed feed(fc);
    OrderBook book;
    const auto r = MarketDataPipeline<SpscRing<Envelope>>().run(
        [&feed](MarketEvent& ev) { return feed.next(ev); }, book);
    CHECK_EQ(r.events, 50'000u);
    CHECK(books_equal(book, feed.exchange_book()));
}
