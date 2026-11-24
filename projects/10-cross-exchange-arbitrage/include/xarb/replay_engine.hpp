#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xarb/audit.hpp"
#include "xarb/feed_codec.hpp"
#include "xarb/instrument.hpp"
#include "xarb/paper_executor.hpp"
#include "xarb/scanner.hpp"
#include "xarb/types.hpp"
#include "xarb/venue_adapter.hpp"

namespace xarb {

struct OutageWindow {
  VenueId venue{VenueId::Coinbase};
  TimestampNs start_ns{0};
  TimestampNs end_ns{0};
};

struct ReplayConfig {
  std::string scenario_name{"default"};
  uint64_t rng_seed{42};
  TimestampNs max_stale_ns{1500LL * kNsPerMs};
  TimestampNs health_check_interval_ns{250LL * kNsPerMs};
  size_t max_book_depth{50};
  // Additional synthetic feed ingestion delay per venue (ns) applied to observed books vs truth books.
  std::array<TimestampNs, kVenueCount> feed_delay_ns{0, 0, 0};
  // Optional synthetic outage windows that suppress messages from a venue during [start_ns, end_ns].
  std::vector<OutageWindow> injected_outages;
  ScannerConfig scanner{};
  PaperConfig paper{};
  bool measure_processing_latency{true};
};

struct ReplayResult {
  ReplaySummary summary;
  std::vector<ArbitrageSignal> signals;
  std::vector<PaperTradeRecord> trades;
};

class ReplayEngine {
 public:
  ReplayEngine(const InstrumentRegistry& registry, ReplayConfig cfg);

  ReplayResult run(const std::vector<RecordedFeedEvent>& feed, const std::string& feed_path = "<memory>");

 private:
  enum class EvKind : uint8_t {
    TruthFeed = 0,
    ObservedFeed = 1,
    OrderArrival = 2,
    RebalanceArrival = 3,
    HealthTimer = 4,
  };

  struct ScheduledEvent {
    TimestampNs ts_ns{0};
    EvKind kind{EvKind::TruthFeed};
    uint64_t seq{0};
    size_t feed_idx{0};
    PendingLegOrder leg{};
    RebalanceTransfer xfer{};

    bool operator>(const ScheduledEvent& o) const noexcept {
      if (ts_ns != o.ts_ns) return ts_ns > o.ts_ns;
      if (kind != o.kind) return static_cast<uint8_t>(kind) > static_cast<uint8_t>(o.kind);
      return seq > o.seq;
    }
  };

  bool is_in_injected_outage(VenueId v, TimestampNs ts_ns) const noexcept;
  double compute_mark_mid_quote(uint32_t inst_id,
                                const std::vector<std::array<std::unique_ptr<VenueAdapter>, kVenueCount>>& books) const;

  const InstrumentRegistry& registry_;
  ReplayConfig cfg_;
};

}  // namespace xarb
