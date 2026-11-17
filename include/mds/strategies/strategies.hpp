// SPDX-License-Identifier: MIT
//
// Reference strategies used to exercise the backtester.
//
//  * InventoryMarketMaker - passive two-sided quoting around a reservation
//    price that is skewed against current inventory (a simplified
//    Avellaneda-Stoikov). Exercises the maker-side fill model: queue position,
//    cancel/replace latency, rebates, adverse selection.
//
//  * ImbalanceTaker - aggressive strategy that trades in the direction of
//    top-of-book order-flow imbalance. Exercises the taker-side fill model:
//    depth walking, latency slippage, taker fees.
#pragma once

#include <memory>
#include <string>

#include "mds/backtest/strategy.hpp"

namespace mds {

struct MarketMakerParams {
    int half_spread_ticks{1};
    Qty quote_size{100};
    Qty max_position{1'000};
    double skew_ticks_per_lot{0.5};  // reservation shift per `quote_size` of inventory
    Timestamp min_requote_ns{500 * kNanosPerMicro};  // throttle cancel/replace churn
};

class InventoryMarketMaker final : public Strategy {
public:
    explicit InventoryMarketMaker(MarketMakerParams p = {}) : p_(p) {}
    [[nodiscard]] std::string name() const override { return "inventory_market_maker"; }
    void on_market_data(StrategyContext& ctx, const OrderBook& book,
                        const MarketEvent& ev) override;
    void on_fill(StrategyContext& ctx, const ExecutionReport& rep) override;
    void on_end(StrategyContext& ctx) override { ctx.cancel_all(); }

private:
    struct Quote {
        OrderId id{0};
        Price price{0};
    };
    void requote(StrategyContext& ctx, Quote& q, Side side, Price px, bool allowed);

    MarketMakerParams p_;
    Quote bid_;
    Quote ask_;
    Timestamp last_quote_ts_{0};
};

struct ImbalanceParams {
    // Calibrated on the default synthetic feed: |imbalance(5)| has p50 ~0.18,
    // p90 ~0.50, p99 ~0.71, so 0.6 enters on roughly the strongest 7% of book
    // states. Lower thresholds fire so often that the strategy just churns
    // and pays the spread.
    std::size_t levels{5};
    double entry_threshold{0.6};
    double exit_threshold{0.2};
    Qty trade_size{100};
    Qty max_position{500};
    Timestamp cooldown_ns{2 * kNanosPerMilli};
    Timestamp max_hold_ns{2 * kNanosPerSecond};
};

class ImbalanceTaker final : public Strategy {
public:
    explicit ImbalanceTaker(ImbalanceParams p = {}) : p_(p) {}
    [[nodiscard]] std::string name() const override { return "imbalance_taker"; }
    void on_market_data(StrategyContext& ctx, const OrderBook& book,
                        const MarketEvent& ev) override;
    void on_fill(StrategyContext& ctx, const ExecutionReport& rep) override;

private:
    ImbalanceParams p_;
    Timestamp last_action_ts_{0};
    Timestamp position_opened_ts_{0};
};

// Factory used by the CLI: "mm" / "market_maker", "obi" / "imbalance".
std::unique_ptr<Strategy> make_strategy(const std::string& name);

}  // namespace mds
