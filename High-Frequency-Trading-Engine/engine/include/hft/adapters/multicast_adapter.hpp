#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hft/contracts/v1/events.hpp"
#include "mdf/channel_receiver.hpp"
#include "mdf/exchange_simulator.hpp"
#include "mdf/fault_injector.hpp"
#include "mdf/multicast_transport.hpp"
#include "mdf/order_book.hpp"
#include "mdf/protocol.hpp"
#include "mdf/recovery_service.hpp"

namespace hft::adapters {

// Translates between canonical contracts::v1::MarketDataEvent and Project 06's custom
// MDF1/MDR1 protocol (mdf::MarketEvent, mdf::MulticastFeedPublisher, mdf::ChannelReceiver,
// mdf::RecoveryStore, mdf::FaultInjector).
// Enforces explicit FeedHealth signaling: while receiver state != kLive (gap wait or
// snapshot sync), is_synced() is false so strategies cannot trade on out-of-sync books.
class MulticastFeedAdapter {
 public:
  struct Config {
    contracts::v1::VenueId venue = 0;
    std::uint16_t channel_id = 1;
    std::size_t retransmit_history_packets = 128;
    std::uint64_t checkpoint_every_packets = 16;
    std::size_t max_gap_before_snapshot = 8;
    std::uint64_t gap_timeout_ns = 1'000'000ULL;
    mdf::FaultConfig fault_config{};
  };

  struct DeliveryResult {
    std::uint64_t packets_published = 0;
    std::uint64_t packets_delivered_wire = 0;
    std::uint64_t gaps_detected = 0;
    std::uint64_t retransmit_packets_recovered = 0;
    std::uint64_t snapshots_applied = 0;
    std::uint64_t checksum_errors = 0;
    std::uint64_t duplicate_packets = 0;
    std::uint64_t out_of_order_buffered = 0;
    bool synced = false;
    bool state_converged = false;
    std::uint64_t publisher_state_hash = 0;
    std::uint64_t subscriber_state_hash = 0;
    contracts::v1::FeedHealth health = contracts::v1::FeedHealth::Healthy;
  };

  explicit MulticastFeedAdapter(Config cfg);

  static mdf::MarketEvent to_mdf_event(contracts::v1::InstrumentId instrument,
                                       const contracts::v1::MarketDataEvent& md) noexcept;
  static contracts::v1::MarketDataEvent from_mdf_event(const mdf::MarketEvent& ev,
                                                       std::uint16_t channel,
                                                       std::uint64_t seq,
                                                       contracts::v1::SimTimeNs ts,
                                                       contracts::v1::FeedHealth health) noexcept;

  // Publish canonical market-data envelopes over MDF1 wire encoding, run through Project 06's
  // FaultInjector (drops, corruption, duplicates, reordering), and feed into ChannelReceiver
  // with automatic retransmit & snapshot recovery.
  void publish_batch(std::span<const contracts::v1::Envelope> envelopes,
                     std::uint64_t wire_now_ns);

  // Flush fault injector reorder queue + drive receiver recovery to convergence.
  void flush_and_recover(std::uint64_t wire_now_ns);
  void set_recovery_available(bool available) noexcept {
    recovery_client_.set_available(available);
  }

  [[nodiscard]] bool is_synced() const noexcept;
  [[nodiscard]] contracts::v1::FeedHealth feed_health() const noexcept;
  [[nodiscard]] DeliveryResult summary() const;

  [[nodiscard]] const mdf::ChannelMarketState& publisher_state() const;
  [[nodiscard]] const mdf::ChannelMarketState& subscriber_state() const noexcept;

  // Live UDP loopback multicast + TCP recovery exercise for `network-lab` mode.
  struct LiveNetworkResult {
    bool started = false;
    bool converged = false;
    std::uint64_t packets_published = 0;
    std::uint64_t gaps_detected = 0;
    std::uint64_t retransmit_recovered = 0;
    std::uint64_t snapshots_applied = 0;
    std::uint64_t pub_hash = 0;
    std::uint64_t sub_hash = 0;
    std::string error;
  };
  static LiveNetworkResult run_live_udp_multicast_demo(
      std::span<const contracts::v1::Envelope> envelopes,
      const mdf::FaultConfig& fault_cfg,
      std::uint16_t udp_port = 31041);

 private:
  Config cfg_;
  mdf::RecoveryStore store_;
  mdf::InMemoryRecoveryClient recovery_client_;
  mdf::MulticastFeedPublisher publisher_;
  mdf::ChannelReceiver receiver_;
  std::uint64_t last_wire_ts_ns_ = 0;
  std::uint64_t packets_delivered_wire_ = 0;
};

}  // namespace hft::adapters
