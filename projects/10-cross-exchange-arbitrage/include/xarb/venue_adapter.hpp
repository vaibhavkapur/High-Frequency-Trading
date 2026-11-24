#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xarb/instrument.hpp"
#include "xarb/order_book.hpp"
#include "xarb/types.hpp"

namespace xarb {

struct AdapterMetrics {
  uint64_t messages_received{0};
  uint64_t deltas_applied{0};
  uint64_t snapshots_applied{0};
  uint64_t heartbeats_received{0};
  uint64_t duplicates_ignored{0};
  uint64_t gaps_detected{0};
  uint64_t checksum_mismatches{0};
  uint64_t parse_errors{0};
  uint64_t disconnects{0};
  uint64_t resync_count{0};
  uint64_t stale_transitions{0};
  TimestampNs total_stale_ns{0};
  TimestampNs total_resync_ns{0};
  std::vector<TimestampNs> reconnect_durations_ns;
  std::vector<TimestampNs> staleness_samples_ns;
};

struct AdapterAction {
  bool book_updated{false};
  bool needs_rest_snapshot{false};
  bool needs_ws_reconnect{false};
  bool gap_detected{false};
  bool checksum_mismatch{false};
  bool parse_error{false};
  bool became_stale{false};
  bool recovered_sync{false};
  std::string reason;
};

struct ReconnectPolicy {
  TimestampNs initial_backoff_ns{200LL * kNsPerMs};
  TimestampNs max_backoff_ns{10LL * kNsPerSec};
  double multiplier{2.0};
};

class VenueAdapter {
 public:
  VenueAdapter(VenueId venue, const InstrumentSpec& instrument, TimestampNs max_stale_ns, size_t max_depth = 50,
               ReconnectPolicy reconnect_policy = {});
  virtual ~VenueAdapter() = default;

  VenueId venue() const noexcept { return venue_; }
  uint32_t instrument_id() const noexcept { return instrument_.id; }
  const InstrumentSpec& instrument() const noexcept { return instrument_; }
  const FlatOrderBook& book() const noexcept { return book_; }
  BookState state() const noexcept { return state_; }
  SeqNum last_sequence() const noexcept { return last_seq_; }
  TimestampNs last_exchange_ts_ns() const noexcept { return last_exchange_ts_ns_; }
  TimestampNs last_recv_ts_ns() const noexcept { return last_recv_ts_ns_; }
  TimestampNs last_sync_ts_ns() const noexcept { return last_sync_ts_ns_; }
  const AdapterMetrics& metrics() const noexcept { return metrics_; }

  bool is_tradable(TimestampNs now_ns) const noexcept {
    if (state_ != BookState::Synced) return false;
    if (last_recv_ts_ns_ <= 0) return false;
    if (now_ns - last_recv_ts_ns_ > max_stale_ns_) return false;
    if (book_.is_crossed()) return false;
    return book_.best_bid().has_value() && book_.best_ask().has_value();
  }

  TimestampNs quote_age_ns(TimestampNs now_ns) const noexcept {
    if (last_recv_ts_ns_ <= 0 || now_ns < last_recv_ts_ns_) return 0;
    return now_ns - last_recv_ts_ns_;
  }

  TimestampNs next_reconnect_backoff_ns() noexcept;

  virtual AdapterAction on_ws_message(std::string_view payload, TimestampNs recv_ns) = 0;
  virtual AdapterAction on_rest_snapshot(std::string_view payload, TimestampNs recv_ns) = 0;
  virtual AdapterAction on_disconnect(TimestampNs now_ns, std::string_view reason);
  virtual AdapterAction on_reconnect(TimestampNs now_ns);
  AdapterAction check_staleness(TimestampNs now_ns);

 protected:
  void transition_to_synced(TimestampNs now_ns, AdapterAction& act);
  void transition_to_resync(TimestampNs now_ns, std::string_view reason, AdapterAction& act);

  VenueId venue_;
  InstrumentSpec instrument_;
  TimestampNs max_stale_ns_;
  ReconnectPolicy reconnect_policy_;
  FlatOrderBook book_;
  BookState state_{BookState::Disconnected};
  SeqNum last_seq_{0};
  TimestampNs last_exchange_ts_ns_{0};
  TimestampNs last_recv_ts_ns_{0};
  TimestampNs last_sync_ts_ns_{0};
  TimestampNs resync_started_ns_{0};
  TimestampNs stale_started_ns_{0};
  uint32_t consecutive_reconnects_{0};
  AdapterMetrics metrics_{};
};

// Binance Spot Depth Stream + REST Snapshot Sync Adapter.
// Rules:
//   1. Buffer WS depthUpdate events while awaiting REST /api/v3/depth snapshot.
//   2. Drop buffered events where u <= lastUpdateId.
//   3. First processed event must satisfy U <= lastUpdateId + 1 && u >= lastUpdateId + 1.
//   4. Every subsequent event must satisfy U == prev_u + 1 (else gap -> resync).
class BinanceAdapter final : public VenueAdapter {
 public:
  BinanceAdapter(const InstrumentSpec& instrument, TimestampNs max_stale_ns, size_t max_depth = 50,
                 ReconnectPolicy reconnect_policy = {});

  AdapterAction on_ws_message(std::string_view payload, TimestampNs recv_ns) override;
  AdapterAction on_rest_snapshot(std::string_view payload, TimestampNs recv_ns) override;
  AdapterAction on_disconnect(TimestampNs now_ns, std::string_view reason) override;
  AdapterAction on_reconnect(TimestampNs now_ns) override;

 private:
  struct BufferedDelta {
    SeqNum first_u{0};
    SeqNum final_u{0};
    TimestampNs exch_ts_ns{0};
    TimestampNs recv_ts_ns{0};
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
  };

  static constexpr size_t kMaxBufferedDeltas = 4096;

  bool parse_depth_update(std::string_view payload, TimestampNs recv_ns, BufferedDelta& out, std::string& err) const;
  void apply_parsed_delta(const BufferedDelta& d);

  std::deque<BufferedDelta> buffer_;
  bool snapshot_requested_{false};
};

// Coinbase Exchange WS Level2 Adapter.
// Rules:
//   1. Initial WS 'snapshot' initializes book & sequence.
//   2. Subsequent 'l2update' and 'heartbeat' must satisfy sequence == last_seq + 1.
//   3. sequence <= last_seq is dropped as stale/duplicate.
//   4. sequence > last_seq + 1 triggers immediate Resyncing + WS reconnect.
class CoinbaseAdapter final : public VenueAdapter {
 public:
  CoinbaseAdapter(const InstrumentSpec& instrument, TimestampNs max_stale_ns, size_t max_depth = 50,
                  ReconnectPolicy reconnect_policy = {});

  AdapterAction on_ws_message(std::string_view payload, TimestampNs recv_ns) override;
  AdapterAction on_rest_snapshot(std::string_view payload, TimestampNs recv_ns) override;
};

// OKX WS v5 Books Adapter with seqId/prevSeqId chaining AND top-25 IEEE CRC32 checksum verification.
// Rules:
//   1. action == "snapshot" resets book, sets last_seq = seqId, verifies CRC32.
//   2. action == "update":
//        - if prevSeqId == seqId: heartbeat/no-op (refreshes liveness)
//        - if seqId <= last_seq: stale duplicate, ignore
//        - if prevSeqId != last_seq: sequence gap -> Resyncing + WS reconnect
//        - apply deltas, recompute top-25 interleaved bid/ask CRC32; mismatch -> Resyncing + WS reconnect.
class OkxAdapter final : public VenueAdapter {
 public:
  OkxAdapter(const InstrumentSpec& instrument, TimestampNs max_stale_ns, size_t max_depth = 50,
             ReconnectPolicy reconnect_policy = {});

  AdapterAction on_ws_message(std::string_view payload, TimestampNs recv_ns) override;
  AdapterAction on_rest_snapshot(std::string_view payload, TimestampNs recv_ns) override;
  AdapterAction on_disconnect(TimestampNs now_ns, std::string_view reason) override;

  // Exposed for testing/synthetic payload generation.
  int32_t compute_top25_checksum() const;

 private:
  struct RawLevelStr {
    std::string price_str;
    std::string qty_str;
  };

  void clear_raw_levels() noexcept;
  void set_raw_level(Side side, PriceTicks price, QtyLots qty, std::string price_str, std::string qty_str);

  std::map<PriceTicks, RawLevelStr, std::greater<PriceTicks>> raw_bids_;
  std::map<PriceTicks, RawLevelStr, std::less<PriceTicks>> raw_asks_;
};

std::unique_ptr<VenueAdapter> make_venue_adapter(VenueId venue, const InstrumentSpec& instrument,
                                                 TimestampNs max_stale_ns, size_t max_depth = 50,
                                                 ReconnectPolicy reconnect_policy = {});

}  // namespace xarb
