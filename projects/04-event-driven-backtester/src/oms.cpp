#include "hftbt/oms.hpp"

namespace hftbt {

Oms::Oms(std::size_t n_instruments, const RiskConfig& risk) : risk_(risk), inst_(n_instruments) {}

RejectReason Oms::try_register(const OrderRequest& req) {
    if (req.instrument >= inst_.size() || req.qty <= 0) return RejectReason::InvalidOrder;
    if (req.qty > risk_.max_order_qty) return RejectReason::RiskMaxOrderQty;
    if (orders_.size() >= risk_.max_open_orders) return RejectReason::RiskMaxOpenOrders;
    Instrument& in = inst_[req.instrument];
    // Worst case: every open order on this side fills and nothing on the other.
    const Qty worst = req.side == Side::Buy ? in.position + in.open_buy + req.qty
                                            : -in.position + in.open_sell + req.qty;
    if (worst > risk_.max_position) return RejectReason::RiskMaxPosition;

    OmsOrder o;
    o.id = req.id;
    o.instrument = req.instrument;
    o.side = req.side;
    o.price = req.price;
    o.qty = req.qty;
    orders_.emplace(req.id, o);
    (req.side == Side::Buy ? in.open_buy : in.open_sell) += req.qty;
    return RejectReason::None;
}

bool Oms::try_cancel(OrderId id) {
    const auto it = orders_.find(id);
    if (it == orders_.end() || it->second.status == OrderStatus::PendingCancel) return false;
    it->second.status = OrderStatus::PendingCancel;
    return true;
}

void Oms::close(std::unordered_map<OrderId, OmsOrder>::iterator it) {
    const OmsOrder& o = it->second;
    Instrument& in = inst_[o.instrument];
    (o.side == Side::Buy ? in.open_buy : in.open_sell) -= o.open();
    orders_.erase(it);
}

void Oms::on_report(const ExecReport& r) {
    const auto it = orders_.find(r.order_id);
    if (it == orders_.end()) return; // e.g. cancel-reject for an order we saw fill
    OmsOrder& o = it->second;
    switch (r.type) {
    case ExecType::Ack:
        if (o.status == OrderStatus::PendingNew) o.status = OrderStatus::Live;
        break;
    case ExecType::Fill: {
        Instrument& in = inst_[o.instrument];
        o.filled += r.qty;
        in.position += sign(o.side) * r.qty;
        (o.side == Side::Buy ? in.open_buy : in.open_sell) -= r.qty;
        if (r.leaves == 0) close(it);
        break;
    }
    case ExecType::Cancelled:
    case ExecType::Rejected: close(it); break;
    case ExecType::CancelRejected:
        if (o.status == OrderStatus::PendingCancel) o.status = OrderStatus::Live;
        break;
    }
}

} // namespace hftbt
