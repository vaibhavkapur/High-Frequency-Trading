#include "hft/adapters/backtester_adapter.hpp"

namespace hft::adapters {

using namespace contracts::v1;

CausalLatencyAdapter::CausalLatencyAdapter(std::int64_t md_base_ns, std::int64_t md_jitter_ns,
                                           std::int64_t order_base_ns, std::int64_t order_jitter_ns,
                                           std::int64_t resp_base_ns, std::int64_t resp_jitter_ns,
                                           std::uint64_t seed)
    : md_path_(md_base_ns, md_jitter_ns, seed ^ 0x11111111ULL),
      order_path_(order_base_ns, order_jitter_ns, seed ^ 0x22222222ULL),
      resp_path_(resp_base_ns, resp_jitter_ns, seed ^ 0x33333333ULL) {}

SimTimeNs CausalLatencyAdapter::deliver_md(SimTimeNs source_ns) noexcept {
  return SimTimeNs{md_path_.deliver_at(source_ns.value)};
}

SimTimeNs CausalLatencyAdapter::deliver_order(SimTimeNs sent_ns) noexcept {
  return SimTimeNs{order_path_.deliver_at(sent_ns.value)};
}

SimTimeNs CausalLatencyAdapter::deliver_response(SimTimeNs exch_ns) noexcept {
  return SimTimeNs{resp_path_.deliver_at(exch_ns.value)};
}

std::vector<Envelope> BacktesterFeedAdapter::generate_l3_envelopes(VenueId venue,
                                                                   std::size_t event_count,
                                                                   std::uint32_t instruments,
                                                                   std::uint64_t seed,
                                                                   std::int64_t start_price_ticks) {
  hftbt::SynthConfig cfg{};
  cfg.events = event_count;
  cfg.instruments = instruments;
  cfg.seed = seed;
  cfg.initial_mid = start_price_ticks;
  const hftbt::FeedData feed = hftbt::generate_l3(cfg);
  return from_hftbt_feed(venue, feed);
}

std::vector<Envelope> BacktesterFeedAdapter::from_hftbt_feed(VenueId venue,
                                                             const hftbt::FeedData& feed) {
  std::vector<Envelope> out;
  out.reserve(feed.events.size());
  EventSeq seq = 1;
  for (const auto& ev : feed.events) {
    MarketDataEvent md{};
    switch (ev.type) {
      case hftbt::MdType::Add: md.kind = MdUpdateKind::Add; break;
      case hftbt::MdType::Cancel: md.kind = MdUpdateKind::Cancel; break;
      case hftbt::MdType::Delete: md.kind = MdUpdateKind::Delete; break;
      case hftbt::MdType::Execute:
      case hftbt::MdType::Trade: md.kind = MdUpdateKind::Trade; break;
      case hftbt::MdType::Level: md.kind = MdUpdateKind::SnapshotLevel; break;
    }
    md.channel = 1;
    md.feed_seq = seq;
    md.source_time_ns = SimTimeNs{ev.ts};
    md.receive_time_ns = SimTimeNs{ev.ts};
    md.feed_order_ref = FeedOrderRef{ev.order_id};
    md.side = (ev.side == hftbt::Side::Buy) ? Side::Buy : Side::Sell;
    md.price = Price{ev.price};
    md.qty = Quantity{ev.qty > 0 ? static_cast<std::uint64_t>(ev.qty) : 0ULL};

    Envelope env{};
    env.contract_version = kContractVersion;
    env.sequence = seq++;
    env.event_time_ns = md.source_time_ns;
    env.priority = PriorityClass::MarketDataDelta;
    env.instrument = ev.instrument;
    env.venue = venue;
    env.payload = md;
    out.push_back(env);
  }
  return out;
}

}  // namespace hft::adapters
