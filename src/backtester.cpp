// SPDX-License-Identifier: MIT
#include "mds/backtest/backtester.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "mds/common/clock.hpp"

namespace mds {

namespace {

double fee_for(Price price, Qty qty, double bps) {
    return to_double(price) * static_cast<double>(qty) * bps / 10'000.0;
}

bool crosses(Side aggressor, Price aggressor_limit, Price resting) {
    return aggressor == Side::Buy ? aggressor_limit >= resting : aggressor_limit <= resting;
}

}  // namespace

Backtester::Backtester(BacktestConfig cfg) : cfg_(cfg) {
    if (cfg_.equity_sample_interval_ns <= 0) cfg_.equity_sample_interval_ns = kNanosPerSecond;
    if (cfg_.tick_size <= 0) throw std::invalid_argument("tick_size must be > 0");
    scratch_ids_.reserve(64);
    scratch_fills_.reserve(256);
}

void Backtester::reset() {
    book_.clear();
    portfolio_ = Portfolio{};
    open_.clear();
    next_id_ = 1ULL << 62;
    now_ = 0;
    next_sample_ts_ = 0;
    last_mid_ = 0.0;
    trading_enabled_ = false;
    result_ = BacktestResult{};
}

// ---------------------------------------------------------------------------
// StrategyContext
// ---------------------------------------------------------------------------

OrderId Backtester::send_limit(Side side, Price price, Qty qty) {
    if (qty <= 0 || price <= 0) return 0;
    const OrderId id = next_id_++;
    SimOrder o;
    o.id = id;
    o.side = side;
    o.price = price;
    o.qty = o.remaining = qty;
    o.arrive_ts = now_ + cfg_.order_latency_ns;
    open_.emplace(id, o);
    ++result_.orders_sent;
    return id;
}

OrderId Backtester::send_market(Side side, Qty qty) {
    if (qty <= 0) return 0;
    const OrderId id = next_id_++;
    SimOrder o;
    o.id = id;
    o.side = side;
    o.price = kNoPrice;
    o.qty = o.remaining = qty;
    o.arrive_ts = now_ + cfg_.order_latency_ns;
    open_.emplace(id, o);
    ++result_.orders_sent;
    return id;
}

void Backtester::cancel(OrderId id) {
    auto it = open_.find(id);
    if (it == open_.end() || it->second.state == OrderState::Done) return;
    if (it->second.cancel_ts == 0) {
        it->second.cancel_ts = now_ + cfg_.cancel_latency_ns;
        ++result_.cancels_sent;
    }
}

void Backtester::cancel_all() {
    for (auto& [id, o] : open_) {
        if (o.state != OrderState::Done && o.cancel_ts == 0) {
            o.cancel_ts = now_ + cfg_.cancel_latency_ns;
            ++result_.cancels_sent;
        }
    }
}

// ---------------------------------------------------------------------------
// Simulated exchange
// ---------------------------------------------------------------------------

void Backtester::fill(SimOrder& o, Price price, Qty qty, bool maker, Strategy& strategy) {
    if (qty <= 0) return;
    const double fee = fee_for(price, qty, maker ? cfg_.maker_fee_bps : cfg_.taker_fee_bps);
    portfolio_.on_fill(o.side, price, qty, fee);
    o.remaining -= qty;
    if (o.remaining == 0) o.state = OrderState::Done;

    ExecutionReport rep;
    rep.order_id = o.id;
    rep.side = o.side;
    rep.price = price;
    rep.qty = qty;
    rep.remaining = o.remaining;
    rep.maker = maker;
    rep.fee = fee;
    rep.ts = now_;
    result_.fills.push_back(rep);
    strategy.on_fill(*this, rep);  // may send / cancel; never mutates book_
}

void Backtester::on_order_arrival(SimOrder& o, Strategy& strategy) {
    o.state = OrderState::Working;

    // Take liquidity first: walk displayed depth on the opposite side.
    const Price limit = o.price == kNoPrice ? (o.side == Side::Buy ? kMaxPrice : kMinPrice)
                                            : o.price;
    auto take = [&](const auto& levels) {
        for (const auto& [px, level] : levels) {
            if (o.remaining == 0 || !crosses(o.side, limit, px)) break;
            fill(o, px, std::min(o.remaining, level.total_qty), /*maker=*/false, strategy);
        }
    };
    if (o.side == Side::Buy) {
        take(book_.asks());
    } else {
        take(book_.bids());
    }

    if (o.remaining == 0) return;
    if (o.price == kNoPrice) {
        o.state = OrderState::Done;  // market orders are IOC: unfilled rest is cancelled
        return;
    }
    // Rest at the back of the queue.
    o.queue_ahead = book_.volume_at(o.side, o.price);
    o.joined_seq = book_.next_sequence();
}

void Backtester::release_due_actions(Strategy& strategy) {
    scratch_ids_.clear();
    for (const auto& [id, o] : open_) {
        const bool arrival_due = o.state == OrderState::PendingNew && o.arrive_ts <= now_;
        const bool cancel_due = o.cancel_ts != 0 && o.cancel_ts <= now_;
        if (arrival_due || cancel_due) scratch_ids_.push_back(id);
    }
    // unordered_map iteration order is unspecified: sort for determinism.
    std::sort(scratch_ids_.begin(), scratch_ids_.end());

    for (OrderId id : scratch_ids_) {
        auto it = open_.find(id);
        if (it == open_.end()) continue;
        SimOrder& o = it->second;
        if (o.state == OrderState::PendingNew && o.arrive_ts <= now_) {
            if (o.cancel_ts != 0 && o.cancel_ts <= o.arrive_ts) {
                o.state = OrderState::Done;  // cancel overtook the order in flight
                continue;
            }
            on_order_arrival(o, strategy);
        }
        if (o.state == OrderState::Working && o.cancel_ts != 0 && o.cancel_ts <= now_) {
            o.state = OrderState::Done;
        }
    }
    sweep_done();
}

void Backtester::pre_event_queue_update(const MarketEvent& ev, Strategy& strategy) {
    if (open_.empty()) return;

    if (ev.type == EventType::Cancel || ev.type == EventType::Modify) {
        const Order* m = book_.find(ev.id);
        if (m == nullptr) return;
        Qty leaves_queue = m->qty;  // cancel, or size-up (moves behind us)
        if (ev.type == EventType::Modify && ev.qty > 0 && ev.qty < m->qty) {
            leaves_queue = m->qty - ev.qty;  // size-down keeps priority
        }
        for (auto& [id, o] : open_) {
            if (o.state != OrderState::Working || o.side != m->side || o.price != m->price) {
                continue;
            }
            if (m->seq < o.joined_seq) {  // it was ahead of us
                o.queue_ahead -= std::min(o.queue_ahead, leaves_queue);
            }
        }
        return;
    }

    // Aggressive flow. Walk the aggressor's quantity through the price-time
    // queue as the matching engine would - better-priced liquidity first, then
    // the queue ahead of us at our price, then us - and compute how much of it
    // reaches each simulated order. Doing this BEFORE the event is applied
    // means the real book still shows exactly the liquidity the aggressor saw.
    //
    // This single rule covers every case: trades that only consume queue ahead
    // of us, aggressors that exhaust the displayed liquidity and reach us,
    // sweeps that trade through our price, and orders that hit us because our
    // quote improved the displayed best.
    if (ev.type != EventType::Add && ev.type != EventType::Market) return;
    const Side aggressor = ev.side;
    const Side our_side = opposite(aggressor);
    const Price limit = ev.type == EventType::Market
                            ? (aggressor == Side::Buy ? kMaxPrice : kMinPrice)
                            : ev.price;

    scratch_ids_.clear();
    for (const auto& [id, o] : open_) {
        if (o.state == OrderState::Working && o.side == our_side && o.remaining > 0 &&
            o.price != kNoPrice && crosses(aggressor, limit, o.price)) {
            scratch_ids_.push_back(id);
        }
    }
    if (scratch_ids_.empty()) return;
    // Price priority among our own orders (best first), id as a stable tiebreak.
    std::sort(scratch_ids_.begin(), scratch_ids_.end(), [&](OrderId a, OrderId b) {
        const SimOrder& x = open_.at(a);
        const SimOrder& y = open_.at(b);
        if (x.price != y.price) return our_side == Side::Buy ? x.price > y.price : x.price < y.price;
        return a < b;
    });

    // Displayed quantity strictly better than `price` on our side.
    auto better_liquidity = [&](Price price) {
        Qty total = 0;
        auto walk = [&](const auto& levels) {
            for (const auto& [lvl_px, level] : levels) {
                const bool better = our_side == Side::Buy ? lvl_px > price : lvl_px < price;
                if (!better) break;
                total += level.total_qty;
            }
        };
        if (our_side == Side::Buy) {
            walk(book_.bids());
        } else {
            walk(book_.asks());
        }
        return total;
    };

    Qty taken_by_us = 0;
    for (OrderId id : scratch_ids_) {
        auto it = open_.find(id);
        if (it == open_.end()) continue;
        SimOrder& o = it->second;
        const Qty ahead = std::min(o.queue_ahead, book_.volume_at(o.side, o.price));
        const Qty reaching = ev.qty - better_liquidity(o.price) - taken_by_us;
        if (reaching <= 0) continue;
        if (reaching <= ahead) {
            o.queue_ahead = ahead - reaching;  // trades consume the queue ahead of us
            continue;
        }
        o.queue_ahead = 0;
        const Qty q = std::min(reaching - ahead, o.remaining);
        taken_by_us += q;
        fill(o, o.price, q, /*maker=*/true, strategy);
    }
}

void Backtester::sweep_done() {
    for (auto it = open_.begin(); it != open_.end();) {
        if (it->second.state == OrderState::Done) {
            it = open_.erase(it);
        } else {
            ++it;
        }
    }
}

void Backtester::sample_equity(Timestamp ts) {
    if (auto mid = book_.mid_price()) last_mid_ = *mid;
    while (ts >= next_sample_ts_) {
        result_.equity_curve.push_back(
            EquityPoint{next_sample_ts_, portfolio_.equity(last_mid_), last_mid_,
                        portfolio_.position()});
        next_sample_ts_ += cfg_.equity_sample_interval_ns;
    }
}

BacktestResult Backtester::run(const std::vector<MarketEvent>& events, Strategy& strategy) {
    reset();
    result_.strategy = strategy.name();
    result_.fills.reserve(4096);
    if (events.empty()) return std::move(result_);

    const auto wall0 = mono_ns();
    result_.sim_start = events.front().ts;
    const std::size_t warmup = std::min(cfg_.warmup_events, events.size() - 1);

    for (std::size_t i = 0; i < events.size(); ++i) {
        const MarketEvent& ev = events[i];
        now_ = ev.ts;

        if (trading_enabled_) {
            release_due_actions(strategy);
            pre_event_queue_update(ev, strategy);
        }

        scratch_fills_.clear();
        book_.apply(ev, scratch_fills_);

        if (trading_enabled_) {
            sweep_done();
            strategy.on_market_data(*this, book_, ev);
            sample_equity(ev.ts);
        } else if (i >= warmup) {
            trading_enabled_ = true;
            next_sample_ts_ = ev.ts;
            sample_equity(ev.ts);
            strategy.on_start(*this);
        }
    }

    strategy.on_end(*this);
    if (auto mid = book_.mid_price()) last_mid_ = *mid;
    result_.sim_end = events.back().ts;
    result_.equity_curve.push_back(EquityPoint{result_.sim_end, portfolio_.equity(last_mid_),
                                               last_mid_, portfolio_.position()});

    const auto wall1 = mono_ns();
    result_.events = events.size();
    result_.wall_seconds = static_cast<double>(wall1 - wall0) / 1e9;
    result_.events_per_sec =
        result_.wall_seconds > 0 ? static_cast<double>(result_.events) / result_.wall_seconds : 0;

    PerformanceMetrics& m = result_.metrics;
    m = compute_metrics(result_.equity_curve,
                        static_cast<double>(cfg_.equity_sample_interval_ns) / 1e9);
    m.realized_pnl = portfolio_.realized();
    m.unrealized_pnl = portfolio_.unrealized(last_mid_);
    m.fees = portfolio_.fees();
    m.fills = portfolio_.fills();
    m.volume = portfolio_.volume();
    m.max_abs_position = portfolio_.max_abs_position();
    m.pnl_per_share = m.volume ? m.total_pnl / static_cast<double>(m.volume) : 0.0;
    return std::move(result_);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::string BacktestResult::to_json() const {
    char buf[2048];
    const auto& m = metrics;
    std::snprintf(
        buf, sizeof(buf),
        "{\n"
        "  \"strategy\": \"%s\",\n"
        "  \"events\": %" PRIu64 ",\n"
        "  \"wall_seconds\": %.6f,\n"
        "  \"events_per_sec\": %.1f,\n"
        "  \"sim_start_ns\": %" PRId64 ",\n"
        "  \"sim_end_ns\": %" PRId64 ",\n"
        "  \"orders_sent\": %" PRIu64 ",\n"
        "  \"cancels_sent\": %" PRIu64 ",\n"
        "  \"metrics\": {\n"
        "    \"total_pnl\": %.4f,\n"
        "    \"realized_pnl\": %.4f,\n"
        "    \"unrealized_pnl\": %.4f,\n"
        "    \"fees\": %.4f,\n"
        "    \"sharpe_annualized\": %.4f,\n"
        "    \"sortino_annualized\": %.4f,\n"
        "    \"max_drawdown\": %.4f,\n"
        "    \"max_drawdown_duration_s\": %.3f,\n"
        "    \"win_rate\": %.4f,\n"
        "    \"pnl_per_share\": %.6f,\n"
        "    \"fills\": %" PRIu64 ",\n"
        "    \"volume\": %" PRId64 ",\n"
        "    \"max_abs_position\": %" PRId64 ",\n"
        "    \"equity_samples\": %zu\n"
        "  }\n"
        "}\n",
        strategy.c_str(), static_cast<std::uint64_t>(events), wall_seconds, events_per_sec,
        static_cast<std::int64_t>(sim_start), static_cast<std::int64_t>(sim_end),
        static_cast<std::uint64_t>(orders_sent), static_cast<std::uint64_t>(cancels_sent),
        m.total_pnl, m.realized_pnl, m.unrealized_pnl, m.fees, m.sharpe_annualized,
        m.sortino_annualized, m.max_drawdown, m.max_drawdown_duration_s, m.win_rate,
        m.pnl_per_share, static_cast<std::uint64_t>(m.fills), static_cast<std::int64_t>(m.volume),
        static_cast<std::int64_t>(m.max_abs_position), m.samples);
    return buf;
}

namespace {
struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f) std::fclose(f);
    }
};
}  // namespace

void BacktestResult::write_equity_csv(const std::string& path) const {
    std::unique_ptr<std::FILE, FileCloser> f(std::fopen(path.c_str(), "w"));
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fputs("ts,equity,mid,position\n", f.get());
    for (const auto& p : equity_curve) {
        std::fprintf(f.get(), "%" PRId64 ",%.6f,%.6f,%" PRId64 "\n",
                     static_cast<std::int64_t>(p.ts), p.equity, p.mid,
                     static_cast<std::int64_t>(p.position));
    }
}

void BacktestResult::write_fills_csv(const std::string& path) const {
    std::unique_ptr<std::FILE, FileCloser> f(std::fopen(path.c_str(), "w"));
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fputs("ts,order_id,side,price,qty,remaining,maker,fee\n", f.get());
    for (const auto& r : fills) {
        std::fprintf(f.get(), "%" PRId64 ",%" PRIu64 ",%s,%.4f,%" PRId64 ",%" PRId64 ",%d,%.6f\n",
                     static_cast<std::int64_t>(r.ts), static_cast<std::uint64_t>(r.order_id),
                     std::string(to_string(r.side)).c_str(), to_double(r.price),
                     static_cast<std::int64_t>(r.qty), static_cast<std::int64_t>(r.remaining),
                     r.maker ? 1 : 0, r.fee);
    }
}

}  // namespace mds
