// SPDX-License-Identifier: MIT
#include "mds/strategies/strategies.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mds {

namespace {

Price floor_to_tick(double px_units, Price tick) {
    return static_cast<Price>(std::floor(px_units / static_cast<double>(tick))) * tick;
}
Price ceil_to_tick(double px_units, Price tick) {
    return static_cast<Price>(std::ceil(px_units / static_cast<double>(tick))) * tick;
}

}  // namespace

// ---------------------------------------------------------------------------
// InventoryMarketMaker
// ---------------------------------------------------------------------------

void InventoryMarketMaker::requote(StrategyContext& ctx, Quote& q, Side side, Price px,
                                   bool allowed) {
    const bool live = q.id != 0 && ctx.is_open(q.id);
    if (!live) q.id = 0;
    if (live && allowed && q.price == px) return;  // already where we want it
    if (live) {
        ctx.cancel(q.id);
        q.id = 0;
    }
    if (allowed) {
        q.id = ctx.send_limit(side, px, p_.quote_size);
        q.price = px;
    }
}

void InventoryMarketMaker::on_market_data(StrategyContext& ctx, const OrderBook& book,
                                          const MarketEvent&) {
    const auto bb = book.best_bid();
    const auto ba = book.best_ask();
    if (!bb || !ba) return;
    if (ctx.now() - last_quote_ts_ < p_.min_requote_ns) return;

    const Price tick = ctx.tick_size();
    const double mid = static_cast<double>(*bb + *ba) / 2.0;
    const double lots = static_cast<double>(ctx.position()) / static_cast<double>(p_.quote_size);
    const double reservation = mid - lots * p_.skew_ticks_per_lot * static_cast<double>(tick);
    const double half = p_.half_spread_ticks * static_cast<double>(tick);

    // Post-only: never cross the opposite touch.
    Price bid_px = std::min(floor_to_tick(reservation - half, tick), *ba - tick);
    Price ask_px = std::max(ceil_to_tick(reservation + half, tick), *bb + tick);
    if (bid_px >= ask_px) ask_px = bid_px + tick;

    const Qty pos = ctx.position();
    const bool before_bid = bid_.id != 0 && bid_.price == bid_px;
    const bool before_ask = ask_.id != 0 && ask_.price == ask_px;
    requote(ctx, bid_, Side::Buy, bid_px, pos + p_.quote_size <= p_.max_position);
    requote(ctx, ask_, Side::Sell, ask_px, pos - p_.quote_size >= -p_.max_position);
    if (!(before_bid && before_ask)) last_quote_ts_ = ctx.now();
}

void InventoryMarketMaker::on_fill(StrategyContext&, const ExecutionReport& rep) {
    if (rep.remaining != 0) return;
    if (rep.order_id == bid_.id) bid_.id = 0;
    if (rep.order_id == ask_.id) ask_.id = 0;
}

// ---------------------------------------------------------------------------
// ImbalanceTaker
// ---------------------------------------------------------------------------

void ImbalanceTaker::on_market_data(StrategyContext& ctx, const OrderBook& book,
                                    const MarketEvent&) {
    if (ctx.open_orders() != 0) return;  // one order in flight at a time
    const Timestamp now = ctx.now();
    if (now - last_action_ts_ < p_.cooldown_ns) return;

    const double imb = book.imbalance(p_.levels);
    const Qty pos = ctx.position();

    // Exit: signal faded / flipped, or held too long.
    if (pos != 0) {
        const bool faded = pos > 0 ? imb < p_.exit_threshold : imb > -p_.exit_threshold;
        const bool stale = now - position_opened_ts_ > p_.max_hold_ns;
        if (faded || stale) {
            ctx.send_market(pos > 0 ? Side::Sell : Side::Buy, pos > 0 ? pos : -pos);
            last_action_ts_ = now;
            return;
        }
    }
    // Entry / scale-in.
    if (imb > p_.entry_threshold && pos + p_.trade_size <= p_.max_position) {
        ctx.send_market(Side::Buy, p_.trade_size);
        last_action_ts_ = now;
    } else if (imb < -p_.entry_threshold && pos - p_.trade_size >= -p_.max_position) {
        ctx.send_market(Side::Sell, p_.trade_size);
        last_action_ts_ = now;
    }
}

void ImbalanceTaker::on_fill(StrategyContext& ctx, const ExecutionReport& rep) {
    const Qty pos = ctx.position();
    const Qty before = rep.side == Side::Buy ? pos - rep.qty : pos + rep.qty;
    if (before == 0 && pos != 0) position_opened_ts_ = rep.ts;
}

std::unique_ptr<Strategy> make_strategy(const std::string& name) {
    if (name == "mm" || name == "market_maker") return std::make_unique<InventoryMarketMaker>();
    if (name == "obi" || name == "imbalance") return std::make_unique<ImbalanceTaker>();
    throw std::invalid_argument("unknown strategy '" + name + "' (expected mm | obi)");
}

}  // namespace mds
