// SPDX-License-Identifier: MIT
#include "mds/feed/synthetic_feed.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace mds {

SyntheticFeed::SyntheticFeed(SyntheticFeedConfig cfg)
    : cfg_(cfg),
      rng_(cfg.seed),
      inter_arrival_(cfg.events_per_second),
      placement_(cfg.placement_decay),
      qty_dist_(cfg.log_qty_mean, cfg.log_qty_sigma),
      book_(cfg.symbol, 1 << 16),
      fair_ticks_(cfg.start_price * static_cast<double>(kPriceScale) /
                  static_cast<double>(cfg.tick_size)),
      ts_accum_ns_(0.0) {
    if (cfg_.events_per_second <= 0) throw std::invalid_argument("events_per_second must be > 0");
    if (cfg_.tick_size <= 0) throw std::invalid_argument("tick_size must be > 0");
    live_ids_.reserve(1 << 16);
    scratch_fills_.reserve(64);
}

double SyntheticFeed::fair_value() const noexcept {
    return fair_ticks_ * static_cast<double>(cfg_.tick_size) / static_cast<double>(kPriceScale);
}

Qty SyntheticFeed::draw_qty() {
    const double raw = qty_dist_(rng_);
    const auto lots = std::max<long long>(1, std::llround(raw / cfg_.lot_size));
    return static_cast<Qty>(lots) * static_cast<Qty>(cfg_.lot_size);
}

bool SyntheticFeed::pick_live_order(OrderId& id) {
    // Filled orders are pruned lazily; occasionally rebuild so the candidate
    // list cannot grow without bound when most orders end up trading.
    if (live_ids_.size() > 4 * book_.order_count() + 1024) {
        live_ids_.erase(std::remove_if(live_ids_.begin(), live_ids_.end(),
                                       [this](OrderId oid) { return book_.find(oid) == nullptr; }),
                        live_ids_.end());
    }
    for (int attempt = 0; attempt < 8 && !live_ids_.empty(); ++attempt) {
        std::uniform_int_distribution<std::size_t> pick(0, live_ids_.size() - 1);
        const std::size_t j = pick(rng_);
        const OrderId candidate = live_ids_[j];
        live_ids_[j] = live_ids_.back();  // swap-remove: O(1)
        live_ids_.pop_back();
        if (book_.find(candidate) != nullptr) {
            id = candidate;
            return true;
        }
    }
    return false;
}

Side SyntheticFeed::taker_side() {
    // Informed flow: with probability `informed_fraction` an aggressor knows
    // the direction of the NEXT fair-value innovation (pre-drawn in next())
    // and trades with it. Liquidity providers who fill these orders see the
    // price move against them afterwards - adverse selection. Keying off the
    // future innovation rather than the current fair/mid gap avoids a
    // feedback loop with price impact (impact widens the gap, which would
    // otherwise trigger more same-side flow and a runaway trend).
    if (cfg_.informed_fraction > 0.0 && uniform_(rng_) < cfg_.informed_fraction) {
        return next_innovation_ >= 0.0 ? Side::Buy : Side::Sell;
    }
    return uniform_(rng_) < 0.5 ? Side::Buy : Side::Sell;
}

MarketEvent SyntheticFeed::make_add(Timestamp ts) {
    const bool aggressive = uniform_(rng_) < cfg_.aggressive_fraction;
    last_aggressive_ = aggressive;
    const Side side = aggressive ? taker_side() : (uniform_(rng_) < 0.5 ? Side::Buy : Side::Sell);
    const auto k = static_cast<long long>(placement_(rng_));
    const auto base = static_cast<long long>(std::floor(fair_ticks_));

    long long idx;
    if (side == Side::Buy) {
        idx = aggressive ? base + 1 + k : base - k;
    } else {
        idx = aggressive ? base - k : base + 1 + k;
    }
    idx = std::max<long long>(idx, 1);

    const OrderId id = next_id_++;
    const MarketEvent ev =
        MarketEvent::add(ts, id, side, static_cast<Price>(idx) * cfg_.tick_size, draw_qty(),
                         cfg_.symbol);
    live_ids_.push_back(id);
    return ev;
}

bool SyntheticFeed::make_cancel(Timestamp ts, MarketEvent& out) {
    OrderId id;
    if (!pick_live_order(id)) return false;
    out = MarketEvent::cancel(ts, id, cfg_.symbol);
    return true;
}

bool SyntheticFeed::make_modify(Timestamp ts, MarketEvent& out) {
    OrderId id;
    if (!pick_live_order(id)) return false;
    const Order* o = book_.find(id);
    const auto lot = static_cast<Qty>(cfg_.lot_size);
    Qty new_qty;
    if (uniform_(rng_) < 0.6) {
        new_qty = std::max(lot, (o->qty / 2 / lot) * lot);  // size down (keeps priority)
    } else {
        new_qty = o->qty + lot * (1 + static_cast<Qty>(placement_(rng_)));  // size up
    }
    live_ids_.push_back(id);  // still live after a modify
    if (new_qty == o->qty) new_qty += lot;
    out = MarketEvent::modify(ts, id, new_qty, cfg_.symbol);
    return true;
}

MarketEvent SyntheticFeed::make_market(Timestamp ts) {
    const Side side = taker_side();
    last_aggressive_ = true;
    return MarketEvent::market(ts, next_id_++, side, draw_qty(), cfg_.symbol);
}

bool SyntheticFeed::next(MarketEvent& out) {
    if (cfg_.max_events != 0 && emitted_ >= cfg_.max_events) return false;

    // Poisson arrivals + Brownian fair value scaled by sqrt(dt).
    const double dt = inter_arrival_(rng_);
    ts_accum_ns_ += dt * 1e9;
    const Timestamp ts = cfg_.start_ts + static_cast<Timestamp>(ts_accum_ns_);
    fair_ticks_ += cfg_.volatility_ticks_per_sqrt_sec * std::sqrt(dt) * next_innovation_;
    next_innovation_ = gauss_(rng_);  // what informed takers "know" about the next step
    fair_ticks_ = std::max(fair_ticks_, 10.0);

    const double total = cfg_.p_add + cfg_.p_cancel + cfg_.p_modify + cfg_.p_market;
    const double u = uniform_(rng_) * total;

    bool produced = false;
    last_aggressive_ = false;
    if (book_.order_count() >= cfg_.min_resting_orders) {
        if (u < cfg_.p_add) {
            out = make_add(ts);
            produced = true;
        } else if (u < cfg_.p_add + cfg_.p_cancel) {
            produced = make_cancel(ts, out);
        } else if (u < cfg_.p_add + cfg_.p_cancel + cfg_.p_modify) {
            produced = make_modify(ts, out);
        } else {
            out = make_market(ts);
            produced = true;
        }
    }
    if (!produced) out = make_add(ts);  // warm-up, or nothing live to cancel

    scratch_fills_.clear();
    book_.apply(out, scratch_fills_);

    // Permanent price impact (Kyle-style): traded volume moves fair value in
    // the aggressor's direction, so quotes that just traded keep getting
    // picked off as the price drifts away - i.e. adverse selection.
    // Only deliberate aggressors (market orders, adds priced through) carry
    // impact: passive adds that merely cross a stale book would otherwise feed
    // back into fair value and create a runaway trend.
    if (cfg_.impact_ticks_per_lot > 0.0 && last_aggressive_ && !scratch_fills_.empty()) {
        Qty traded = 0;
        for (const Fill& f : scratch_fills_) traded += f.qty;
        const double sign = scratch_fills_.front().aggressor == Side::Buy ? 1.0 : -1.0;
        fair_ticks_ += sign * cfg_.impact_ticks_per_lot * static_cast<double>(traded) / cfg_.lot_size;
    }
    ++emitted_;
    return true;
}

std::vector<MarketEvent> SyntheticFeed::generate(std::size_t n) {
    std::vector<MarketEvent> events;
    events.reserve(n);
    MarketEvent ev;
    while (events.size() < n && next(ev)) events.push_back(ev);
    return events;
}

// ---------------------------------------------------------------------------
// CSV / binary persistence
// ---------------------------------------------------------------------------

namespace {

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f) std::fclose(f);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr open_or_throw(const std::string& path, const char* mode) {
    FilePtr f(std::fopen(path.c_str(), mode));
    if (!f) throw std::runtime_error("cannot open " + path);
    return f;
}

EventType parse_type(const std::string& s) {
    if (s == "ADD") return EventType::Add;
    if (s == "CANCEL") return EventType::Cancel;
    if (s == "MODIFY") return EventType::Modify;
    if (s == "MARKET") return EventType::Market;
    throw std::runtime_error("bad event type: " + s);
}

constexpr char kMagic[8] = {'M', 'D', 'S', 'E', 'V', 'T', '0', '1'};

}  // namespace

void write_csv(const std::string& path, const std::vector<MarketEvent>& events) {
    auto f = open_or_throw(path, "w");
    std::fputs("ts,symbol,type,side,id,price,qty\n", f.get());
    for (const auto& e : events) {
        std::fprintf(f.get(), "%" PRId64 ",%" PRIu32 ",%s,%s,%" PRIu64 ",%.4f,%" PRId64 "\n",
                     static_cast<std::int64_t>(e.ts), static_cast<std::uint32_t>(e.symbol),
                     std::string(to_string(e.type)).c_str(), std::string(to_string(e.side)).c_str(),
                     static_cast<std::uint64_t>(e.id), to_double(e.price),
                     static_cast<std::int64_t>(e.qty));
    }
}

std::vector<MarketEvent> read_csv(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<MarketEvent> events;
    std::string line;
    std::getline(in, line);  // header
    std::size_t lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string ts, sym, type, side, id, price, qty;
        if (!std::getline(ss, ts, ',') || !std::getline(ss, sym, ',') ||
            !std::getline(ss, type, ',') || !std::getline(ss, side, ',') ||
            !std::getline(ss, id, ',') || !std::getline(ss, price, ',') ||
            !std::getline(ss, qty, ',')) {
            throw std::runtime_error("malformed CSV line " + std::to_string(lineno));
        }
        MarketEvent e;
        e.ts = std::stoll(ts);
        e.symbol = static_cast<SymbolId>(std::stoul(sym));
        e.type = parse_type(type);
        e.side = side == "SELL" ? Side::Sell : Side::Buy;
        e.id = std::stoull(id);
        e.price = to_price(std::stod(price));
        e.qty = std::stoll(qty);
        events.push_back(e);
    }
    return events;
}

void write_binary(const std::string& path, const std::vector<MarketEvent>& events) {
    auto f = open_or_throw(path, "wb");
    const std::uint64_t count = events.size();
    if (std::fwrite(kMagic, 1, sizeof(kMagic), f.get()) != sizeof(kMagic) ||
        std::fwrite(&count, sizeof(count), 1, f.get()) != 1 ||
        (count && std::fwrite(events.data(), sizeof(MarketEvent), events.size(), f.get()) !=
                      events.size())) {
        throw std::runtime_error("short write to " + path);
    }
}

std::vector<MarketEvent> read_binary(const std::string& path) {
    auto f = open_or_throw(path, "rb");
    char magic[8];
    std::uint64_t count = 0;
    if (std::fread(magic, 1, sizeof(magic), f.get()) != sizeof(magic) ||
        std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error(path + ": not an MDSEVT01 file");
    }
    if (std::fread(&count, sizeof(count), 1, f.get()) != 1) {
        throw std::runtime_error(path + ": truncated header");
    }
    std::vector<MarketEvent> events(count);
    if (count && std::fread(events.data(), sizeof(MarketEvent), count, f.get()) != count) {
        throw std::runtime_error(path + ": truncated body");
    }
    return events;
}

}  // namespace mds
