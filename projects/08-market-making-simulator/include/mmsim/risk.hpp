#pragma once

#include "mmsim/config.hpp"
#include "mmsim/types.hpp"

#include <array>
#include <cstdint>

namespace mmsim {

struct RiskState {
    Qty confirmed_position = 0;
    Qty open_buy_qty = 0;  // PendingNew + Live + PendingCancel buy quantity
    Qty open_sell_qty = 0; // PendingNew + Live + PendingCancel sell quantity
    int open_order_count = 0;
    double mark_price_dollars = 0.0;
    double current_net_equity_dollars = 0.0;
};

struct RiskStats {
    uint64_t checked_orders = 0;
    uint64_t approved_orders = 0;
    uint64_t rejected_orders = 0;
    std::array<uint64_t, 10> rejects_by_reason{};
    bool drawdown_kill_triggered = false;
    TimestampNs drawdown_kill_time_ns = 0;
};

class RiskManager {
public:
    explicit RiskManager(const RiskConfig& cfg);

    void reset(double initial_equity_dollars);

    // Updates peak equity and checks drawdown kill-switch. Returns true if kill-switch is active.
    bool update_equity(TimestampNs now_ns, double current_net_equity_dollars);

    [[nodiscard]] bool kill_switch_active() const noexcept { return kill_triggered_; }

    [[nodiscard]] RejectReason check_order(const OrderRequest& req, const RiskState& state);

    [[nodiscard]] const RiskStats& stats() const noexcept { return stats_; }

private:
    RiskConfig cfg_{};
    RiskStats stats_{};
    double peak_equity_dollars_ = 0.0;
    bool kill_triggered_ = false;
};

} // namespace mmsim
