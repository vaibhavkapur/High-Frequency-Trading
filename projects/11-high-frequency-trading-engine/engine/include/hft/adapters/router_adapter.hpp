#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hft/contracts/v1/interfaces.hpp"
#include "sor/audit_log.hpp"
#include "sor/consolidated_book.hpp"
#include "sor/oms.hpp"
#include "sor/risk_manager.hpp"
#include "sor/route_planner.hpp"
#include "sor/venue_health.hpp"

namespace hft::adapters {

struct RoutedChildOrder {
  contracts::v1::VenueId venue = 0;
  contracts::v1::InstrumentId instrument = 0;
  contracts::v1::ParentOrderId parent_id = 0;
  contracts::v1::ChildOrderId child_id = 0;
  contracts::v1::OrderIntent intent{};
};

// Thin adapter around Project 07 (sor::ConsolidatedBook, sor::RoutePlanner, sor::VenueHealthMonitor,
// sor::RiskManager, sor::OrderManagementSystem).
// Instantiates one isolated SOR stack per InstrumentId because Project 07's ConsolidatedBook
// maintains single-symbol state.
class SmartRouterAdapter {
 public:
  SmartRouterAdapter(std::vector<contracts::v1::InstrumentMeta> instruments,
                     std::vector<contracts::v1::VenueMeta> venues,
                     sor::RoutingPolicy policy = sor::RoutingPolicy::SmartCostAware);

  void set_policy(sor::RoutingPolicy policy);

  void update_venue_depth(contracts::v1::InstrumentId instrument, contracts::v1::VenueId venue,
                          const std::vector<contracts::v1::LevelView>& bids,
                          const std::vector<contracts::v1::LevelView>& asks,
                          contracts::v1::SimTimeNs exchange_ts_ns,
                          contracts::v1::SimTimeNs recv_ts_ns);

  contracts::v1::ParentOrderId route_parent(
      contracts::v1::InstrumentId instrument, const contracts::v1::OrderIntent& parent_intent,
      contracts::v1::SimTimeNs now_ns, std::vector<RoutedChildOrder>& out_children);

  void on_child_execution_report(contracts::v1::InstrumentId instrument,
                                 contracts::v1::VenueId venue,
                                 const contracts::v1::ExecutionReport& report,
                                 contracts::v1::SimTimeNs now_ns,
                                 std::vector<RoutedChildOrder>& out_reroute_children);

  void mark_venue_down(contracts::v1::VenueId venue, contracts::v1::SimTimeNs now_ns,
                       std::vector<RoutedChildOrder>& out_reroute_children);
  void mark_venue_up(contracts::v1::VenueId venue, contracts::v1::SimTimeNs now_ns);

  [[nodiscard]] sor::ConsolidatedBBO consolidated_bbo(contracts::v1::InstrumentId instrument,
                                                      contracts::v1::SimTimeNs now_ns) const;
  [[nodiscard]] sor::OmsStats stats(contracts::v1::InstrumentId instrument) const;
  [[nodiscard]] const sor::ParentOrder* find_parent(contracts::v1::InstrumentId instrument,
                                                    contracts::v1::ParentOrderId parent_id) const;
  [[nodiscard]] std::size_t audit_event_count(contracts::v1::InstrumentId instrument) const;

 private:
  struct InstrumentSorStack {
    sor::ConsolidatedBook book;
    sor::VenueHealthMonitor health;
    sor::RoutePlanner planner;
    sor::RiskManager risk;
    sor::AuditLog audit;
    std::unique_ptr<sor::OrderManagementSystem> oms;
  };

  struct ChildCorrelation {
    contracts::v1::InstrumentId instrument = 0;
    contracts::v1::VenueId venue = 0;
    contracts::v1::ParentOrderId parent_id = 0;
    contracts::v1::ChildOrderId child_id = 0;
  };

  void translate_child_commands(contracts::v1::InstrumentId instrument,
                                const std::vector<sor::ChildCommand>& cmds,
                                contracts::v1::SimTimeNs now_ns,
                                std::vector<RoutedChildOrder>& out_children);

  std::vector<contracts::v1::InstrumentMeta> instruments_;
  std::vector<contracts::v1::VenueMeta> venues_;
  std::vector<std::unique_ptr<InstrumentSorStack>> stacks_;
  std::unordered_map<contracts::v1::ClientToken, ChildCorrelation, contracts::v1::ClientTokenHash>
      token_to_child_;
  std::unordered_map<contracts::v1::ChildOrderId, contracts::v1::ClientToken> child_to_token_;
  contracts::v1::OrderId next_client_order_id_ = 50'000;
};

}  // namespace hft::adapters
