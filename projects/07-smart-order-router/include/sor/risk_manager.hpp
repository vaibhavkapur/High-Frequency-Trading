#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>

#include "sor/types.hpp"

namespace sor {

struct RiskLimits {
    Quantity max_order_qty{50'000};
    double max_order_notional_usd{5'000'000.0};
    Quantity max_net_position_qty{100'000};
    Quantity max_gross_position_qty{200'000};
    double max_open_notional_usd{10'000'000.0};
    std::uint32_t max_orders_per_second{1'000};
    double max_price_collar_bps{500.0};  // Reject limit price deviating > 5% from reference mid
    bool kill_switch_active{false};
};

enum class RiskRejectReason : std::uint8_t {
    Approved = 0,
    KillSwitchActive = 1,
    InvalidOrder = 2,
    DuplicateClientOrderId = 3,
    MaxOrderQtyExceeded = 4,
    MaxOrderNotionalExceeded = 5,
    MaxNetPositionExceeded = 6,
    MaxGrossPositionExceeded = 7,
    MaxOpenNotionalExceeded = 8,
    OrderRateLimitExceeded = 9,
    PriceCollarBreached = 10,
};

[[nodiscard]] std::string_view to_string(RiskRejectReason r) noexcept;

struct RiskCheckResult {
    bool approved{false};
    RiskRejectReason reason{RiskRejectReason::Approved};
    std::string message{};
};

struct PreTradeOrderView {
    std::string client_order_id{};
    Side side{Side::Buy};
    Quantity qty{0};
    PriceTicks limit_price{0};
    double reference_mid_ticks{0.0};
    TimestampNs now_ns{0};
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits = {}) : limits_(limits) {}

    void set_limits(RiskLimits limits) noexcept { limits_ = limits; }
    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }
    void set_kill_switch(bool active) noexcept { limits_.kill_switch_active = active; }

    // Validate and (if approved) reserve risk capacity and record client_order_id.
    [[nodiscard]] RiskCheckResult check_and_reserve(const PreTradeOrderView& order);

    // Update position & open exposure on a confirmed fill.
    void on_fill(Side side, Quantity fill_qty, PriceTicks fill_price,
                 PriceTicks reserved_limit_price) noexcept;

    // Release remaining unexecuted reserved qty when a parent order terminates without full fill.
    void on_release(Side side, Quantity unreleased_qty, PriceTicks reserved_limit_price) noexcept;

    [[nodiscard]] Quantity net_position() const noexcept { return net_position_; }
    [[nodiscard]] Quantity open_buy_qty() const noexcept { return open_buy_qty_; }
    [[nodiscard]] Quantity open_sell_qty() const noexcept { return open_sell_qty_; }
    [[nodiscard]] double open_notional_usd() const noexcept { return open_notional_usd_; }
    [[nodiscard]] bool is_duplicate_client_id(const std::string& client_order_id) const;

    void reset();

private:
    RiskLimits limits_{};
    Quantity net_position_{0};
    Quantity open_buy_qty_{0};
    Quantity open_sell_qty_{0};
    double open_notional_usd_{0.0};
    std::deque<TimestampNs> recent_order_ts_{};
    std::unordered_set<std::string> seen_client_ids_{};
};

}  // namespace sor
