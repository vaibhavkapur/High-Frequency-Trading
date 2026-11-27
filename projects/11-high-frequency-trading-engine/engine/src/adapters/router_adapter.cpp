#include "hft/adapters/router_adapter.hpp"

#include <algorithm>
#include <cmath>

namespace hft::adapters {

using namespace contracts::v1;

namespace {

sor::VenueConfig to_sor_venue(const VenueMeta& vm, const InstrumentMeta& inst) {
  sor::VenueConfig vc{};
  vc.id = static_cast<sor::VenueId>(vm.id);
  vc.name = vm.name;
  vc.enabled = vm.enabled;
  vc.taker_fee_per_share_ticks = 0;
  vc.taker_fee_bps = static_cast<double>(vm.taker_fee_bps_x100) / 100.0;
  vc.maker_fee_per_share_ticks = 0;
  vc.maker_fee_bps = static_cast<double>(vm.maker_fee_bps_x100) / 100.0;
  vc.base_one_way_latency_ns = vm.order_latency_ns;
  vc.latency_jitter_ns = std::max<std::int64_t>(5'000, vm.order_latency_ns / 10);
  vc.stale_timeout_ns = vm.stale_threshold_ns;
  vc.tick_size = inst.tick_size_price4;
  vc.lot_size = static_cast<sor::Quantity>(inst.lot_size);
  vc.max_child_qty = 100'000;
  vc.priority = vm.id;
  return vc;
}

sor::TimeInForce to_sor_tif(TimeInForce tif) noexcept {
  switch (tif) {
    case TimeInForce::IOC: return sor::TimeInForce::IOC;
    case TimeInForce::FOK: return sor::TimeInForce::FOK;
    case TimeInForce::GTC:
    case TimeInForce::DAY: return sor::TimeInForce::DAY;
  }
  return sor::TimeInForce::IOC;
}

TimeInForce from_sor_tif(sor::TimeInForce tif) noexcept {
  switch (tif) {
    case sor::TimeInForce::IOC: return TimeInForce::IOC;
    case sor::TimeInForce::FOK: return TimeInForce::FOK;
    case sor::TimeInForce::DAY: return TimeInForce::DAY;
  }
  return TimeInForce::IOC;
}

}  // namespace

SmartRouterAdapter::SmartRouterAdapter(std::vector<InstrumentMeta> instruments,
                                       std::vector<VenueMeta> venues, sor::RoutingPolicy policy)
    : instruments_(std::move(instruments)), venues_(std::move(venues)) {
  for (const auto& inst : instruments_) {
    auto st = std::make_unique<InstrumentSorStack>();
    std::vector<sor::VenueConfig> vcs;
    for (const auto& vm : venues_) {
      if (vm.id < sor::kMaxVenues) {
        vcs.push_back(to_sor_venue(vm, inst));
      }
    }
    st->book.configure_venues(vcs);
    sor::RouterConfig rcfg{};
    rcfg.policy = policy;
    rcfg.respect_staleness = true;
    rcfg.respect_venue_health = true;
    rcfg.allow_partial_sweep = true;
    st->planner.set_config(rcfg);

    sor::RiskLimits lim{};
    lim.max_order_qty = 50'000;
    lim.max_order_notional_usd = 50'000'000.0;
    lim.max_net_position_qty = 100'000;
    lim.max_open_notional_usd = 100'000'000.0;
    lim.max_price_collar_bps = 1000.0;
    st->risk.set_limits(lim);
    st->audit.set_enabled(true);
    st->oms = std::make_unique<sor::OrderManagementSystem>(
        st->book, st->planner, st->risk, st->health, st->audit);
    stacks_.push_back(std::move(st));
  }
}

void SmartRouterAdapter::set_policy(sor::RoutingPolicy policy) {
  for (auto& st : stacks_) {
    auto cfg = st->planner.config();
    cfg.policy = policy;
    st->planner.set_config(cfg);
  }
}

void SmartRouterAdapter::update_venue_depth(InstrumentId instrument, VenueId venue,
                                            const std::vector<LevelView>& bids,
                                            const std::vector<LevelView>& asks,
                                            SimTimeNs exchange_ts_ns, SimTimeNs recv_ts_ns) {
  if (instrument >= stacks_.size() || venue >= sor::kMaxVenues) return;
  auto& st = *stacks_[instrument];
  const auto& inst = instruments_[instrument];
  const auto vid = static_cast<sor::VenueId>(venue);
  if (!st.book.has_venue(vid)) return;

  const auto seq = static_cast<std::uint64_t>(std::max<std::int64_t>(1, recv_ts_ns.value));
  auto apply_side = [&](sor::Side s, const std::vector<LevelView>& lvls) {
    sor::NormalizedQuoteUpdate upd{};
    upd.venue_id = vid;
    upd.symbol_id = instrument;
    upd.type = sor::QuoteUpdateType::Snapshot;
    upd.side = s;
    upd.exchange_ts_ns = exchange_ts_ns.value;
    upd.recv_ts_ns = recv_ts_ns.value;
    upd.seq = seq;
    const std::size_t n = std::min(lvls.size(), sor::kMaxBookDepth);
    upd.level_count = static_cast<std::uint8_t>(n);
    for (std::size_t i = 0; i < n; ++i) {
      upd.levels[i].price = lvls[i].price.ticks * inst.tick_size_price4;
      upd.levels[i].quantity = static_cast<sor::Quantity>(lvls[i].qty.lots);
      upd.levels[i].order_count = lvls[i].order_count;
    }
    (void)st.book.apply_update(upd);
  };
  apply_side(sor::Side::Buy, bids);
  apply_side(sor::Side::Sell, asks);
}

void SmartRouterAdapter::translate_child_commands(InstrumentId instrument,
                                                  const std::vector<sor::ChildCommand>& cmds,
                                                  SimTimeNs now_ns,
                                                  std::vector<RoutedChildOrder>& out_children) {
  const auto& inst = instruments_[instrument];
  for (const auto& cmd : cmds) {
    RoutedChildOrder rco{};
    rco.venue = cmd.venue_id;
    rco.instrument = instrument;
    rco.parent_id = cmd.parent_id;
    rco.child_id = cmd.child_id;

    OrderIntent oi{};
    oi.client_order_id = next_client_order_id_++;
    oi.parent_id = cmd.parent_id;
    oi.side = (cmd.side == sor::Side::Buy) ? Side::Buy : Side::Sell;
    oi.order_type = OrderType::Limit;
    oi.tif = from_sor_tif(cmd.tif);
    oi.limit_price = Price{cmd.limit_price / inst.tick_size_price4};
    oi.qty = Quantity{cmd.quantity > 0 ? static_cast<std::uint64_t>(cmd.quantity) : 0ULL};
    oi.strategy_decision_time_ns = now_ns;

    if (cmd.type == sor::ChildCommandType::NewOrder) {
      oi.action = OrderAction::New;
      oi.token = ClientToken::from_uint64("SOR", cmd.child_id);
      token_to_child_[oi.token] = ChildCorrelation{instrument, rco.venue, cmd.parent_id,
                                                   cmd.child_id};
      child_to_token_[cmd.child_id] = oi.token;
    } else if (cmd.type == sor::ChildCommandType::CancelOrder) {
      oi.action = OrderAction::Cancel;
      oi.token = ClientToken::from_uint64("SXC", oi.client_order_id);
      const auto it = child_to_token_.find(cmd.child_id);
      if (it != child_to_token_.end()) oi.orig_token = it->second;
    } else {
      oi.action = OrderAction::Replace;
      oi.token = ClientToken::from_uint64("SXR", oi.client_order_id);
      const auto it = child_to_token_.find(cmd.child_id);
      if (it != child_to_token_.end()) oi.orig_token = it->second;
      token_to_child_[oi.token] = ChildCorrelation{instrument, rco.venue, cmd.parent_id,
                                                   cmd.child_id};
      child_to_token_[cmd.child_id] = oi.token;
    }
    rco.intent = oi;
    out_children.push_back(rco);
  }
}

ParentOrderId SmartRouterAdapter::route_parent(InstrumentId instrument,
                                               const OrderIntent& parent_intent, SimTimeNs now_ns,
                                               std::vector<RoutedChildOrder>& out_children) {
  if (instrument >= stacks_.size()) return 0;
  auto& st = *stacks_[instrument];
  const auto& inst = instruments_[instrument];

  sor::ParentOrderRequest req{};
  req.client_order_id = parent_intent.token.to_string();
  req.symbol_id = instrument;
  req.side = (parent_intent.side == Side::Buy) ? sor::Side::Buy : sor::Side::Sell;
  req.quantity = static_cast<sor::Quantity>(parent_intent.qty.lots);
  req.limit_price = parent_intent.limit_price.ticks * inst.tick_size_price4;
  req.tif = to_sor_tif(parent_intent.tif);
  req.timestamp_ns = now_ns.value;

  std::vector<sor::ChildCommand> cmds;
  const sor::OrderId pid = st.oms->submit_parent(req, cmds);
  translate_child_commands(instrument, cmds, now_ns, out_children);
  return pid;
}

void SmartRouterAdapter::on_child_execution_report(
    InstrumentId instrument, VenueId venue, const ExecutionReport& report, SimTimeNs now_ns,
    std::vector<RoutedChildOrder>& out_reroute_children) {
  if (instrument >= stacks_.size()) return;
  const auto it = token_to_child_.find(report.token);
  if (it == token_to_child_.end()) return;

  auto& st = *stacks_[instrument];
  const auto& inst = instruments_[instrument];

  sor::ExecutionReport sr{};
  sr.child_id = it->second.child_id;
  sr.venue_id = static_cast<sor::VenueId>(venue);
  sr.exec_id = report.exec_id;
  sr.fill_qty = static_cast<sor::Quantity>(report.fill_qty.lots);
  sr.fill_price = report.exec_price.ticks * inst.tick_size_price4;
  sr.fee_usd = report.fee_scaled.to_dollars();
  sr.timestamp_ns = now_ns.value;

  switch (report.type) {
    case ExecReportType::Accepted: sr.type = sor::ExecReportType::Ack; break;
    case ExecReportType::PartiallyFilled:
    case ExecReportType::Filled: sr.type = sor::ExecReportType::Fill; break;
    case ExecReportType::Canceled: sr.type = sor::ExecReportType::CancelAck; break;
    case ExecReportType::Replaced: sr.type = sor::ExecReportType::ReplaceAck; break;
    case ExecReportType::Rejected:
    case ExecReportType::CancelRejected:
      sr.type = sor::ExecReportType::Reject;
      sr.reason = to_string(report.reject_reason);
      break;
    case ExecReportType::Uncertain:
      sr.type = sor::ExecReportType::Timeout;
      sr.reason = "uncertain_disconnect";
      break;
  }

  std::vector<sor::ChildCommand> reroutes;
  st.oms->on_execution_report(sr, reroutes);
  translate_child_commands(instrument, reroutes, now_ns, out_reroute_children);
}

void SmartRouterAdapter::mark_venue_down(VenueId venue, SimTimeNs now_ns,
                                         std::vector<RoutedChildOrder>& out_reroute_children) {
  if (venue >= sor::kMaxVenues) return;
  for (std::size_t i = 0; i < stacks_.size(); ++i) {
    std::vector<sor::ChildCommand> reroutes;
    stacks_[i]->oms->on_venue_down(static_cast<sor::VenueId>(venue), now_ns.value, reroutes);
    translate_child_commands(static_cast<InstrumentId>(i), reroutes, now_ns, out_reroute_children);
  }
}

void SmartRouterAdapter::mark_venue_up(VenueId venue, SimTimeNs now_ns) {
  if (venue >= sor::kMaxVenues) return;
  for (auto& st : stacks_) {
    st->health.set_manual_down(static_cast<sor::VenueId>(venue), false, now_ns.value);
  }
}

sor::ConsolidatedBBO SmartRouterAdapter::consolidated_bbo(InstrumentId instrument,
                                                          SimTimeNs now_ns) const {
  if (instrument >= stacks_.size()) return sor::ConsolidatedBBO{};
  return stacks_[instrument]->book.consolidated_bbo(now_ns.value, &stacks_[instrument]->health);
}

sor::OmsStats SmartRouterAdapter::stats(InstrumentId instrument) const {
  if (instrument >= stacks_.size()) return sor::OmsStats{};
  return stacks_[instrument]->oms->stats();
}

const sor::ParentOrder* SmartRouterAdapter::find_parent(InstrumentId instrument,
                                                        ParentOrderId parent_id) const {
  if (instrument >= stacks_.size()) return nullptr;
  return stacks_[instrument]->oms->get_parent(parent_id);
}

std::size_t SmartRouterAdapter::audit_event_count(InstrumentId instrument) const {
  if (instrument >= stacks_.size()) return 0;
  return stacks_[instrument]->audit.size();
}

}  // namespace hft::adapters
