// SPDX-License-Identifier: MIT
//
// Position / cash / PnL accounting with average-cost realised PnL.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "mds/common/types.hpp"

namespace mds {

class Portfolio {
public:
    void on_fill(Side side, Price price, Qty qty, double fee) noexcept {
        const double px = to_double(price);
        const Qty signed_qty = side == Side::Buy ? qty : -qty;

        // Realise PnL on the part of the fill that reduces the open position.
        if (position_ != 0 && ((position_ > 0) != (signed_qty > 0))) {
            const Qty closing = std::min(std::llabs(signed_qty), std::llabs(position_));
            const double direction = position_ > 0 ? 1.0 : -1.0;
            realized_ += direction * static_cast<double>(closing) * (px - avg_cost_);
        }

        const Qty new_pos = position_ + signed_qty;
        if (new_pos == 0) {
            avg_cost_ = 0.0;
        } else if (position_ == 0 || (position_ > 0) != (new_pos > 0)) {
            avg_cost_ = px;  // opened fresh or flipped through zero
        } else if ((position_ > 0) == (signed_qty > 0)) {
            // Adding to the position: weighted average cost.
            avg_cost_ = (avg_cost_ * static_cast<double>(std::llabs(position_)) +
                         px * static_cast<double>(qty)) /
                        static_cast<double>(std::llabs(new_pos));
        }
        position_ = new_pos;

        cash_ -= static_cast<double>(signed_qty) * px;
        cash_ -= fee;
        fees_ += fee;
        volume_ += qty;
        notional_ += static_cast<double>(qty) * px;
        ++fills_;
        if (std::llabs(position_) > max_abs_position_) max_abs_position_ = std::llabs(position_);
    }

    [[nodiscard]] double equity(double mark) const noexcept {
        return cash_ + static_cast<double>(position_) * mark;
    }
    [[nodiscard]] double unrealized(double mark) const noexcept {
        return static_cast<double>(position_) * (mark - avg_cost_);
    }

    [[nodiscard]] Qty position() const noexcept { return position_; }
    [[nodiscard]] double cash() const noexcept { return cash_; }
    [[nodiscard]] double realized() const noexcept { return realized_; }
    [[nodiscard]] double avg_cost() const noexcept { return avg_cost_; }
    [[nodiscard]] double fees() const noexcept { return fees_; }
    [[nodiscard]] Qty volume() const noexcept { return volume_; }
    [[nodiscard]] double notional() const noexcept { return notional_; }
    [[nodiscard]] std::uint64_t fills() const noexcept { return fills_; }
    [[nodiscard]] Qty max_abs_position() const noexcept { return max_abs_position_; }

private:
    Qty position_{0};
    double cash_{0.0};
    double avg_cost_{0.0};
    double realized_{0.0};
    double fees_{0.0};
    Qty volume_{0};
    double notional_{0.0};
    std::uint64_t fills_{0};
    Qty max_abs_position_{0};
};

}  // namespace mds
