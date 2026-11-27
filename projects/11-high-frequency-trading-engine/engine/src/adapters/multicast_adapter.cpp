#include "hft/adapters/multicast_adapter.hpp"

#include <chrono>
#include <thread>

namespace hft::adapters {

using namespace contracts::v1;

namespace {

mdf::RecoveryStoreConfig make_store_cfg(const MulticastFeedAdapter::Config& cfg) {
  mdf::RecoveryStoreConfig rc{};
  rc.retransmit_buffer_packets = cfg.retransmit_history_packets;
  rc.checkpoint_interval = cfg.checkpoint_every_packets;
  return rc;
}

mdf::ChannelReceiverConfig make_recv_cfg(const MulticastFeedAdapter::Config& cfg) {
  mdf::ChannelReceiverConfig rc{};
  rc.channel_id = cfg.channel_id;
  rc.reorder_timeout_us = std::max<std::uint64_t>(1, cfg.gap_timeout_ns / 1000ULL);
  rc.reorder_window_packets = static_cast<std::uint32_t>(cfg.max_gap_before_snapshot);
  return rc;
}

}  // namespace

MulticastFeedAdapter::MulticastFeedAdapter(Config cfg)
    : cfg_(std::move(cfg)),
      store_(make_store_cfg(cfg_)),
      recovery_client_(store_),
      publisher_(store_, cfg_.fault_config),
      receiver_(make_recv_cfg(cfg_), &recovery_client_) {
  publisher_.register_channel(cfg_.channel_id);
  publisher_.set_custom_sink([this](std::span<const std::uint8_t> datagram) {
    ++packets_delivered_wire_;
    (void)receiver_.on_datagram(datagram, last_wire_ts_ns_);
    return true;
  });
}

mdf::MarketEvent MulticastFeedAdapter::to_mdf_event(InstrumentId instrument,
                                                    const MarketDataEvent& md) noexcept {
  mdf::MarketEvent ev{};
  ev.instrument_id = instrument;
  ev.order_id = md.feed_order_ref.value;
  ev.side = (md.side == Side::Buy) ? mdf::Side::kBid : mdf::Side::kAsk;
  ev.price_ticks = md.price.ticks;
  ev.qty = static_cast<std::uint32_t>(std::min<std::uint64_t>(md.qty.lots, 0xFFFFFFFFULL));
  switch (md.kind) {
    case MdUpdateKind::Add:
    case MdUpdateKind::SnapshotLevel:
      ev.type = mdf::EventType::kAddOrder;
      break;
    case MdUpdateKind::Modify:
      ev.type = mdf::EventType::kModifyOrder;
      break;
    case MdUpdateKind::Cancel:
    case MdUpdateKind::Delete:
      ev.type = mdf::EventType::kCancelOrder;
      if (md.kind == MdUpdateKind::Delete && ev.qty == 0) {
        ev.qty = 0xFFFFFFFFU;  // remove full resting order
      }
      break;
    case MdUpdateKind::Trade:
      ev.type = mdf::EventType::kTrade;
      break;
    case MdUpdateKind::SnapshotClear:
    case MdUpdateKind::BookClear:
      ev.type = mdf::EventType::kClearBook;
      break;
  }
  return ev;
}

MarketDataEvent MulticastFeedAdapter::from_mdf_event(const mdf::MarketEvent& ev,
                                                     std::uint16_t channel, std::uint64_t seq,
                                                     SimTimeNs ts, FeedHealth health) noexcept {
  MarketDataEvent md{};
  md.channel = channel;
  md.feed_seq = seq;
  md.source_time_ns = ts;
  md.receive_time_ns = ts;
  md.feed_order_ref = FeedOrderRef{ev.order_id};
  md.side = (ev.side == mdf::Side::kBid) ? Side::Buy : Side::Sell;
  md.price = Price{ev.price_ticks};
  md.qty = Quantity{ev.qty};
  md.health = health;
  switch (ev.type) {
    case mdf::EventType::kAddOrder: md.kind = MdUpdateKind::Add; break;
    case mdf::EventType::kModifyOrder: md.kind = MdUpdateKind::Modify; break;
    case mdf::EventType::kCancelOrder: md.kind = MdUpdateKind::Cancel; break;
    case mdf::EventType::kTrade: md.kind = MdUpdateKind::Trade; break;
    case mdf::EventType::kClearBook: md.kind = MdUpdateKind::BookClear; break;
  }
  return md;
}

void MulticastFeedAdapter::publish_batch(std::span<const Envelope> envelopes,
                                         std::uint64_t wire_now_ns) {
  last_wire_ts_ns_ = wire_now_ns;
  std::vector<mdf::MarketEvent> batch;
  batch.reserve(envelopes.size());
  for (const auto& env : envelopes) {
    if (const auto* md = std::get_if<MarketDataEvent>(&env.payload)) {
      batch.push_back(to_mdf_event(env.instrument, *md));
    }
  }
  if (batch.empty()) return;
  (void)publisher_.publish_events(cfg_.channel_id, batch);
  receiver_.tick(last_wire_ts_ns_);
}

void MulticastFeedAdapter::flush_and_recover(std::uint64_t wire_now_ns) {
  last_wire_ts_ns_ = wire_now_ns;
  publisher_.flush_fault_injector();
  (void)publisher_.publish_heartbeat(cfg_.channel_id);
  // Advance receiver recovery timers so any pending gap triggers retransmit or snapshot sync.
  for (int step = 0; step < 8; ++step) {
    last_wire_ts_ns_ += cfg_.gap_timeout_ns + 1000ULL;
    receiver_.tick(last_wire_ts_ns_);
    (void)receiver_.flush_recovery(last_wire_ts_ns_);
    if (receiver_.state() == mdf::ReceiverState::kLive &&
        receiver_.market_state().last_applied_seq() >= publisher_.last_seq(cfg_.channel_id)) {
      break;
    }
  }
}

bool MulticastFeedAdapter::is_synced() const noexcept {
  return receiver_.state() == mdf::ReceiverState::kLive;
}

FeedHealth MulticastFeedAdapter::feed_health() const noexcept {
  switch (receiver_.state()) {
    case mdf::ReceiverState::kLive: return FeedHealth::Healthy;
    case mdf::ReceiverState::kGapWait: return FeedHealth::GapDetected;
    case mdf::ReceiverState::kSyncingSnapshot: return FeedHealth::Resyncing;
  }
  return FeedHealth::Stale;
}

MulticastFeedAdapter::DeliveryResult MulticastFeedAdapter::summary() const {
  DeliveryResult res{};
  const auto st = receiver_.stats();
  const auto& pub = publisher_.authoritative_state(cfg_.channel_id);
  const auto& sub = receiver_.market_state();
  res.packets_published = publisher_.packets_published();
  res.packets_delivered_wire = packets_delivered_wire_;
  res.gaps_detected = st.gaps_detected;
  res.retransmit_packets_recovered = st.retransmit_packets_recovered;
  res.snapshots_applied = st.snapshots_applied;
  res.checksum_errors = st.checksum_errors;
  res.duplicate_packets = st.duplicate_packets;
  res.out_of_order_buffered = st.out_of_order_packets;
  res.synced = is_synced();
  res.publisher_state_hash = pub.state_hash();
  res.subscriber_state_hash = sub.state_hash();
  res.state_converged = (pub == sub && res.publisher_state_hash == res.subscriber_state_hash);
  res.health = feed_health();
  return res;
}

const mdf::ChannelMarketState& MulticastFeedAdapter::publisher_state() const {
  return publisher_.authoritative_state(cfg_.channel_id);
}

const mdf::ChannelMarketState& MulticastFeedAdapter::subscriber_state() const noexcept {
  return receiver_.market_state();
}

MulticastFeedAdapter::LiveNetworkResult MulticastFeedAdapter::run_live_udp_multicast_demo(
    std::span<const Envelope> envelopes, const mdf::FaultConfig& fault_cfg,
    std::uint16_t udp_port) {
  LiveNetworkResult res{};

  mdf::RecoveryStoreConfig scfg{};
  scfg.retransmit_buffer_packets = 64;
  scfg.checkpoint_interval = 8;
  mdf::RecoveryStore store(scfg);
  store.register_channel(1);

  mdf::TcpRecoveryServer tcp_srv(store);
  mdf::TcpRecoveryServerConfig tcfg{};
  tcfg.bind_ip = "127.0.0.1";
  tcfg.port = 0;
  std::string err;
  if (!tcp_srv.start(tcfg, &err)) {
    res.error = "TcpRecoveryServer::start failed: " + err;
    return res;
  }

  mdf::MulticastConfig mcfg{};
  mcfg.group_ip = "239.255.0.41";
  mcfg.port = udp_port;
  mcfg.interface_ip = "127.0.0.1";
  mcfg.loopback = true;
  mcfg.ttl = 1;

  mdf::SubscriberConfig sub_cfg{};
  sub_cfg.multicast = mcfg;
  sub_cfg.recovery_host = "127.0.0.1";
  sub_cfg.recovery_port = tcp_srv.bound_port();
  mdf::ChannelReceiverConfig ch_cfg{};
  ch_cfg.channel_id = 1;
  ch_cfg.reorder_timeout_us = 15'000ULL;
  ch_cfg.reorder_window_packets = 12;
  sub_cfg.channels.push_back(ch_cfg);
  sub_cfg.ingress_faults = fault_cfg;

  mdf::FeedSubscriber subscriber(sub_cfg);
  if (!subscriber.start(&err)) {
    tcp_srv.stop();
    res.error = "FeedSubscriber::start failed: " + err;
    return res;
  }

  mdf::MulticastFeedPublisher pub(store, mdf::FaultConfig{});
  pub.register_channel(1);
  if (!pub.open_multicast(mcfg, &err)) {
    subscriber.stop();
    tcp_srv.stop();
    res.error = "MulticastFeedPublisher::open_multicast failed: " + err;
    return res;
  }
  res.started = true;

  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  std::vector<mdf::MarketEvent> evs;
  evs.reserve(envelopes.size());
  for (const auto& env : envelopes) {
    if (const auto* md = std::get_if<MarketDataEvent>(&env.payload)) {
      evs.push_back(to_mdf_event(env.instrument, *md));
    }
  }
  const std::size_t chunk = 4;
  for (std::size_t i = 0; i < evs.size(); i += chunk) {
    const std::size_t n = std::min(chunk, evs.size() - i);
    (void)pub.publish_events(1, std::span<const mdf::MarketEvent>(evs.data() + i, n));
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
  for (int h = 0; h < 4; ++h) {
    (void)pub.publish_heartbeat(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }

  const std::uint64_t target_seq = pub.last_seq(1);
  (void)subscriber.wait_until_synced(1, target_seq, 2000);

  const auto pub_state = pub.authoritative_state(1);
  const auto sub_state = subscriber.copy_market_state(1);
  const auto stats = subscriber.channel_stats(1);
  res.packets_published = pub.packets_published();
  res.gaps_detected = stats.gaps_detected;
  res.retransmit_recovered = stats.retransmit_packets_recovered;
  res.snapshots_applied = stats.snapshots_applied;
  res.pub_hash = pub_state.state_hash();
  res.sub_hash = sub_state.state_hash();
  res.converged = (pub_state == sub_state && res.pub_hash == res.sub_hash);

  subscriber.stop();
  tcp_srv.stop();
  return res;
}

}  // namespace hft::adapters
