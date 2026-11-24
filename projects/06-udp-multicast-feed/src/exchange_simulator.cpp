#include "mdf/exchange_simulator.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "mdf/socket_utils.hpp"

namespace mdf {

ExchangeSimulator::ExchangeSimulator(ExchangeSimConfig cfg)
    : cfg_(std::move(cfg)), rng_(cfg_.seed) {
  if (cfg_.instrument_ids.empty()) {
    cfg_.instrument_ids = {1001};
  }
  for (std::size_t i = 0; i < cfg_.instrument_ids.size(); ++i) {
    mid_ticks_[cfg_.instrument_ids[i]] =
        cfg_.initial_mid_ticks + static_cast<int64_t>(i) * 500;
  }
}

MarketEvent ExchangeSimulator::next_event() {
  std::uniform_int_distribution<std::size_t> inst_dist(0, cfg_.instrument_ids.size() - 1);
  std::uniform_int_distribution<int> action_dist(0, 99);
  std::uniform_int_distribution<int> spread_dist(1, 8);
  std::uniform_int_distribution<uint32_t> qty_dist(1, 50);

  const bool force_add = active_orders_.size() < cfg_.target_resting_orders / 2;
  const int roll = force_add ? 0 : action_dist(rng_);

  // 45% Add, 20% Modify, 20% Cancel, 15% Trade.
  if (roll < 45 || active_orders_.empty()) {
    const uint32_t inst = cfg_.instrument_ids[inst_dist(rng_)];
    const Side side = (action_dist(rng_) < 50) ? Side::kBid : Side::kAsk;
    int64_t& mid = mid_ticks_[inst];
    const int delta = spread_dist(rng_);
    const int64_t px = (side == Side::kBid) ? (mid - delta) : (mid + delta);
    const uint32_t qty = qty_dist(rng_) * 10U;
    const uint64_t oid = next_order_id_++;

    active_orders_.push_back(ActiveOrder{inst, oid, side, px, qty});
    return MarketEvent{EventType::kAddOrder, inst, oid, side, px, qty};
  }

  std::uniform_int_distribution<std::size_t> ord_dist(0, active_orders_.size() - 1);
  const std::size_t idx = ord_dist(rng_);
  ActiveOrder& ord = active_orders_[idx];

  if (roll < 65) {
    // Modify price and/or quantity.
    std::uniform_int_distribution<int> shift(-2, 2);
    ord.price_ticks += shift(rng_);
    ord.qty = qty_dist(rng_) * 10U;
    return MarketEvent{EventType::kModifyOrder, ord.instrument_id, ord.order_id, ord.side,
                       ord.price_ticks, ord.qty};
  }

  if (roll < 85) {
    // Partial or full cancel.
    const bool partial = (ord.qty > 10 && action_dist(rng_) < 35);
    const uint32_t cancel_qty = partial ? (ord.qty / 2) : ord.qty;
    MarketEvent ev{EventType::kCancelOrder, ord.instrument_id, ord.order_id, ord.side, 0,
                   cancel_qty};
    if (cancel_qty >= ord.qty) {
      active_orders_[idx] = active_orders_.back();
      active_orders_.pop_back();
    } else {
      ord.qty -= cancel_qty;
    }
    return ev;
  }

  // Trade against resting order and nudge mid price.
  const uint32_t exec_qty =
      (ord.qty > 10 && action_dist(rng_) < 50) ? (ord.qty / 2) : ord.qty;
  MarketEvent ev{EventType::kTrade, ord.instrument_id, ord.order_id, ord.side, ord.price_ticks,
                 exec_qty};
  mid_ticks_[ord.instrument_id] = ord.price_ticks;
  if (exec_qty >= ord.qty) {
    active_orders_[idx] = active_orders_.back();
    active_orders_.pop_back();
  } else {
    ord.qty -= exec_qty;
  }
  return ev;
}

std::vector<MarketEvent> ExchangeSimulator::next_batch(std::size_t count) {
  std::vector<MarketEvent> batch;
  batch.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    batch.push_back(next_event());
  }
  return batch;
}

MulticastFeedPublisher::MulticastFeedPublisher(RecoveryStore& recovery_store, FaultConfig fault_cfg)
    : recovery_store_(recovery_store), fault_injector_(std::move(fault_cfg)) {}

bool MulticastFeedPublisher::open_multicast(const MulticastConfig& cfg, std::string* err_out) {
  return sender_.open(cfg, err_out);
}

void MulticastFeedPublisher::register_channel(uint16_t channel_id) {
  states_.try_emplace(channel_id, channel_id);
  next_seq_.try_emplace(channel_id, 1);
  recovery_store_.register_channel(channel_id);
}

bool MulticastFeedPublisher::emit_raw(std::span<const uint8_t> datagram) {
  const auto scheduled = fault_injector_.process(datagram);
  for (const auto& item : scheduled) {
    if (item.jitter_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(item.jitter_us));
    }
    if (custom_sink_) {
      if (!custom_sink_(item.bytes)) return false;
    } else if (sender_.is_open()) {
      if (!sender_.send(item.bytes)) return false;
    }
  }
  return true;
}

uint64_t MulticastFeedPublisher::publish_events(uint16_t channel_id,
                                                std::span<const MarketEvent> events) {
  if (states_.find(channel_id) == states_.end()) {
    register_channel(channel_id);
  }
  const uint64_t seq = next_seq_[channel_id]++;

  PacketHeader hdr{};
  hdr.channel_id = channel_id;
  hdr.seq_num = seq;
  hdr.send_ts_ns = steady_now_ns();

  std::vector<uint8_t> wire;
  if (!encode_packet(hdr, events, wire)) {
    return 0;
  }

  auto& state = states_.at(channel_id);
  state.apply_packet(seq, events);
  recovery_store_.record_packet(channel_id, seq, wire, state);

  ++packets_published_;
  events_published_ += events.size();

  if (!emit_raw(wire)) {
    return 0;
  }
  return seq;
}

bool MulticastFeedPublisher::publish_heartbeat(uint16_t channel_id) {
  if (states_.find(channel_id) == states_.end()) {
    register_channel(channel_id);
  }
  const uint64_t seq = states_.at(channel_id).last_applied_seq();
  std::vector<uint8_t> wire;
  if (!encode_heartbeat(channel_id, seq, steady_now_ns(), wire)) {
    return false;
  }
  return emit_raw(wire);
}

void MulticastFeedPublisher::flush_fault_injector() {
  const auto remaining = fault_injector_.flush();
  for (const auto& item : remaining) {
    if (custom_sink_) {
      (void)custom_sink_(item.bytes);
    } else if (sender_.is_open()) {
      (void)sender_.send(item.bytes);
    }
  }
}

void MulticastFeedPublisher::create_checkpoint(uint16_t channel_id) {
  auto it = states_.find(channel_id);
  if (it != states_.end()) {
    recovery_store_.create_checkpoint(channel_id, it->second);
  }
}

const ChannelMarketState& MulticastFeedPublisher::authoritative_state(uint16_t channel_id) const {
  return states_.at(channel_id);
}

uint64_t MulticastFeedPublisher::last_seq(uint16_t channel_id) const {
  auto it = states_.find(channel_id);
  return (it == states_.end()) ? 0 : it->second.last_applied_seq();
}

FeedSubscriber::FeedSubscriber(SubscriberConfig cfg)
    : cfg_(std::move(cfg)), ingress_faults_(cfg_.ingress_faults) {
  if (cfg_.recovery_port > 0) {
    recovery_client_ = std::make_unique<TcpRecoveryClient>(
        cfg_.recovery_host, cfg_.recovery_port, cfg_.recovery_timeout_ms);
  }
  for (const auto& ch_cfg : cfg_.channels) {
    auto rec = std::make_unique<ChannelReceiver>(ch_cfg, recovery_client_.get());
    rec->set_gap_alarm_callback([this](const GapAlarm& alarm) { gap_alarms_.push_back(alarm); });
    receivers_.emplace(ch_cfg.channel_id, std::move(rec));
  }
}

FeedSubscriber::~FeedSubscriber() { stop(); }

bool FeedSubscriber::start(std::string* err_out) {
  stop();
  if (!udp_receiver_.open(cfg_.multicast, err_out)) {
    return false;
  }
  running_.store(true, std::memory_order_release);
  worker_ = std::thread([this]() { run_loop(); });
  return true;
}

void FeedSubscriber::stop() {
  running_.store(false, std::memory_order_release);
  if (worker_.joinable()) {
    worker_.join();
  }
  udp_receiver_.close();
  if (recovery_client_) {
    recovery_client_->disconnect();
  }
}

void FeedSubscriber::ingest_one(std::span<const uint8_t> datagram, uint64_t recv_ts_ns) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto scheduled = ingress_faults_.process(datagram);
  for (const auto& item : scheduled) {
    if (item.jitter_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(item.jitter_us));
    }
    PacketHeader hdr{};
    const DecodeStatus hst = decode_packet_header(item.bytes, hdr);
    if (hst == DecodeStatus::kOk) {
      auto it = receivers_.find(hdr.channel_id);
      if (it != receivers_.end()) {
        (void)it->second->on_datagram(item.bytes, recv_ts_ns);
      }
    } else if (!receivers_.empty()) {
      // Feed corrupt/malformed datagrams to the first channel receiver so checksum/malformed
      // counters reflect wire corruption visibly.
      (void)receivers_.begin()->second->on_datagram(item.bytes, recv_ts_ns);
    }
  }
}

void FeedSubscriber::run_loop() {
  if (cfg_.pin_to_core >= 0) {
    (void)pin_current_thread_to_core(cfg_.pin_to_core, nullptr);
  }

  std::vector<ReceivedDatagram> batch;
  batch.reserve(std::max<std::size_t>(1, cfg_.recv_batch_size));

  while (running_.load(std::memory_order_acquire)) {
    batch.clear();
    const std::size_t n =
        udp_receiver_.recv_batch(batch, std::max<std::size_t>(1, cfg_.recv_batch_size));
    if (n > 0) {
      for (const auto& d : batch) {
        ingest_one(d.bytes, d.recv_ts_ns);
      }
    } else {
      const uint64_t now_ns = steady_now_ns();
      std::lock_guard<std::mutex> lock(mu_);
      for (auto& [cid, rec] : receivers_) {
        (void)cid;
        rec->tick(now_ns);
      }
      if (cfg_.multicast.non_blocking) {
        std::this_thread::yield();
      }
    }
  }

  // Flush any delayed packets left in the per-subscriber fault injector on clean stop.
  std::lock_guard<std::mutex> lock(mu_);
  const uint64_t now_ns = steady_now_ns();
  for (const auto& item : ingress_faults_.flush()) {
    PacketHeader hdr{};
    if (decode_packet_header(item.bytes, hdr) == DecodeStatus::kOk) {
      auto it = receivers_.find(hdr.channel_id);
      if (it != receivers_.end()) {
        (void)it->second->on_datagram(item.bytes, now_ns);
      }
    }
  }
}

bool FeedSubscriber::wait_until_synced(uint16_t channel_id, uint64_t target_seq, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = receivers_.find(channel_id);
      if (it != receivers_.end() && it->second->state() == ReceiverState::kLive &&
          it->second->market_state().last_applied_seq() >= target_seq) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

ChannelMarketState FeedSubscriber::copy_market_state(uint16_t channel_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return receivers_.at(channel_id)->market_state();
}

ChannelStats FeedSubscriber::channel_stats(uint16_t channel_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return receivers_.at(channel_id)->stats();
}

ReceiverState FeedSubscriber::receiver_state(uint16_t channel_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return receivers_.at(channel_id)->state();
}

FaultStats FeedSubscriber::ingress_fault_stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return ingress_faults_.stats();
}

std::vector<uint64_t> FeedSubscriber::copy_e2e_latencies_ns(uint16_t channel_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return receivers_.at(channel_id)->e2e_latencies_ns();
}

std::vector<uint64_t> FeedSubscriber::copy_recovery_times_ns(uint16_t channel_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return receivers_.at(channel_id)->recovery_times_ns();
}

std::vector<GapAlarm> FeedSubscriber::copy_gap_alarms() const {
  std::lock_guard<std::mutex> lock(mu_);
  return gap_alarms_;
}

}  // namespace mdf
