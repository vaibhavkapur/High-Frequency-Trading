#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "mdf/channel_receiver.hpp"
#include "mdf/fault_injector.hpp"
#include "mdf/multicast_transport.hpp"
#include "mdf/order_book.hpp"
#include "mdf/protocol.hpp"
#include "mdf/recovery_service.hpp"

namespace mdf {

struct ExchangeSimConfig {
  uint64_t seed = 1337;
  std::vector<uint32_t> instrument_ids = {1001, 1002, 1003, 1004};
  int64_t initial_mid_ticks = 10000;
  uint32_t target_resting_orders = 64;
};

// Deterministic synthetic exchange order-flow generator producing valid Add/Modify/Cancel/Trade
// event sequences with realistic resting-order lifecycles.
class ExchangeSimulator {
 public:
  explicit ExchangeSimulator(ExchangeSimConfig cfg = {});

  MarketEvent next_event();
  std::vector<MarketEvent> next_batch(std::size_t count);

 private:
  struct ActiveOrder {
    uint32_t instrument_id = 0;
    uint64_t order_id = 0;
    Side side = Side::kBid;
    int64_t price_ticks = 0;
    uint32_t qty = 0;
  };

  ExchangeSimConfig cfg_{};
  std::mt19937_64 rng_;
  uint64_t next_order_id_ = 1;
  std::map<uint32_t, int64_t> mid_ticks_;
  std::vector<ActiveOrder> active_orders_;
};

// Sequenced multicast publisher maintaining authoritative per-channel books, feeding the
// RecoveryStore (retransmit ring + checkpoints), optionally passing packets through a
// FaultInjector, and transmitting via UDP multicast or a custom test sink.
class MulticastFeedPublisher {
 public:
  using PacketSinkFn = std::function<bool(std::span<const uint8_t>)>;

  MulticastFeedPublisher(RecoveryStore& recovery_store, FaultConfig fault_cfg = {});

  bool open_multicast(const MulticastConfig& cfg, std::string* err_out = nullptr);
  void set_custom_sink(PacketSinkFn sink) { custom_sink_ = std::move(sink); }

  void register_channel(uint16_t channel_id);

  // Assign next sequence number, update authoritative book, record in RecoveryStore, and transmit.
  // Returns the assigned sequence number (0 on error).
  uint64_t publish_events(uint16_t channel_id, std::span<const MarketEvent> events);

  // Send a heartbeat packet carrying the channel's latest sequence number (does not advance seq).
  bool publish_heartbeat(uint16_t channel_id);

  // Flush any delayed packets held inside the publisher's FaultInjector.
  void flush_fault_injector();

  void create_checkpoint(uint16_t channel_id);

  const ChannelMarketState& authoritative_state(uint16_t channel_id) const;
  uint64_t last_seq(uint16_t channel_id) const;
  const FaultStats& fault_stats() const noexcept { return fault_injector_.stats(); }
  uint64_t packets_published() const noexcept { return packets_published_; }
  uint64_t events_published() const noexcept { return events_published_; }
  int actual_sndbuf_bytes() const { return sender_.actual_sndbuf_bytes(); }

 private:
  bool emit_raw(std::span<const uint8_t> datagram);

  RecoveryStore& recovery_store_;
  FaultInjector fault_injector_;
  UdpMulticastSender sender_;
  PacketSinkFn custom_sink_;
  std::map<uint16_t, ChannelMarketState> states_;
  std::map<uint16_t, uint64_t> next_seq_;
  uint64_t packets_published_ = 0;
  uint64_t events_published_ = 0;
};

struct SubscriberConfig {
  std::string name = "sub";
  MulticastConfig multicast{};
  std::string recovery_host = "127.0.0.1";
  uint16_t recovery_port = 0;
  int recovery_timeout_ms = 5000;
  std::vector<ChannelReceiverConfig> channels;
  // Optional per-subscriber fault injection applied before ChannelReceiver ingestion.
  FaultConfig ingress_faults{};
  std::size_t recv_batch_size = 16;
  int pin_to_core = -1;
};

// Threaded multicast feed subscriber combining UdpMulticastReceiver, optional per-subscriber
// FaultInjector, TcpRecoveryClient, and per-channel ChannelReceivers.
class FeedSubscriber {
 public:
  explicit FeedSubscriber(SubscriberConfig cfg);
  ~FeedSubscriber();

  FeedSubscriber(const FeedSubscriber&) = delete;
  FeedSubscriber& operator=(const FeedSubscriber&) = delete;

  bool start(std::string* err_out = nullptr);
  void stop();

  bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  uint16_t bound_multicast_port() const noexcept { return udp_receiver_.bound_port(); }
  int actual_rcvbuf_bytes() const { return udp_receiver_.actual_rcvbuf_bytes(); }
  bool busy_poll_active() const noexcept { return udp_receiver_.busy_poll_active(); }
  bool recvmmsg_supported() const noexcept { return udp_receiver_.recvmmsg_supported(); }

  // Wait until `channel_id` reaches LIVE state with `last_applied_seq >= target_seq`.
  bool wait_until_synced(uint16_t channel_id, uint64_t target_seq, int timeout_ms);

  // Thread-safe snapshots of receiver state and stats (safe to call while stopped or running).
  ChannelMarketState copy_market_state(uint16_t channel_id) const;
  ChannelStats channel_stats(uint16_t channel_id) const;
  ReceiverState receiver_state(uint16_t channel_id) const;
  FaultStats ingress_fault_stats() const;
  std::vector<uint64_t> copy_e2e_latencies_ns(uint16_t channel_id) const;
  std::vector<uint64_t> copy_recovery_times_ns(uint16_t channel_id) const;
  std::vector<GapAlarm> copy_gap_alarms() const;

 private:
  void run_loop();
  void ingest_one(std::span<const uint8_t> datagram, uint64_t recv_ts_ns);

  SubscriberConfig cfg_{};
  UdpMulticastReceiver udp_receiver_;
  std::unique_ptr<TcpRecoveryClient> recovery_client_;
  FaultInjector ingress_faults_;
  mutable std::mutex mu_;
  std::map<uint16_t, std::unique_ptr<ChannelReceiver>> receivers_;
  std::vector<GapAlarm> gap_alarms_;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

}  // namespace mdf
