// SPDX-License-Identifier: MIT
//
// mds_query - read analytics series back out of Redis.
//
//   mds_query --redis 127.0.0.1:6379 --symbol SIM --field mid --agg avg --bucket-ms 1000
//   mds_query --redis 127.0.0.1:6379 --symbol SIM --field trade_qty --agg sum --bucket-ms 60000
//
// With RedisTimeSeries the aggregation runs server-side (TS.RANGE ... AGGREGATION);
// with the sorted-set fallback the same buckets are computed client-side.
#include <cstdio>
#include <exception>
#include <string>

#include "mds/common/cli.hpp"
#include "mds/storage/tick_store.hpp"

using namespace mds;

int main(int argc, char** argv) try {
    CliArgs args(argc, argv);
    if (args.has("help") || !args.has("redis")) {
        std::puts(
            "usage: mds_query --redis HOST:PORT [--symbol SIM] [--field mid]\n"
            "                 [--from-ms 0] [--to-ms max] [--agg avg|min|max|first|last|sum|count]\n"
            "                 [--bucket-ms 1000] [--limit 20]\n"
            "fields: bid ask mid spread imbalance trade_px trade_qty");
        return args.has("help") ? 0 : 1;
    }
    std::string host;
    std::uint16_t port;
    RedisClient::parse_endpoint(args.str("redis"), host, port);
    RedisClient redis;
    redis.connect(host, port);

    TickStoreConfig tc;
    tc.symbol = args.str("symbol", "SIM");
    TickStore store(redis, tc);
    store.init();

    const auto samples =
        store.range(args.str("field", "mid"), args.i64("from-ms", 0),
                    args.i64("to-ms", INT64_MAX / kNanosPerMilli),
                    parse_aggregation(args.str("agg", "none")), args.i64("bucket-ms", 0));
    const auto limit = static_cast<std::size_t>(args.i64("limit", 20));
    std::printf("# backend=%s key=%s samples=%zu\n",
                store.using_timeseries() ? "RedisTimeSeries" : "sorted-set",
                store.key("ts:" + args.str("field", "mid")).c_str(), samples.size());
    std::puts("ts_ms,value");
    for (std::size_t i = 0; i < samples.size() && i < limit; ++i) {
        std::printf("%lld,%.6f\n", static_cast<long long>(samples[i].first), samples[i].second);
    }
    if (samples.size() > limit) std::printf("... (%zu more)\n", samples.size() - limit);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "mds_query: %s\n", e.what());
    return 1;
}
