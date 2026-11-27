#include "hft/runtime/fault_injector.hpp"

namespace hft::runtime {

using namespace contracts::v1;

ExecutionFaultInjector::ExecutionFaultInjector(ExecFaultConfig cfg) : cfg_(cfg) {}

FaultInjectionResult ExecutionFaultInjector::evaluate(InstrumentId instrument,
                                                      const ExecutionReport& report) {
  ++stats_.reports_seen;
  FaultInjectionResult res{};

  if (cfg_.extra_ack_delay_ns > 0 && report.type == ExecReportType::Accepted) {
    res.extra_delay_ns = cfg_.extra_ack_delay_ns;
    ++stats_.acks_delayed;
  }

  if (cfg_.drop_response_at_index > 0 && stats_.reports_seen == cfg_.drop_response_at_index) {
    res.drop_report = true;
    ++stats_.responses_dropped;
    dropped_for_reconnect_.emplace_back(instrument, report);
    return res;
  }

  const bool is_fill = (report.type == ExecReportType::PartiallyFilled ||
                        report.type == ExecReportType::Filled);
  if ((cfg_.duplicate_every_fill && is_fill) ||
      (cfg_.duplicate_report_every_n > 0 &&
       (stats_.reports_seen % cfg_.duplicate_report_every_n == 0))) {
    res.emit_duplicate = true;
    ++stats_.duplicates_injected;
  }

  if (cfg_.disconnect_after_reports > 0 &&
      stats_.reports_seen == cfg_.disconnect_after_reports) {
    res.trigger_disconnect = true;
    ++stats_.disconnects_injected;
  }

  return res;
}

}  // namespace hft::runtime
