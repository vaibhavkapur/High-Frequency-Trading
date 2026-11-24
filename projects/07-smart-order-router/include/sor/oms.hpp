#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sor/audit_log.hpp"
#include "sor/consolidated_book.hpp"
#include "sor/risk_manager.hpp"
#include "sor/route_planner.hpp"
#include "sor/types.hpp"
#include "sor/venue_health.hpp"

namespace sor {

struct ParentOrderRequest {
    std::string client_order_id{};
    std::uint32_t symbol_id{0};
    Side side{Side::Buy};
    Quantity quantity{0};
    PriceTicks limit_price{0};
    TimeInForce tif{TimeInForce::IOC};
    TimestampNs timestamp_ns{0};
    double benchmark_mid_ticks{0.0};  // Optional true arrival mid for apples-to-apples IS reporting.
};

struct ParentOrder {
    OrderId parent_id{0};
    std::string client_order_id{};
    std::uint32_t symbol_id{0};
    Side side{Side::Buy};
    TimeInForce tif{TimeInForce::IOC};
    Quantity orig_qty{0};
    Quantity filled_qty{0};
    Quantity leaves_qty{0};
    Quantity in_flight_qty{0};
    PriceTicks limit_price{0};
    double arrival_mid_ticks{0.0};
    TimestampNs arrival_ts_ns{0};
    TimestampNs completion_ts_ns{0};
    ParentOrderState state{ParentOrderState::New};
    RiskRejectReason risk_reason{RiskRejectReason::Approved};
    std::uint32_t reroute_attempts{0};
    std::uint32_t failed_venues_mask{0};
    bool cancel_requested{false};
    double total_notional_usd{0.0};
    double total_fees_usd{0.0};
    std::vector<OrderId> child_ids{};

    [[nodiscard]] double avg_fill_price_usd() const noexcept {
        return filled_qty > 0 ? (total_notional_usd / static_cast<double>(filled_qty)) : 0.0;
    }

    // Net implementation shortfall in bps vs arrival consolidated mid (positive = worse than mid).
    [[nodiscard]] double net_shortfall_bps() const noexcept {
        if (filled_qty <= 0 || arrival_mid_ticks <= 0.0) return 0.0;
        const double mid_usd = arrival_mid_ticks / static_cast<double>(kPriceScale);
        const double avg_px = avg_fill_price_usd();
        const double fee_per_share = total_fees_usd / static_cast<double>(filled_qty);
        const double signed_slip = (side == Side::Buy)
                                       ? ((avg_px + fee_per_share) - mid_usd)
                                       : (mid_usd - (avg_px - fee_per_share));
        return (signed_slip / mid_usd) * 10'000.0;
    }

    [[nodiscard]] double gross_shortfall_bps() const noexcept {
        if (filled_qty <= 0 || arrival_mid_ticks <= 0.0) return 0.0;
        const double mid_usd = arrival_mid_ticks / static_cast<double>(kPriceScale);
        const double avg_px = avg_fill_price_usd();
        const double signed_slip = (side == Side::Buy) ? (avg_px - mid_usd) : (mid_usd - avg_px);
        return (signed_slip / mid_usd) * 10'000.0;
    }
};

struct ChildOrder {
    OrderId child_id{0};
    OrderId parent_id{0};
    VenueId venue_id{kInvalidVenue};
    Side side{Side::Buy};
    TimeInForce tif{TimeInForce::IOC};
    Quantity orig_qty{0};
    Quantity filled_qty{0};
    Quantity leaves_qty{0};
    PriceTicks limit_price{0};
    TimestampNs sent_ts_ns{0};
    TimestampNs ack_ts_ns{0};
    TimestampNs last_update_ts_ns{0};
    ChildOrderState state{ChildOrderState::PendingNew};
    Quantity pending_replace_qty{0};
    PriceTicks pending_replace_price{0};
    double total_notional_usd{0.0};
    double total_fees_usd{0.0};
    CostBreakdown expected_cost{};
};

enum class ChildCommandType : std::uint8_t {
    NewOrder = 0,
    CancelOrder = 1,
    ReplaceOrder = 2,
};

struct ChildCommand {
    ChildCommandType type{ChildCommandType::NewOrder};
    OrderId child_id{0};
    OrderId parent_id{0};
    VenueId venue_id{kInvalidVenue};
    Side side{Side::Buy};
    TimeInForce tif{TimeInForce::IOC};
    Quantity quantity{0};
    PriceTicks limit_price{0};
    TimestampNs timestamp_ns{0};
};

enum class ExecReportType : std::uint8_t {
    Ack = 0,
    Fill = 1,
    CancelAck = 2,
    ReplaceAck = 3,
    Reject = 4,
    Timeout = 5,
};

struct ExecutionReport {
    ExecReportType type{ExecReportType::Ack};
    OrderId child_id{0};
    VenueId venue_id{kInvalidVenue};
    ExecId exec_id{0};  // Must be unique per fill; duplicates are ignored.
    Quantity fill_qty{0};
    PriceTicks fill_price{0};
    double fee_usd{0.0};
    TimestampNs timestamp_ns{0};
    std::string reason{};
};

struct OmsStats {
    std::uint64_t parents_submitted{0};
    std::uint64_t parents_risk_rejected{0};
    std::uint64_t parents_filled{0};
    std::uint64_t parents_partial{0};
    std::uint64_t parents_unfilled{0};
    std::uint64_t children_sent{0};
    std::uint64_t children_acked{0};
    std::uint64_t children_filled{0};
    std::uint64_t children_cancelled{0};
    std::uint64_t children_replaced{0};
    std::uint64_t children_rejected{0};
    std::uint64_t children_timed_out{0};
    std::uint64_t duplicate_execs_ignored{0};
    std::uint64_t failovers_triggered{0};
};

class OrderManagementSystem {
public:
    OrderManagementSystem(ConsolidatedBook& book, RoutePlanner& planner, RiskManager& risk,
                          VenueHealthMonitor& health, AuditLog& audit);

    // Submit a parent order: runs pre-trade risk, plans route, emits outbound child commands.
    OrderId submit_parent(const ParentOrderRequest& req, std::vector<ChildCommand>& out_commands);

    // Process an inbound venue execution report. May emit failover child commands.
    void on_execution_report(const ExecutionReport& report,
                             std::vector<ChildCommand>& out_commands);

    // Cancel an entire parent order (cancels all active children).
    bool cancel_parent(OrderId parent_id, TimestampNs now_ns,
                       std::vector<ChildCommand>& out_commands);

    // Cancel or cancel/replace a specific child order.
    bool cancel_child(OrderId child_id, TimestampNs now_ns,
                      std::vector<ChildCommand>& out_commands);
    bool replace_child(OrderId child_id, Quantity new_qty, PriceTicks new_price,
                       TimestampNs now_ns, std::vector<ChildCommand>& out_commands);

    // Handle venue outage: trips circuit breaker, cancels active children on that venue,
    // and triggers failover for affected parents.
    void on_venue_down(VenueId vid, TimestampNs now_ns, std::vector<ChildCommand>& out_commands);

    [[nodiscard]] const ParentOrder* get_parent(OrderId parent_id) const noexcept;
    [[nodiscard]] const ChildOrder* get_child(OrderId child_id) const noexcept;
    [[nodiscard]] const std::unordered_map<OrderId, ParentOrder>& parents() const noexcept {
        return parents_;
    }
    [[nodiscard]] const std::unordered_map<OrderId, ChildOrder>& children() const noexcept {
        return children_;
    }
    [[nodiscard]] const OmsStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::array<std::uint32_t, kMaxVenues>& active_children_per_venue()
        const noexcept {
        return active_children_per_venue_;
    }

    void reset();

private:
    void route_remaining(ParentOrder& parent, TimestampNs now_ns,
                         std::vector<ChildCommand>& out_commands);
    void finalize_parent(ParentOrder& parent, ParentOrderState final_state, TimestampNs now_ns);
    void decrement_active_child(VenueId vid) noexcept;

    ConsolidatedBook& book_;
    RoutePlanner& planner_;
    RiskManager& risk_;
    VenueHealthMonitor& health_;
    AuditLog& audit_;

    OrderId next_parent_id_{1};
    OrderId next_child_id_{1};
    std::unordered_map<OrderId, ParentOrder> parents_{};
    std::unordered_map<OrderId, ChildOrder> children_{};
    std::unordered_set<ExecId> seen_exec_ids_{};
    std::array<std::uint32_t, kMaxVenues> active_children_per_venue_{};
    OmsStats stats_{};
};

}  // namespace sor
