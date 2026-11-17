// SPDX-License-Identifier: MIT
//
// Redis-backed tick storage.
//
// Two complementary representations are written for each symbol:
//
// 1. Analytics series (RedisTimeSeries, one key per field)
//      mds:{SYM}:ts:bid  ask  mid  spread  imbalance  trade_px  trade_qty
//    * Down-sampled to one sample per millisecond (RedisTimeSeries timestamps
//      are ms). bid/ask/mid/... keep the LAST book state in each ms,
//      trade_qty uses DUPLICATE_POLICY SUM so it becomes volume-per-ms.
//    * Labels (symbol=, field=, source=mds) allow TS.MRANGE ... FILTER queries
//      across symbols, and server-side AGGREGATION gives OHLC / VWAP-style
//      bars without shipping raw ticks to the client.
//    * If the RedisTimeSeries module is absent the store falls back to plain
//      sorted sets (score = ts_ms, member = "ts_ms:value") with client-side
//      aggregation, so the project still works against vanilla Redis.
//
// 2. Lossless event log (for exact replay into the backtester)
//      mds:{SYM}:chunk:<n>   binary STRING: N packed 40-byte MarketEvents
//      mds:{SYM}:chunks      ZSET  score = first ts (ms) of chunk, member = n
//      mds:{SYM}:meta        HASH  events, chunks, first_ts, last_ts, schema
//    Storing chunks of ~4096 events instead of one key per tick cuts per-key
//    overhead (~50-70 bytes of dict/robj metadata each) by >99% and turns a
//    replay into one ZRANGEBYSCORE + one MGET.
//
// The {SYM} hash tag pins every key of a symbol to the same Redis Cluster
// slot, so multi-key commands (MGET, TS.MADD) remain legal under clustering.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mds/common/event.hpp"
#include "mds/orderbook/order_book.hpp"
#include "mds/storage/redis_client.hpp"

namespace mds {

struct TickStoreConfig {
    std::string prefix{"mds"};
    std::string symbol{"SIM"};
    std::size_t chunk_events{4096};
    std::size_t pipeline_depth{256};       // commands buffered before a flush
    std::size_t madd_batch_samples{512};   // samples per TS.MADD command
    std::int64_t retention_ms{0};          // 0 == keep forever
    bool force_sorted_set_fallback{false};
};

struct TickStoreStats {
    std::uint64_t snapshots{0};
    std::uint64_t series_samples{0};
    std::uint64_t events{0};
    std::uint64_t chunks{0};
};

using Sample = std::pair<std::int64_t, double>;  // (ts_ms, value)

enum class Aggregation { None, Avg, Min, Max, First, Last, Sum, Count };
Aggregation parse_aggregation(const std::string& s);

class TickStore {
public:
    static constexpr const char* kFields[] = {"bid",       "ask",      "mid",      "spread",
                                              "imbalance", "trade_px", "trade_qty"};

    TickStore(RedisClient& redis, TickStoreConfig cfg = {});

    // Detects RedisTimeSeries, creates the series with labels. Idempotent.
    void init();
    [[nodiscard]] bool using_timeseries() const noexcept { return use_ts_; }

    // ---- write path --------------------------------------------------------
    void record_book(Timestamp ts, const OrderBook& book);  // top-of-book snapshot
    void record_trade(Timestamp ts, Price price, Qty qty);
    void record_event(const MarketEvent& ev);                // lossless log
    void flush();

    // ---- read path ---------------------------------------------------------
    std::vector<Sample> range(const std::string& field, std::int64_t from_ms, std::int64_t to_ms,
                              Aggregation agg = Aggregation::None, std::int64_t bucket_ms = 0);
    std::vector<MarketEvent> load_events(Timestamp from_ns, Timestamp to_ns);

    [[nodiscard]] const TickStoreStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::string key(const std::string& suffix) const;

    // Deletes every key this store owns for the configured symbol.
    void purge();

    // Client-side bucket aggregation (also used for the sorted-set fallback).
    static std::vector<Sample> aggregate(const std::vector<Sample>& raw, Aggregation agg,
                                         std::int64_t bucket_ms);

private:
    struct Snapshot {
        std::int64_t ms{-1};
        double bid{0}, ask{0}, mid{0}, spread{0}, imbalance{0};
    };

    struct TradeBucket {
        std::int64_t ms{-1};
        double last_px{0};
        double qty{0};
    };

    void emit_sample(const char* field, std::int64_t ts_ms, double value);
    void emit_snapshot(const Snapshot& s);
    void emit_trades(const TradeBucket& t);
    void flush_samples();
    void seal_chunk();
    void maybe_flush_pipeline();
    void check_replies(const std::vector<RedisReply>& replies);

    RedisClient& redis_;
    TickStoreConfig cfg_;
    bool use_ts_{false};
    bool initialised_{false};

    Snapshot pending_snapshot_;
    TradeBucket pending_trades_;
    std::vector<std::string> madd_args_;   // accumulating TS.MADD argv
    std::size_t madd_samples_{0};
    std::string chunk_buf_;                // packed events of the open chunk
    std::size_t chunk_count_{0};
    Timestamp chunk_first_ts_{0};
    std::uint64_t next_chunk_id_{0};
    Timestamp first_event_ts_{0};
    Timestamp last_event_ts_{0};
    TickStoreStats stats_;
};

}  // namespace mds
