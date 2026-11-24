#include "sor/oms.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace sor {

OrderManagementSystem::OrderManagementSystem(ConsolidatedBook& book, RoutePlanner& planner,
                                             RiskManager& risk, VenueHealthMonitor& health,
                                             AuditLog& audit)
    : book_(book), planner_(planner), risk_(risk), health_(health), audit_(audit) {}

void OrderManagementSystem::reset() {
    next_parent_id_ = 1;
    next_child_id_ = 1;
    parents_.clear();
    children_.clear();
    seen_exec_ids_.clear();
    active_children_per_venue_.fill(0);
    stats_ = OmsStats{};
}

const ParentOrder* OrderManagementSystem::get_parent(OrderId parent_id) const noexcept {
    auto it = parents_.find(parent_id);
    return it == parents_.end() ? nullptr : &it->second;
}

const ChildOrder* OrderManagementSystem::get_child(OrderId child_id) const noexcept {
    auto it = children_.find(child_id);
    return it == children_.end() ? nullptr : &it->second;
}

void OrderManagementSystem::decrement_active_child(VenueId vid) noexcept {
    if (vid < kMaxVenues && active_children_per_venue_[vid] > 0) {
        --active_children_per_venue_[vid];
    }
}

OrderId OrderManagementSystem::submit_parent(const ParentOrderRequest& req,
                                             std::vector<ChildCommand>& out_commands) {
    const OrderId pid = next_parent_id_++;
    ++stats_.parents_submitted;

    const auto bbo = book_.consolidated_bbo(req.timestamp_ns, &health_, true, true);
    const double arrival_mid =
        req.benchmark_mid_ticks > 0.0 ? req.benchmark_mid_ticks : bbo.mid_price_ticks();

    ParentOrder parent{};
    parent.parent_id = pid;
    parent.client_order_id = req.client_order_id;
    parent.symbol_id = req.symbol_id;
    parent.side = req.side;
    parent.tif = req.tif;
    parent.orig_qty = req.quantity;
    parent.leaves_qty = req.quantity;
    parent.limit_price = req.limit_price;
    parent.arrival_mid_ticks = arrival_mid;
    parent.arrival_ts_ns = req.timestamp_ns;
    parent.state = ParentOrderState::New;

    if (audit_.enabled()) {
        std::ostringstream oss;
        oss << to_string(req.side) << " qty=" << req.quantity << " limit=$" << std::fixed
            << std::setprecision(4) << ticks_to_usd(req.limit_price)
            << " tif=" << to_string(req.tif);
        audit_.record(req.timestamp_ns, AuditEventType::ParentSubmitted, pid, 0, kInvalidVenue,
                      req.client_order_id, oss.str());
    }

    PreTradeOrderView view{};
    view.client_order_id = req.client_order_id;
    view.side = req.side;
    view.qty = req.quantity;
    view.limit_price = req.limit_price;
    view.reference_mid_ticks = arrival_mid;
    view.now_ns = req.timestamp_ns;

    const RiskCheckResult risk_res = risk_.check_and_reserve(view);
    parent.risk_reason = risk_res.reason;

    if (!risk_res.approved) {
        ++stats_.parents_risk_rejected;
        parent.state = ParentOrderState::Rejected;
        parent.leaves_qty = 0;
        parent.completion_ts_ns = req.timestamp_ns;
        audit_.record(req.timestamp_ns, AuditEventType::RiskRejected, pid, 0, kInvalidVenue,
                      req.client_order_id,
                      std::string(to_string(risk_res.reason)) + ": " + risk_res.message);
        parents_.emplace(pid, std::move(parent));
        return pid;
    }

    audit_.record(req.timestamp_ns, AuditEventType::RiskApproved, pid, 0, kInvalidVenue,
                  req.client_order_id, "approved");

    parent.state = ParentOrderState::Active;
    auto [it, _] = parents_.emplace(pid, std::move(parent));
    route_remaining(it->second, req.timestamp_ns, out_commands);
    return pid;
}

void OrderManagementSystem::route_remaining(ParentOrder& parent, TimestampNs now_ns,
                                            std::vector<ChildCommand>& out_commands) {
    if (is_terminal(parent.state) || parent.cancel_requested) return;

    const Quantity unallocated = parent.leaves_qty - parent.in_flight_qty;
    if (unallocated <= 0) return;

    RouteRequest rreq{};
    rreq.parent_id = parent.parent_id;
    rreq.side = parent.side;
    rreq.qty = unallocated;
    rreq.limit_price = parent.limit_price;
    rreq.tif = parent.tif;
    rreq.now_ns = now_ns;
    rreq.reference_mid_ticks = parent.arrival_mid_ticks;
    rreq.excluded_venues_mask = parent.failed_venues_mask;
    rreq.active_children_per_venue = active_children_per_venue_;

    const RoutePlan plan = planner_.plan(book_, &health_, rreq);

    if (audit_.enabled()) {
        audit_.record(now_ns, AuditEventType::RouteDecision, parent.parent_id, 0, kInvalidVenue,
                      parent.client_order_id, RoutePlanner::format_explanation(plan, book_));
    }

    if (plan.slice_count == 0) {
        if (parent.in_flight_qty == 0) {
            finalize_parent(parent, ParentOrderState::Expired, now_ns);
        }
        return;
    }

    for (std::size_t i = 0; i < plan.slice_count; ++i) {
        const auto& sl = plan.slices[i];
        const OrderId cid = next_child_id_++;

        ChildOrder child{};
        child.child_id = cid;
        child.parent_id = parent.parent_id;
        child.venue_id = sl.venue_id;
        child.side = parent.side;
        child.tif = (parent.tif == TimeInForce::FOK) ? TimeInForce::IOC : parent.tif;
        child.orig_qty = sl.quantity;
        child.leaves_qty = sl.quantity;
        child.limit_price = sl.limit_price;
        child.sent_ts_ns = now_ns;
        child.last_update_ts_ns = now_ns;
        child.state = ChildOrderState::PendingNew;
        child.expected_cost = sl.cost;

        children_.emplace(cid, child);
        parent.child_ids.push_back(cid);
        parent.in_flight_qty += sl.quantity;
        if (sl.venue_id < kMaxVenues) {
            ++active_children_per_venue_[sl.venue_id];
        }
        ++stats_.children_sent;

        ChildCommand cmd{};
        cmd.type = ChildCommandType::NewOrder;
        cmd.child_id = cid;
        cmd.parent_id = parent.parent_id;
        cmd.venue_id = sl.venue_id;
        cmd.side = child.side;
        cmd.tif = child.tif;
        cmd.quantity = child.orig_qty;
        cmd.limit_price = child.limit_price;
        cmd.timestamp_ns = now_ns;
        out_commands.push_back(cmd);

        if (audit_.enabled()) {
            std::ostringstream oss;
            oss << "qty=" << child.orig_qty << " limit=$" << std::fixed << std::setprecision(4)
                << ticks_to_usd(child.limit_price) << " exp_cost=" << std::setprecision(2)
                << child.expected_cost.total_expected_cost_bps << "bps";
            audit_.record(now_ns, AuditEventType::ChildSent, parent.parent_id, cid, sl.venue_id,
                          parent.client_order_id, oss.str());
        }
    }
}

void OrderManagementSystem::on_execution_report(const ExecutionReport& report,
                                                std::vector<ChildCommand>& out_commands) {
    auto cit = children_.find(report.child_id);
    if (cit == children_.end()) return;
    auto& child = cit->second;

    auto pit = parents_.find(child.parent_id);
    if (pit == parents_.end()) return;
    auto& parent = pit->second;

    const VenueId vid = child.venue_id;

    switch (report.type) {
        case ExecReportType::Ack: {
            if (is_terminal(child.state)) return;
            child.ack_ts_ns = report.timestamp_ns;
            child.last_update_ts_ns = report.timestamp_ns;
            if (child.state == ChildOrderState::PendingNew) {
                child.state = ChildOrderState::Acked;
            }
            ++stats_.children_acked;
            const TimestampNs rtt =
                report.timestamp_ns > child.sent_ts_ns ? (report.timestamp_ns - child.sent_ts_ns) : 0;
            health_.record_ack(vid, rtt, report.timestamp_ns);
            audit_.record(report.timestamp_ns, AuditEventType::ChildAck, parent.parent_id,
                          child.child_id, vid, parent.client_order_id, "acked");
            break;
        }

        case ExecReportType::Fill: {
            // Idempotency: ignore duplicate execution IDs so partial fills reduce parent qty
            // exactly once.
            if (report.exec_id != 0) {
                if (seen_exec_ids_.contains(report.exec_id)) {
                    ++stats_.duplicate_execs_ignored;
                    audit_.record(report.timestamp_ns, AuditEventType::DuplicateExecIgnored,
                                  parent.parent_id, child.child_id, vid, parent.client_order_id,
                                  "duplicate exec_id=" + std::to_string(report.exec_id));
                    return;
                }
                seen_exec_ids_.insert(report.exec_id);
            }

            if (is_terminal(child.state) || report.fill_qty <= 0) return;

            const Quantity applied_qty =
                std::min({report.fill_qty, child.leaves_qty, parent.leaves_qty});
            if (applied_qty <= 0) return;

            const double notional =
                ticks_to_usd(report.fill_price) * static_cast<double>(applied_qty);
            child.filled_qty += applied_qty;
            child.leaves_qty -= applied_qty;
            child.total_notional_usd += notional;
            child.total_fees_usd += report.fee_usd;
            child.last_update_ts_ns = report.timestamp_ns;

            parent.filled_qty += applied_qty;
            parent.leaves_qty -= applied_qty;
            parent.in_flight_qty = std::max<Quantity>(0, parent.in_flight_qty - applied_qty);
            parent.total_notional_usd += notional;
            parent.total_fees_usd += report.fee_usd;

            risk_.on_fill(parent.side, applied_qty, report.fill_price, parent.limit_price);

            if (child.leaves_qty == 0) {
                child.state = ChildOrderState::Filled;
                ++stats_.children_filled;
                decrement_active_child(vid);
                health_.record_fill_outcome(vid, child.orig_qty, child.filled_qty);
            } else {
                child.state = ChildOrderState::PartiallyFilled;
            }

            if (audit_.enabled()) {
                std::ostringstream oss;
                oss << "exec_id=" << report.exec_id << " qty=" << applied_qty << " px=$"
                    << std::fixed << std::setprecision(4) << ticks_to_usd(report.fill_price)
                    << " fee=$" << report.fee_usd << " parent_rem=" << parent.leaves_qty;
                audit_.record(report.timestamp_ns, AuditEventType::ChildFill, parent.parent_id,
                              child.child_id, vid, parent.client_order_id, oss.str());
            }

            if (parent.leaves_qty == 0) {
                finalize_parent(parent, ParentOrderState::Filled, report.timestamp_ns);
            } else {
                parent.state = ParentOrderState::PartiallyFilled;
            }
            break;
        }

        case ExecReportType::CancelAck: {
            if (is_terminal(child.state)) return;
            const Quantity cancelled_qty = child.leaves_qty;
            child.leaves_qty = 0;
            child.state = ChildOrderState::Cancelled;
            child.last_update_ts_ns = report.timestamp_ns;
            parent.in_flight_qty = std::max<Quantity>(0, parent.in_flight_qty - cancelled_qty);
            ++stats_.children_cancelled;
            decrement_active_child(vid);
            health_.record_fill_outcome(vid, child.orig_qty, child.filled_qty);

            audit_.record(report.timestamp_ns, AuditEventType::ChildCancelled, parent.parent_id,
                          child.child_id, vid, parent.client_order_id,
                          "cancelled_qty=" + std::to_string(cancelled_qty));

            if (is_terminal(parent.state)) return;

            if (parent.cancel_requested) {
                if (parent.in_flight_qty == 0) {
                    finalize_parent(parent, ParentOrderState::Cancelled, report.timestamp_ns);
                }
                return;
            }

            // If there is remaining unallocated quantity and we have reroute attempts left:
            // - For DAY orders, or when a venue failure forced cancellation, reroute to remaining
            //   healthy venues.
            // - For IOC orders whose child was partially filled/unfilled normally (venue healthy),
            //   also allow rerouting if another eligible venue has liquidity and reroute attempts
            //   remain (excluding the venue that just failed to fill the rest).
            const Quantity unallocated = parent.leaves_qty - parent.in_flight_qty;
            if (unallocated > 0 &&
                parent.reroute_attempts < planner_.config().max_reroute_attempts) {
                parent.failed_venues_mask |= (1U << vid);
                ++parent.reroute_attempts;
                route_remaining(parent, report.timestamp_ns, out_commands);
            } else if (parent.in_flight_qty == 0 && parent.leaves_qty > 0) {
                finalize_parent(parent, ParentOrderState::Expired, report.timestamp_ns);
            }
            break;
        }

        case ExecReportType::ReplaceAck: {
            if (is_terminal(child.state)) return;
            const Quantity old_leaves = child.leaves_qty;
            if (child.pending_replace_qty > child.filled_qty) {
                child.orig_qty = child.pending_replace_qty;
                child.leaves_qty = child.orig_qty - child.filled_qty;
            }
            if (child.pending_replace_price > 0) {
                child.limit_price = child.pending_replace_price;
            }
            child.pending_replace_qty = 0;
            child.pending_replace_price = 0;
            child.state = child.filled_qty > 0 ? ChildOrderState::PartiallyFilled
                                               : ChildOrderState::Acked;
            child.last_update_ts_ns = report.timestamp_ns;

            parent.in_flight_qty =
                std::max<Quantity>(0, parent.in_flight_qty - old_leaves + child.leaves_qty);
            ++stats_.children_replaced;
            audit_.record(report.timestamp_ns, AuditEventType::ChildReplaced, parent.parent_id,
                          child.child_id, vid, parent.client_order_id,
                          "new_qty=" + std::to_string(child.orig_qty));
            break;
        }

        case ExecReportType::Reject:
        case ExecReportType::Timeout: {
            if (is_terminal(child.state)) return;
            const Quantity failed_qty = child.leaves_qty;
            child.leaves_qty = 0;
            child.state = (report.type == ExecReportType::Reject) ? ChildOrderState::Rejected
                                                                  : ChildOrderState::Cancelled;
            child.last_update_ts_ns = report.timestamp_ns;
            parent.in_flight_qty = std::max<Quantity>(0, parent.in_flight_qty - failed_qty);
            decrement_active_child(vid);

            bool tripped = false;
            if (report.type == ExecReportType::Reject) {
                ++stats_.children_rejected;
                tripped = health_.record_reject(vid, report.timestamp_ns);
                audit_.record(report.timestamp_ns, AuditEventType::ChildRejected, parent.parent_id,
                              child.child_id, vid, parent.client_order_id, report.reason);
            } else {
                ++stats_.children_timed_out;
                tripped = health_.record_timeout(vid, report.timestamp_ns);
                audit_.record(report.timestamp_ns, AuditEventType::ChildTimeout, parent.parent_id,
                              child.child_id, vid, parent.client_order_id, report.reason);
            }

            if (tripped) {
                audit_.record(report.timestamp_ns, AuditEventType::VenueHealthChanged,
                              parent.parent_id, child.child_id, vid, parent.client_order_id,
                              "state=CIRCUIT_OPEN");
            }

            if (is_terminal(parent.state)) return;

            if (parent.cancel_requested) {
                if (parent.in_flight_qty == 0) {
                    finalize_parent(parent, ParentOrderState::Cancelled, report.timestamp_ns);
                }
                return;
            }

            // Automatic failover: exclude the failing venue for this parent and reroute remaining.
            parent.failed_venues_mask |= (1U << vid);
            const Quantity unallocated = parent.leaves_qty - parent.in_flight_qty;
            if (unallocated > 0 &&
                parent.reroute_attempts < planner_.config().max_reroute_attempts) {
                ++parent.reroute_attempts;
                ++stats_.failovers_triggered;
                audit_.record(report.timestamp_ns, AuditEventType::FailoverTriggered,
                              parent.parent_id, child.child_id, vid, parent.client_order_id,
                              "attempt=" + std::to_string(parent.reroute_attempts) +
                                  " qty=" + std::to_string(unallocated));
                route_remaining(parent, report.timestamp_ns, out_commands);
            } else if (parent.in_flight_qty == 0 && parent.leaves_qty > 0) {
                finalize_parent(parent, ParentOrderState::Expired, report.timestamp_ns);
            }
            break;
        }
    }
}

bool OrderManagementSystem::cancel_parent(OrderId parent_id, TimestampNs now_ns,
                                          std::vector<ChildCommand>& out_commands) {
    auto it = parents_.find(parent_id);
    if (it == parents_.end() || is_terminal(it->second.state)) return false;

    auto& parent = it->second;
    parent.cancel_requested = true;

    bool any_active_child = false;
    for (OrderId cid : parent.child_ids) {
        auto cit = children_.find(cid);
        if (cit == children_.end() || is_terminal(cit->second.state)) continue;
        any_active_child = true;
        cancel_child(cid, now_ns, out_commands);
    }

    if (!any_active_child) {
        finalize_parent(parent, ParentOrderState::Cancelled, now_ns);
    }
    return true;
}

bool OrderManagementSystem::cancel_child(OrderId child_id, TimestampNs now_ns,
                                         std::vector<ChildCommand>& out_commands) {
    auto it = children_.find(child_id);
    if (it == children_.end() || is_terminal(it->second.state) ||
        it->second.state == ChildOrderState::PendingCancel) {
        return false;
    }
    auto& child = it->second;
    child.state = ChildOrderState::PendingCancel;
    child.last_update_ts_ns = now_ns;

    ChildCommand cmd{};
    cmd.type = ChildCommandType::CancelOrder;
    cmd.child_id = child.child_id;
    cmd.parent_id = child.parent_id;
    cmd.venue_id = child.venue_id;
    cmd.side = child.side;
    cmd.timestamp_ns = now_ns;
    out_commands.push_back(cmd);

    const auto* parent = get_parent(child.parent_id);
    audit_.record(now_ns, AuditEventType::ChildCancelSent, child.parent_id, child.child_id,
                  child.venue_id, parent ? parent->client_order_id : "", "cancel_sent");
    return true;
}

bool OrderManagementSystem::replace_child(OrderId child_id, Quantity new_qty, PriceTicks new_price,
                                          TimestampNs now_ns,
                                          std::vector<ChildCommand>& out_commands) {
    auto it = children_.find(child_id);
    if (it == children_.end() || is_terminal(it->second.state) ||
        it->second.state == ChildOrderState::PendingCancel) {
        return false;
    }
    auto& child = it->second;
    if (new_qty <= child.filled_qty || new_price <= 0) return false;

    const auto* parent = get_parent(child.parent_id);
    if (!parent) return false;

    // Ensure replace cannot cause parent over-allocation.
    const Quantity delta_leaves = (new_qty - child.filled_qty) - child.leaves_qty;
    if (delta_leaves > (parent->leaves_qty - parent->in_flight_qty)) {
        return false;
    }

    child.pending_replace_qty = new_qty;
    child.pending_replace_price = new_price;
    child.state = ChildOrderState::PendingReplace;
    child.last_update_ts_ns = now_ns;

    ChildCommand cmd{};
    cmd.type = ChildCommandType::ReplaceOrder;
    cmd.child_id = child.child_id;
    cmd.parent_id = child.parent_id;
    cmd.venue_id = child.venue_id;
    cmd.side = child.side;
    cmd.quantity = new_qty;
    cmd.limit_price = new_price;
    cmd.timestamp_ns = now_ns;
    out_commands.push_back(cmd);

    audit_.record(now_ns, AuditEventType::ChildReplaceSent, child.parent_id, child.child_id,
                  child.venue_id, parent->client_order_id,
                  "replace_qty=" + std::to_string(new_qty));
    return true;
}

void OrderManagementSystem::on_venue_down(VenueId vid, TimestampNs now_ns,
                                          std::vector<ChildCommand>& out_commands) {
    health_.set_manual_down(vid, true, now_ns);
    audit_.record(now_ns, AuditEventType::VenueHealthChanged, 0, 0, vid, "",
                  "state=CIRCUIT_OPEN reason=MANUAL_OR_OUTAGE");

    std::vector<OrderId> affected_children;
    for (const auto& [cid, child] : children_) {
        if (child.venue_id == vid && !is_terminal(child.state)) {
            affected_children.push_back(cid);
        }
    }

    for (OrderId cid : affected_children) {
        cancel_child(cid, now_ns, out_commands);
        // Synthesize immediate timeout failover so the parent does not stall waiting on a dead
        // venue.
        ExecutionReport rep{};
        rep.type = ExecReportType::Timeout;
        rep.child_id = cid;
        rep.venue_id = vid;
        rep.timestamp_ns = now_ns;
        rep.reason = "venue_down_failover";
        on_execution_report(rep, out_commands);
    }
}

void OrderManagementSystem::finalize_parent(ParentOrder& parent, ParentOrderState final_state,
                                            TimestampNs now_ns) {
    if (is_terminal(parent.state)) return;

    const Quantity unreleased = parent.leaves_qty;
    if (unreleased > 0) {
        risk_.on_release(parent.side, unreleased, parent.limit_price);
    }
    parent.leaves_qty = 0;
    parent.state = final_state;
    parent.completion_ts_ns = now_ns;

    if (parent.filled_qty == parent.orig_qty) {
        ++stats_.parents_filled;
    } else if (parent.filled_qty > 0) {
        ++stats_.parents_partial;
    } else {
        ++stats_.parents_unfilled;
    }

    if (audit_.enabled()) {
        std::ostringstream oss;
        oss << "state=" << to_string(final_state) << " filled=" << parent.filled_qty << "/"
            << parent.orig_qty << " avg_px=$" << std::fixed << std::setprecision(4)
            << parent.avg_fill_price_usd() << " fees=$" << parent.total_fees_usd
            << " net_is_bps=" << std::setprecision(2) << parent.net_shortfall_bps();
        audit_.record(now_ns, AuditEventType::ParentCompleted, parent.parent_id, 0, kInvalidVenue,
                      parent.client_order_id, oss.str());
    }
}

}  // namespace sor
