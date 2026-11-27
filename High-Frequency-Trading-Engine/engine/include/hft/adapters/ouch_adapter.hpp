#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hft/contracts/v1/events.hpp"
#include "ouch/event_log.hpp"
#include "ouch/gateway.hpp"
#include "ouch/order_manager.hpp"
#include "ouch/sim/exchange.hpp"
#include "ouch/wire.hpp"

namespace hft::adapters {

// Thin protocol & journaling adapter around Project 03 (ouch::wire, ouch::OrderManager,
// ouch::EventLog, ouch::Gateway, ouch::sim::ExchangeSim).
// Preserves OUCH-style token idempotence, SoupBinTCP/wire codecs, and journal replay recovery.
class OuchProtocolAdapter {
 public:
  explicit OuchProtocolAdapter(std::vector<contracts::v1::InstrumentMeta> instruments,
                               std::string firm = "HFT1");

  // Encode canonical OrderIntent -> Project 03 OUCH wire inbound message + wire bytes.
  contracts::v1::ErrorCode to_ouch_inbound(contracts::v1::InstrumentId instrument,
                                           const contracts::v1::OrderIntent& intent,
                                           ouch::wire::InboundMessage& out_msg,
                                           std::vector<std::uint8_t>* out_wire = nullptr) const;

  // Decode Project 03 OUCH wire outbound message -> canonical ExecutionReport.
  contracts::v1::ErrorCode from_ouch_outbound(contracts::v1::InstrumentId instrument,
                                              contracts::v1::OrderId client_order_id,
                                              const ouch::wire::OutboundMessage& msg,
                                              contracts::v1::SimTimeNs recv_time_ns,
                                              contracts::v1::ExecutionReport& out_report) const;

  // Encode canonical ExecutionReport -> Project 03 OUCH wire outbound message (for journaling/replay).
  contracts::v1::ErrorCode to_ouch_outbound(contracts::v1::InstrumentId instrument,
                                            const contracts::v1::ExecutionReport& report,
                                            ouch::wire::OutboundMessage& out_msg,
                                            std::vector<std::uint8_t>* out_wire = nullptr) const;

  static ouch::Token to_ouch_token(const contracts::v1::ClientToken& tok) noexcept;
  static contracts::v1::ClientToken from_ouch_token(const ouch::Token& tok) noexcept;
  static ouch::Symbol to_ouch_symbol(const contracts::v1::InstrumentMeta& inst) noexcept;

 private:
  std::vector<contracts::v1::InstrumentMeta> instruments_;
  ouch::Firm firm_;
};

// Wraps Project 03's binary EventLog + OrderManager so deterministic & network-lab runs
// record durable binary journals and reconstruct OMS state identically on replay/restart.
class OuchJournalBridge {
 public:
  explicit OuchJournalBridge(const OuchProtocolAdapter& proto);

  bool open(const std::string& path, bool flush_each = false);
  void close();
  [[nodiscard]] bool is_open() const noexcept { return log_.is_open(); }

  void record_order_sent(contracts::v1::InstrumentId instrument,
                         const contracts::v1::OrderIntent& intent, std::uint64_t ts_ns,
                         std::uint64_t seq);
  void record_order_resent(contracts::v1::InstrumentId instrument,
                           const contracts::v1::OrderIntent& intent, std::uint64_t ts_ns,
                           std::uint64_t seq);
  void record_exec_report(contracts::v1::InstrumentId instrument,
                          const contracts::v1::ExecutionReport& report, std::uint64_t ts_ns,
                          std::uint64_t seq);
  void record_session_up(std::uint64_t ts_ns, std::uint64_t seq);
  void record_session_down(std::uint64_t ts_ns);
  void flush();

  // Replays a journal file into a fresh ouch::OrderManager and verifies Project 03 invariants.
  struct ReplayCheck {
    bool ok = false;
    std::uint64_t records_read = 0;
    std::uint64_t orders_reconstructed = 0;
    std::uint64_t unresolved_orders = 0;
    std::string invariant_error;
  };
  static ReplayCheck replay_and_verify(const std::string& path, ouch::OrderManager& out_oms);

  [[nodiscard]] const ouch::OrderManager& live_oms() const noexcept { return oms_; }
  [[nodiscard]] std::uint64_t records_written() const noexcept { return log_.records_written(); }

 private:
  const OuchProtocolAdapter& proto_;
  ouch::EventLog log_;
  ouch::OrderManager oms_;
};

// End-to-end loopback TCP session pairing Project 03's ouch::Gateway with ouch::sim::ExchangeSim
// backed by Project 01's PoolBook (via OuchMatchingBridge). Used in `network-lab` mode.
struct OuchLoopbackResult {
  bool started = false;
  bool connected = false;
  bool reconnected = false;
  std::uint64_t orders_submitted = 0;
  std::uint64_t accepted = 0;
  std::uint64_t filled = 0;
  std::uint64_t canceled = 0;
  std::uint64_t rejected = 0;
  std::uint64_t uncertain_recovered = 0;
  std::string invariant_check;
  std::string error;
};

OuchLoopbackResult run_ouch_loopback_session(
    const std::vector<contracts::v1::InstrumentMeta>& instruments,
    const std::string& journal_path, bool inject_disconnect);

}  // namespace hft::adapters
