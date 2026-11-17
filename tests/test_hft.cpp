// SPDX-License-Identifier: MIT
#include <cstring>
#include <string>

#include "mds/hft/ouch.hpp"
#include "mds/hft/risk.hpp"
#include "test_framework.hpp"

using namespace mds;
using namespace mds::hft;

TEST(risk, accepts_within_limits_and_counts) {
    RiskGate g;
    CHECK(g.check(Side::Buy, 1'000'000, 100, 1'000'000, 1) == RiskReject::None);
    CHECK_EQ(g.count(RiskReject::None), 1u);
    CHECK_EQ(g.open_orders(), 1u);
}

TEST(risk, rejects_each_limit) {
    RiskLimits l;
    l.max_order_qty = 1000;
    l.max_order_notional = 50'000;
    l.collar_ticks = 10;
    l.max_position = 500;
    l.max_open_orders = 1000;
    RiskGate g(l);
    const Price px = 1'000'000;  // $100
    CHECK(g.check(Side::Buy, px, 2000, px, 1) == RiskReject::MaxOrderQty);
    CHECK(g.check(Side::Buy, px, 600, px, 2) == RiskReject::MaxNotional);   // $60k
    CHECK(g.check(Side::Buy, px + 11 * 100, 100, px, 3) == RiskReject::PriceCollar);
    g.on_fill(Side::Buy, 450);
    CHECK(g.check(Side::Buy, px, 100, px, 4) == RiskReject::MaxPosition);
    CHECK(g.check(Side::Sell, px, 100, px, 5) == RiskReject::None);       // reduces position
}

TEST(risk, token_bucket_rate_limit) {
    RiskLimits l;
    l.orders_per_second = 1000;  // 1 per ms
    l.burst = 5;
    l.max_open_orders = 1'000'000;
    RiskGate g(l);
    int accepted = 0;
    for (int i = 0; i < 20; ++i) accepted += g.check(Side::Buy, 1'000'000, 1, 0, 1'000'000) == RiskReject::None;
    CHECK_EQ(accepted, 5);  // burst only
    CHECK(g.check(Side::Buy, 1'000'000, 1, 0, 1'000'000 + 2'000'000) == RiskReject::None);  // 2 ms later: refilled
}

TEST(risk, kill_switch_trips_after_consecutive_rejects) {
    RiskLimits l;
    l.kill_after_consecutive_rejects = 3;
    l.max_order_qty = 10;
    RiskGate g(l);
    for (int i = 0; i < 3; ++i) g.check(Side::Buy, 1'000'000, 100, 0, i);
    CHECK(g.killed());
    CHECK(g.check(Side::Buy, 1'000'000, 1, 0, 9) == RiskReject::KillSwitch);
    g.reset_kill_switch();
    CHECK(g.check(Side::Buy, 1'000'000, 1, 0, 10) == RiskReject::None);
}

TEST(ouch, enter_and_cancel_layout) {
    std::uint8_t buf[64];
    CHECK_EQ(ouch::enter_order(buf, 35, Side::Sell, 300, "AAPL", 1'503'400, ouch::kTifDay), 49u);
    CHECK_EQ(buf[0], 'O');
    CHECK_EQ(std::string(reinterpret_cast<char*>(buf + 1), 14), std::string("MDS0000000000Z"));
    CHECK_EQ(buf[15], 'S');
    CHECK_EQ(itch::be32(buf + 16), 300u);
    CHECK_EQ(std::string(reinterpret_cast<char*>(buf + 20), 8), std::string("AAPL    "));
    CHECK_EQ(itch::be32(buf + 28), 1'503'400u);
    CHECK_EQ(itch::be32(buf + 32), 99'999u);
    CHECK_EQ(buf[48], 'R');
    CHECK_EQ(ouch::cancel_order(buf, 35, 0), 19u);
    CHECK_EQ(buf[0], 'X');
    CHECK_EQ(itch::be32(buf + 15), 0u);
}
