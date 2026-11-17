// SPDX-License-Identifier: MIT
#include "mds/storage/tick_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace mds {

namespace {

std::string fmt_double(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

bool contains_ci(const std::string& hay, const char* needle) {
    std::string h = hay;
    std::string n = needle;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
    return h.find(n) != std::string::npos;
}

double reply_to_double(const RedisReply& r) {
    if (r.type == RedisReply::Type::Integer) return static_cast<double>(r.integer);
    return std::strtod(r.str.c_str(), nullptr);
}

const char* agg_name(Aggregation a) {
    switch (a) {
        case Aggregation::Avg: return "avg";
        case Aggregation::Min: return "min";
        case Aggregation::Max: return "max";
        case Aggregation::First: return "first";
        case Aggregation::Last: return "last";
        case Aggregation::Sum: return "sum";
        case Aggregation::Count: return "count";
        case Aggregation::None: break;
    }
    return "";
}

constexpr std::int64_t to_ms(Timestamp ns) { return ns / kNanosPerMilli; }

}  // namespace

Aggregation parse_aggregation(const std::string& s) {
    if (s.empty() || s == "none") return Aggregation::None;
    if (s == "avg") return Aggregation::Avg;
    if (s == "min") return Aggregation::Min;
    if (s == "max") return Aggregation::Max;
    if (s == "first") return Aggregation::First;
    if (s == "last") return Aggregation::Last;
    if (s == "sum") return Aggregation::Sum;
    if (s == "count") return Aggregation::Count;
    throw std::invalid_argument("unknown aggregation: " + s);
}

TickStore::TickStore(RedisClient& redis, TickStoreConfig cfg) : redis_(redis), cfg_(std::move(cfg)) {
    chunk_buf_.reserve(cfg_.chunk_events * sizeof(MarketEvent));
}

std::string TickStore::key(const std::string& suffix) const {
    return cfg_.prefix + ":{" + cfg_.symbol + "}:" + suffix;
}

void TickStore::init() {
    if (initialised_) return;

    use_ts_ = false;
    if (!cfg_.force_sorted_set_fallback) {
        // Probe for the module: an unknown command means vanilla Redis.
        const RedisReply probe = redis_.command({"TS.INFO", key("ts:__probe__")});
        use_ts_ = !(probe.is_error() && contains_ci(probe.str, "unknown command"));
    }

    if (use_ts_) {
        for (const char* field : kFields) {
            std::vector<std::string> args{"TS.CREATE", key(std::string("ts:") + field)};
            if (cfg_.retention_ms > 0) {
                args.insert(args.end(), {"RETENTION", std::to_string(cfg_.retention_ms)});
            }
            const bool is_volume = std::strcmp(field, "trade_qty") == 0;
            args.insert(args.end(), {"DUPLICATE_POLICY", is_volume ? "SUM" : "LAST"});
            args.insert(args.end(),
                        {"LABELS", "symbol", cfg_.symbol, "field", field, "source", "mds"});
            redis_.append(args);
        }
        for (const auto& r : redis_.flush()) {
            if (r.is_error() && !contains_ci(r.str, "already exists")) {
                throw RedisError("TS.CREATE failed: " + r.str);
            }
        }
    }

    // Resume chunk numbering if the symbol already has data.
    const RedisReply n = redis_.command({"HGET", key("meta"), "chunks"});
    if (n.type == RedisReply::Type::Bulk) next_chunk_id_ = std::stoull(n.str);
    redis_.command({"HSET", key("meta"), "schema", "MDSEVT01", "event_bytes",
                    std::to_string(sizeof(MarketEvent)),
                    "backend", use_ts_ ? "redistimeseries" : "zset"});
    initialised_ = true;
}

// ---------------------------------------------------------------------------
// Write path
// ---------------------------------------------------------------------------

void TickStore::check_replies(const std::vector<RedisReply>& replies) {
    for (const auto& r : replies) {
        if (r.is_error()) throw RedisError("redis error: " + r.str);
        if (r.is_array()) {
            for (const auto& el : r.elements) {
                if (el.is_error()) throw RedisError("redis error: " + el.str);
            }
        }
    }
}

void TickStore::maybe_flush_pipeline() {
    if (redis_.pending() >= cfg_.pipeline_depth) check_replies(redis_.flush());
}

void TickStore::flush_samples() {
    if (madd_samples_ == 0) return;
    redis_.append(madd_args_);
    madd_args_.clear();
    madd_samples_ = 0;
    maybe_flush_pipeline();
}

void TickStore::emit_sample(const char* field, std::int64_t ts_ms, double value) {
    const std::string k = key(std::string("ts:") + field);
    const std::string ts = std::to_string(ts_ms);
    const std::string v = fmt_double(value);
    ++stats_.series_samples;
    if (use_ts_) {
        // TS.MADD writes many (key, ts, value) triples in one command.
        if (madd_args_.empty()) madd_args_.emplace_back("TS.MADD");
        madd_args_.push_back(k);
        madd_args_.push_back(ts);
        madd_args_.push_back(v);
        if (++madd_samples_ >= cfg_.madd_batch_samples) flush_samples();
    } else {
        redis_.append({"ZADD", k, ts, ts + ":" + v});
        maybe_flush_pipeline();
    }
}

void TickStore::emit_snapshot(const Snapshot& s) {
    emit_sample("bid", s.ms, s.bid);
    emit_sample("ask", s.ms, s.ask);
    emit_sample("mid", s.ms, s.mid);
    emit_sample("spread", s.ms, s.spread);
    emit_sample("imbalance", s.ms, s.imbalance);
}

void TickStore::emit_trades(const TradeBucket& t) {
    emit_sample("trade_px", t.ms, t.last_px);
    emit_sample("trade_qty", t.ms, t.qty);
}

void TickStore::record_book(Timestamp ts, const OrderBook& book) {
    const auto bb = book.best_bid();
    const auto ba = book.best_ask();
    if (!bb || !ba) return;
    const std::int64_t ms = to_ms(ts);
    // Keep only the last book state of each millisecond.
    if (pending_snapshot_.ms >= 0 && ms != pending_snapshot_.ms) emit_snapshot(pending_snapshot_);
    pending_snapshot_.ms = ms;
    pending_snapshot_.bid = to_double(*bb);
    pending_snapshot_.ask = to_double(*ba);
    pending_snapshot_.mid = (pending_snapshot_.bid + pending_snapshot_.ask) / 2.0;
    pending_snapshot_.spread = pending_snapshot_.ask - pending_snapshot_.bid;
    pending_snapshot_.imbalance = book.imbalance(5);
    ++stats_.snapshots;
}

void TickStore::record_trade(Timestamp ts, Price price, Qty qty) {
    const std::int64_t ms = to_ms(ts);
    if (pending_trades_.ms >= 0 && ms != pending_trades_.ms) {
        emit_trades(pending_trades_);
        pending_trades_.qty = 0;
    }
    pending_trades_.ms = ms;
    pending_trades_.last_px = to_double(price);
    pending_trades_.qty += static_cast<double>(qty);
}

void TickStore::record_event(const MarketEvent& ev) {
    if (chunk_count_ == 0) chunk_first_ts_ = ev.ts;
    if (stats_.events == 0) first_event_ts_ = ev.ts;
    last_event_ts_ = ev.ts;
    chunk_buf_.append(reinterpret_cast<const char*>(&ev), sizeof(MarketEvent));
    ++chunk_count_;
    ++stats_.events;
    if (chunk_count_ >= cfg_.chunk_events) seal_chunk();
}

void TickStore::seal_chunk() {
    if (chunk_count_ == 0) return;
    const std::string id = std::to_string(next_chunk_id_++);
    redis_.append({"SET", key("chunk:" + id), chunk_buf_});
    redis_.append({"ZADD", key("chunks"), std::to_string(to_ms(chunk_first_ts_)), id});
    redis_.append({"HINCRBY", key("meta"), "events", std::to_string(chunk_count_)});
    redis_.append({"HSETNX", key("meta"), "first_ts", std::to_string(first_event_ts_)});
    redis_.append({"HSET", key("meta"), "chunks", std::to_string(next_chunk_id_), "last_ts",
                   std::to_string(last_event_ts_)});
    chunk_buf_.clear();
    chunk_count_ = 0;
    ++stats_.chunks;
    maybe_flush_pipeline();
}

void TickStore::flush() {
    if (pending_snapshot_.ms >= 0) {
        emit_snapshot(pending_snapshot_);
        pending_snapshot_.ms = -1;
    }
    if (pending_trades_.ms >= 0) {
        emit_trades(pending_trades_);
        pending_trades_ = TradeBucket{};
    }
    flush_samples();
    seal_chunk();
    check_replies(redis_.flush());
}

// ---------------------------------------------------------------------------
// Read path
// ---------------------------------------------------------------------------

std::vector<Sample> TickStore::aggregate(const std::vector<Sample>& raw, Aggregation agg,
                                         std::int64_t bucket_ms) {
    if (agg == Aggregation::None || bucket_ms <= 0) return raw;
    std::vector<Sample> out;
    std::size_t i = 0;
    while (i < raw.size()) {
        const std::int64_t bucket = raw[i].first - (((raw[i].first % bucket_ms) + bucket_ms) % bucket_ms);
        double acc = raw[i].second;
        double sum = 0.0;
        std::size_t n = 0;
        double last = raw[i].second;
        for (; i < raw.size() && raw[i].first < bucket + bucket_ms; ++i) {
            const double v = raw[i].second;
            sum += v;
            ++n;
            last = v;
            if (agg == Aggregation::Min) acc = std::min(acc, v);
            if (agg == Aggregation::Max) acc = std::max(acc, v);
        }
        double value = acc;  // First / Min / Max
        if (agg == Aggregation::Avg) value = sum / static_cast<double>(n);
        if (agg == Aggregation::Sum) value = sum;
        if (agg == Aggregation::Count) value = static_cast<double>(n);
        if (agg == Aggregation::Last) value = last;
        out.emplace_back(bucket, value);
    }
    return out;
}

std::vector<Sample> TickStore::range(const std::string& field, std::int64_t from_ms,
                                     std::int64_t to_ms, Aggregation agg, std::int64_t bucket_ms) {
    const std::string k = key("ts:" + field);
    std::vector<Sample> out;
    if (use_ts_) {
        std::vector<std::string> args{"TS.RANGE", k, std::to_string(from_ms), std::to_string(to_ms)};
        if (agg != Aggregation::None && bucket_ms > 0) {
            args.insert(args.end(), {"AGGREGATION", agg_name(agg), std::to_string(bucket_ms)});
        }
        const RedisReply r = redis_.command(args);
        if (r.is_error()) throw RedisError("TS.RANGE: " + r.str);
        out.reserve(r.elements.size());
        for (const auto& pt : r.elements) {
            if (pt.elements.size() != 2) continue;
            out.emplace_back(pt.elements[0].integer, reply_to_double(pt.elements[1]));
        }
        return out;
    }

    const RedisReply r =
        redis_.command({"ZRANGEBYSCORE", k, std::to_string(from_ms), std::to_string(to_ms)});
    if (r.is_error()) throw RedisError("ZRANGEBYSCORE: " + r.str);
    out.reserve(r.elements.size());
    for (const auto& m : r.elements) {
        const auto colon = m.str.find(':');
        if (colon == std::string::npos) continue;
        out.emplace_back(std::stoll(m.str.substr(0, colon)), std::stod(m.str.substr(colon + 1)));
    }
    return aggregate(out, agg, bucket_ms);
}

std::vector<MarketEvent> TickStore::load_events(Timestamp from_ns, Timestamp to_ns) {
    const std::string from_ms = std::to_string(to_ms(from_ns));
    const std::string to_ms_s = std::to_string(to_ms(to_ns));

    // The chunk that starts strictly before `from` may still hold events in
    // range, plus every chunk that starts inside [from, to].
    redis_.append({"ZREVRANGEBYSCORE", key("chunks"), "(" + from_ms, "-inf", "LIMIT", "0", "1"});
    redis_.append({"ZRANGEBYSCORE", key("chunks"), from_ms, to_ms_s});
    const auto replies = redis_.flush();
    check_replies(replies);

    std::set<std::uint64_t> ids;
    for (const auto& r : replies) {
        for (const auto& el : r.elements) ids.insert(std::stoull(el.str));
    }

    std::vector<MarketEvent> events;
    std::vector<std::string> mget{"MGET"};
    auto drain = [&] {
        if (mget.size() == 1) return;
        const RedisReply r = redis_.command(mget);
        if (r.is_error()) throw RedisError("MGET: " + r.str);
        for (const auto& blob : r.elements) {
            if (blob.type != RedisReply::Type::Bulk) continue;
            const std::size_t n = blob.str.size() / sizeof(MarketEvent);
            for (std::size_t i = 0; i < n; ++i) {
                MarketEvent ev;
                std::memcpy(&ev, blob.str.data() + i * sizeof(MarketEvent), sizeof(MarketEvent));
                if (ev.ts >= from_ns && ev.ts <= to_ns) events.push_back(ev);
            }
        }
        mget.resize(1);
    };
    for (std::uint64_t id : ids) {  // std::set iterates in chunk order == time order
        mget.push_back(key("chunk:" + std::to_string(id)));
        if (mget.size() > 64) drain();
    }
    drain();
    return events;
}

void TickStore::purge() {
    const RedisReply chunks = redis_.command({"ZRANGE", key("chunks"), "0", "-1"});
    std::vector<std::string> del{"DEL", key("chunks"), key("meta")};
    for (const char* f : kFields) del.push_back(key(std::string("ts:") + f));
    for (const auto& c : chunks.elements) del.push_back(key("chunk:" + c.str));
    redis_.command(del);
    next_chunk_id_ = 0;
}

}  // namespace mds
