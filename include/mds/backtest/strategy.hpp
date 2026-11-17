// SPDX-License-Identifier: MIT
//
// Strategy API for the event-driven backtester.
//
// A Strategy reacts to market-data events and talks to the simulated exchange
// only through StrategyContext. Orders it sends are NOT acted on immediately:
// they are delayed by the configured order-entry latency, exactly as a real
// order would be in flight to the exchange. The strategy therefore cannot
// trade on information it would not have had.
#pragma once

#include <cstddef>
#include <string>

#include "mds/common/event.hpp"
#include "mds/common/types.hpp"
#include "mds/orderbook/order_book.hpp"

namespace mds {

struct ExecutionReport {
    OrderId order_id{0};
    Side side{Side::Buy};
    Price price{0};
    Qty qty{0};        // this fill
    Qty remaining{0};  // still working after this fill (0 == order done)
    bool maker{false};
    double fee{0.0};   // currency units; negative == rebate received
    Timestamp ts{0};
};

class StrategyContext {
public:
    virtual ~StrategyContext() = default;

    // Order entry. Returns the strategy-side order id immediately; the order
    // reaches the matching engine `latency_ns` later.
    virtual OrderId send_limit(Side side, Price price, Qty qty) = 0;
    virtual OrderId send_market(Side side, Qty qty) = 0;
    virtual void cancel(OrderId id) = 0;
    virtual void cancel_all() = 0;

    // State visible to the strategy (its own, not the exchange's).
    [[nodiscard]] virtual Qty position() const = 0;
    [[nodiscard]] virtual double cash() const = 0;
    [[nodiscard]] virtual Timestamp now() const = 0;
    [[nodiscard]] virtual std::size_t open_orders() const = 0;  // pending + working
    [[nodiscard]] virtual bool is_open(OrderId id) const = 0;
    [[nodiscard]] virtual Price tick_size() const = 0;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    [[nodiscard]] virtual std::string name() const = 0;

    virtual void on_start(StrategyContext&) {}
    // Called after every market event has been applied to the book.
    virtual void on_market_data(StrategyContext& ctx, const OrderBook& book,
                                const MarketEvent& ev) = 0;
    virtual void on_fill(StrategyContext&, const ExecutionReport&) {}
    virtual void on_end(StrategyContext&) {}
};

}  // namespace mds
