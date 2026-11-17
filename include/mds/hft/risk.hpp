// SPDX-License-Identifier: MIT
//
// Pre-trade risk gate - the checks every order must pass before it may leave
// the building (cf. SEC Rule 15c3-5, the "market access rule"). Designed for
// the hot path: no allocation, no locks, a handful of compares per order.
//
//   * max order size and max order notional ("fat finger")
//   * price collar: limit price within N ticks of the reference (mid) price
//   * max absolute position (current position + this order)
//   * max open orders
//   * order rate throttle: token bucket refilled from the TSC clock
//   * kill switch: trips automatically after too many rejects in a row, or
//     manually; once tripped every order is rejected until reset
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>

#include "mds/common/types.hpp"

namespace mds::hft {

enum class RiskReject : std::uint8_t {
    None = 0,
    KillSwitch,
    MaxOrderQty,
    MaxNotional,
    PriceCollar,
    MaxPosition,
    MaxOpenOrders,
    RateLimit,
    kCount,
};

inline const char* to_string(RiskReject r) noexcept {
    switch (r) {
        case RiskReject::None: return "accepted";
        case RiskReject::KillSwitch: return "kill_switch";
        case RiskReject::MaxOrderQty: return "max_order_qty";
        case RiskReject::MaxNotional: return "max_notional";
        case RiskReject::PriceCollar: return "price_collar";
        case RiskReject::MaxPosition: return "max_position";
        case RiskReject::MaxOpenOrders: return "max_open_orders";
        case RiskReject::RateLimit: return "rate_limit";
        case RiskReject::kCount: break;
    }
    return "?";
}

struct RiskLimits {
    Qty max_order_qty{5'000};
    double max_order_notional{1'000'000.0};  // currency units
    std::int64_t collar_ticks{50};           // |price - reference| <= collar
    Price tick{100};
    Qty max_position{10'000};
    std::uint32_t max_open_orders{200};
    double orders_per_second{20'000.0};     // token bucket refill rate
    double burst{500.0};                    // token bucket depth
    std::uint32_t kill_after_consecutive_rejects{1'000};
};

class RiskGate {
public:
    explicit RiskGate(RiskLimits limits = {}) : lim_(limits), tokens_(limits.burst) {}

    // `now_ns` drives the rate limiter; `reference` is usually the mid price.
    RiskReject check(Side side, Price price, Qty qty, Price reference, std::int64_t now_ns) noexcept {
        RiskReject r = evaluate(side, price, qty, reference, now_ns);
        ++counts_[static_cast<std::size_t>(r)];
        if (r == RiskReject::None) {
            consecutive_rejects_ = 0;
            ++open_orders_;
        } else if (r != RiskReject::KillSwitch && ++consecutive_rejects_ >= lim_.kill_after_consecutive_rejects) {
            killed_ = true;  // something upstream is badly wrong: stop sending
        }
        return r;
    }

    void on_fill(Side side, Qty qty) noexcept { position_ += side == Side::Buy ? qty : -qty; }
    void on_order_closed() noexcept {
        if (open_orders_) --open_orders_;
    }
    void kill() noexcept { killed_ = true; }
    void reset_kill_switch() noexcept {
        killed_ = false;
        consecutive_rejects_ = 0;
    }

    [[nodiscard]] bool killed() const noexcept { return killed_; }
    [[nodiscard]] Qty position() const noexcept { return position_; }
    [[nodiscard]] std::uint32_t open_orders() const noexcept { return open_orders_; }
    [[nodiscard]] std::uint64_t count(RiskReject r) const noexcept { return counts_[static_cast<std::size_t>(r)]; }
    [[nodiscard]] const RiskLimits& limits() const noexcept { return lim_; }

private:
    RiskReject evaluate(Side side, Price price, Qty qty, Price reference, std::int64_t now_ns) noexcept {
        if (killed_) return RiskReject::KillSwitch;
        if (qty <= 0 || qty > lim_.max_order_qty) return RiskReject::MaxOrderQty;
        if (to_double(price) * static_cast<double>(qty) > lim_.max_order_notional) return RiskReject::MaxNotional;
        if (reference > 0 && std::llabs(price - reference) > lim_.collar_ticks * lim_.tick) return RiskReject::PriceCollar;
        const Qty after = position_ + (side == Side::Buy ? qty : -qty);
        if (std::llabs(after) > lim_.max_position) return RiskReject::MaxPosition;
        if (open_orders_ >= lim_.max_open_orders) return RiskReject::MaxOpenOrders;
        // Token bucket.
        if (last_ns_ != 0) {
            tokens_ += static_cast<double>(now_ns - last_ns_) * 1e-9 * lim_.orders_per_second;
            if (tokens_ > lim_.burst) tokens_ = lim_.burst;
        }
        last_ns_ = now_ns;
        if (tokens_ < 1.0) return RiskReject::RateLimit;
        tokens_ -= 1.0;
        return RiskReject::None;
    }

    RiskLimits lim_;
    double tokens_;
    std::int64_t last_ns_{0};
    Qty position_{0};
    std::uint32_t open_orders_{0};
    std::uint32_t consecutive_rejects_{0};
    bool killed_{false};
    std::array<std::uint64_t, static_cast<std::size_t>(RiskReject::kCount)> counts_{};
};

}  // namespace mds::hft
