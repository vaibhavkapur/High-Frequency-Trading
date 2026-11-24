#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xarb/paper_executor.hpp"
#include "xarb/scanner.hpp"
#include "xarb/venue_adapter.hpp"

namespace xarb {

struct PercentileStats {
  double min_val{0.0};
  double p50{0.0};
  double p95{0.0};
  double p99{0.0};
  double p999{0.0};
  double max_val{0.0};
  double mean{0.0};
  size_t count{0};
};

PercentileStats compute_percentiles(std::vector<double> samples);

struct VenueRunReport {
  VenueId venue{VenueId::Binance};
  uint64_t messages_received{0};
  uint64_t deltas_applied{0};
  uint64_t snapshots_applied{0};
  uint64_t heartbeats_received{0};
  uint64_t gaps_detected{0};
  uint64_t checksum_mismatches{0};
  uint64_t disconnects{0};
  uint64_t resync_count{0};
  uint64_t stale_transitions{0};
  double total_stale_ms{0.0};
  double total_resync_ms{0.0};
  PercentileStats reconnect_duration_ms{};
  PercentileStats staleness_ms{};
};

struct ReplaySummary {
  std::string feed_path;
  std::string scenario_name;
  uint64_t total_events{0};
  double simulated_span_sec{0.0};
  double wall_time_sec{0.0};
  double events_per_sec{0.0};
  double book_updates_per_sec{0.0};

  ScannerStats scanner{};
  PaperStats paper{};

  double mean_gross_spread_bps{0.0};
  double mean_net_edge_bps{0.0};
  double false_positive_rate{0.0};  // (both_filled_loss + one_legged + both_missed) / completed
  double fill_rate{0.0};            // (both_filled_profitable + both_filled_loss) / attempted
  double initial_equity_quote{0.0};
  double final_equity_quote{0.0};
  double equity_delta_quote{0.0};
  bool accounting_invariants_ok{true};

  PercentileStats processing_latency_ns{};
  std::array<VenueRunReport, kVenueCount> venues{};
  uint64_t run_hash{0};
};

// Writes audit artifacts (summary.json, signals.csv, trades.csv, and optional audit.sqlite) to out_dir.
bool write_audit_outputs(const std::string& out_dir, const InstrumentRegistry& registry,
                         const ReplaySummary& summary, const std::vector<ArbitrageSignal>& signals,
                         const std::vector<PaperTradeRecord>& trades, bool write_sqlite, std::string* err = nullptr);

std::string format_summary_text(const ReplaySummary& s);

}  // namespace xarb
