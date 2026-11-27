#include "hft/adapters/itch_adapter.hpp"

namespace hft::adapters {

using namespace contracts::v1;

ItchFeedAdapter::ItchFeedAdapter(VenueId venue, std::vector<InstrumentMeta> instruments,
                                 std::uint16_t channel)
    : venue_(venue), channel_(channel), instruments_(std::move(instruments)) {
  // Default mapping: ITCH synth locates are 1-based (1 -> InstrumentId 0, etc.).
  for (std::size_t i = 0; i < instruments_.size(); ++i) {
    locate_to_inst_[static_cast<std::uint16_t>(i + 1)] = static_cast<InstrumentId>(i);
  }
}

void ItchFeedAdapter::bind_locate(std::uint16_t stock_locate, InstrumentId instrument) {
  locate_to_inst_[stock_locate] = instrument;
}

std::optional<InstrumentId> ItchFeedAdapter::resolve_locate(std::uint16_t locate) const noexcept {
  const auto it = locate_to_inst_.find(locate);
  if (it == locate_to_inst_.end()) return std::nullopt;
  if (it->second >= instruments_.size()) return std::nullopt;
  return it->second;
}

ErrorCode ItchFeedAdapter::decode_payload(std::span<const std::uint8_t> payload,
                                          SimTimeNs receive_time_ns,
                                          std::vector<Envelope>& out_events) {
  const auto ref_res = ref_handler_.process(payload);
  if (ref_res.status != itch::DecodeStatus::Ok) {
    ++stats_.decode_errors;
    return ErrorCode::DecodeError;
  }
  ++stats_.frames_decoded;

  auto emit_md = [&](InstrumentId inst, const MarketDataEvent& md) {
    Envelope env{};
    env.contract_version = kContractVersion;
    env.sequence = next_seq_++;
    env.event_time_ns = md.source_time_ns;
    env.priority = PriorityClass::MarketDataDelta;
    env.instrument = inst;
    env.venue = venue_;
    env.payload = md;
    out_events.push_back(env);
    ++stats_.canonical_events_emitted;
  };

  auto handle_add = [&](std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, itch::Side s,
                        std::uint32_t shares, itch::Price4 price4) {
    const auto inst = resolve_locate(locate);
    if (!inst.has_value()) {
      ++stats_.unmapped_locates;
      return;
    }
    const auto px = instruments_[*inst].price4_to_price(price4);
    const auto qty = instruments_[*inst].shares_to_qty(shares);
    if (!px.has_value() || !qty.has_value()) {
      ++stats_.scale_alignment_rejects;
      return;
    }
    const Side side = (s == itch::Side::Buy) ? Side::Buy : Side::Sell;
    feed_orders_[ref] = FeedOrderMeta{*inst, side, *px};

    MarketDataEvent md{};
    md.kind = MdUpdateKind::Add;
    md.channel = channel_;
    md.feed_seq = next_seq_;
    md.source_time_ns = SimTimeNs{static_cast<std::int64_t>(ts)};
    md.receive_time_ns = receive_time_ns.value > 0 ? receive_time_ns : md.source_time_ns;
    md.feed_order_ref = FeedOrderRef{ref};
    md.side = side;
    md.price = *px;
    md.qty = *qty;
    emit_md(*inst, md);
  };

  const auto st = itch::decode(payload, [&](const auto& m) {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, itch::AddOrder>) {
      handle_add(m.hdr.stock_locate, m.hdr.timestamp, m.order_ref, m.side, m.shares, m.price);
    } else if constexpr (std::is_same_v<T, itch::AddOrderMpid>) {
      handle_add(m.hdr.stock_locate, m.hdr.timestamp, m.order_ref, m.side, m.shares, m.price);
    } else if constexpr (std::is_same_v<T, itch::OrderCancel>) {
      const auto oit = feed_orders_.find(m.order_ref);
      if (oit == feed_orders_.end()) return;
      const auto qty = instruments_[oit->second.instrument].shares_to_qty(m.cancelled_shares);
      if (!qty.has_value()) {
        ++stats_.scale_alignment_rejects;
        return;
      }
      MarketDataEvent md{};
      md.kind = MdUpdateKind::Cancel;
      md.channel = channel_;
      md.feed_seq = next_seq_;
      md.source_time_ns = SimTimeNs{static_cast<std::int64_t>(m.hdr.timestamp)};
      md.receive_time_ns = receive_time_ns.value > 0 ? receive_time_ns : md.source_time_ns;
      md.feed_order_ref = FeedOrderRef{m.order_ref};
      md.side = oit->second.side;
      md.price = oit->second.price;
      md.qty = *qty;
      emit_md(oit->second.instrument, md);
    } else if constexpr (std::is_same_v<T, itch::OrderDelete>) {
      const auto oit = feed_orders_.find(m.order_ref);
      if (oit == feed_orders_.end()) return;
      MarketDataEvent md{};
      md.kind = MdUpdateKind::Delete;
      md.channel = channel_;
      md.feed_seq = next_seq_;
      md.source_time_ns = SimTimeNs{static_cast<std::int64_t>(m.hdr.timestamp)};
      md.receive_time_ns = receive_time_ns.value > 0 ? receive_time_ns : md.source_time_ns;
      md.feed_order_ref = FeedOrderRef{m.order_ref};
      md.side = oit->second.side;
      md.price = oit->second.price;
      emit_md(oit->second.instrument, md);
      feed_orders_.erase(oit);
    } else if constexpr (std::is_same_v<T, itch::OrderExecuted>) {
      const auto oit = feed_orders_.find(m.order_ref);
      if (oit == feed_orders_.end()) return;
      const auto qty = instruments_[oit->second.instrument].shares_to_qty(m.executed_shares);
      if (!qty.has_value()) {
        ++stats_.scale_alignment_rejects;
        return;
      }
      MarketDataEvent md{};
      md.kind = MdUpdateKind::Trade;
      md.channel = channel_;
      md.feed_seq = next_seq_;
      md.source_time_ns = SimTimeNs{static_cast<std::int64_t>(m.hdr.timestamp)};
      md.receive_time_ns = receive_time_ns.value > 0 ? receive_time_ns : md.source_time_ns;
      md.feed_order_ref = FeedOrderRef{m.order_ref};
      md.side = oit->second.side;
      md.price = oit->second.price;
      md.qty = *qty;
      emit_md(oit->second.instrument, md);
    } else if constexpr (std::is_same_v<T, itch::OrderExecutedWithPrice>) {
      const auto oit = feed_orders_.find(m.order_ref);
      if (oit == feed_orders_.end()) return;
      const auto px = instruments_[oit->second.instrument].price4_to_price(m.execution_price);
      const auto qty = instruments_[oit->second.instrument].shares_to_qty(m.executed_shares);
      if (!px.has_value() || !qty.has_value()) {
        ++stats_.scale_alignment_rejects;
        return;
      }
      MarketDataEvent md{};
      md.kind = MdUpdateKind::Trade;
      md.channel = channel_;
      md.feed_seq = next_seq_;
      md.source_time_ns = SimTimeNs{static_cast<std::int64_t>(m.hdr.timestamp)};
      md.receive_time_ns = receive_time_ns.value > 0 ? receive_time_ns : md.source_time_ns;
      md.feed_order_ref = FeedOrderRef{m.order_ref};
      md.side = oit->second.side;
      md.price = *px;
      md.qty = *qty;
      emit_md(oit->second.instrument, md);
    } else if constexpr (std::is_same_v<T, itch::OrderReplace>) {
      const auto oit = feed_orders_.find(m.original_order_ref);
      if (oit == feed_orders_.end()) return;
      const InstrumentId inst = oit->second.instrument;
      const Side side = oit->second.side;
      const auto px = instruments_[inst].price4_to_price(m.price);
      const auto qty = instruments_[inst].shares_to_qty(m.shares);
      if (!px.has_value() || !qty.has_value()) {
        ++stats_.scale_alignment_rejects;
        return;
      }
      // Represent ITCH replace (which allocates a new order_ref and loses queue priority)
      // as canonical Delete(old_ref) + Add(new_ref) so all downstream books stay consistent.
      MarketDataEvent del{};
      del.kind = MdUpdateKind::Delete;
      del.channel = channel_;
      del.feed_seq = next_seq_;
      del.source_time_ns = SimTimeNs{static_cast<std::int64_t>(m.hdr.timestamp)};
      del.receive_time_ns = receive_time_ns.value > 0 ? receive_time_ns : del.source_time_ns;
      del.feed_order_ref = FeedOrderRef{m.original_order_ref};
      del.side = side;
      del.price = oit->second.price;
      emit_md(inst, del);
      feed_orders_.erase(oit);

      feed_orders_[m.new_order_ref] = FeedOrderMeta{inst, side, *px};
      MarketDataEvent add{};
      add.kind = MdUpdateKind::Add;
      add.channel = channel_;
      add.feed_seq = next_seq_;
      add.source_time_ns = SimTimeNs{static_cast<std::int64_t>(m.hdr.timestamp)};
      add.receive_time_ns = receive_time_ns.value > 0 ? receive_time_ns : add.source_time_ns;
      add.feed_order_ref = FeedOrderRef{m.new_order_ref};
      add.side = side;
      add.price = *px;
      add.qty = *qty;
      emit_md(inst, add);
    }
  });

  if (st != itch::DecodeStatus::Ok) {
    ++stats_.decode_errors;
    return ErrorCode::DecodeError;
  }
  return ErrorCode::Ok;
}

ErrorCode ItchFeedAdapter::decode_stream(std::span<const std::uint8_t> framed_stream,
                                         std::vector<Envelope>& out_events,
                                         std::size_t max_messages) {
  itch::LengthPrefixedReader reader(framed_stream);
  std::span<const std::uint8_t> payload;
  std::size_t count = 0;
  while (true) {
    if (max_messages > 0 && count >= max_messages) break;
    const auto fstatus = reader.next(payload);
    if (fstatus == itch::FrameStatus::End) break;
    if (fstatus == itch::FrameStatus::ZeroLength) continue;
    if (fstatus != itch::FrameStatus::Ok) {
      ++stats_.decode_errors;
      return ErrorCode::DecodeError;
    }
    const ErrorCode rc = decode_payload(payload, SimTimeNs{0}, out_events);
    if (rc != ErrorCode::Ok) return rc;
    ++count;
  }
  return ErrorCode::Ok;
}

std::vector<std::uint8_t> ItchFeedAdapter::generate_synthetic_stream(std::uint64_t messages,
                                                                     std::uint32_t symbols,
                                                                     std::uint64_t seed) {
  itch::SynthConfig cfg{};
  cfg.messages = messages;
  cfg.symbols = symbols;
  cfg.seed = seed;
  cfg.profile = itch::SynthProfile::Balanced;
  return itch::generate_synthetic(cfg);
}

TopOfBook ItchFeedAdapter::reference_tob(std::uint16_t stock_locate) const {
  TopOfBook tob{};
  const auto inst = resolve_locate(stock_locate);
  if (!inst.has_value()) return tob;
  const itch::Bbo bbo = ref_handler_.books().bbo(stock_locate);
  if (bbo.has_bid) {
    const auto px = instruments_[*inst].price4_to_price(bbo.bid_price);
    const auto qty = instruments_[*inst].shares_to_qty(bbo.bid_qty);
    if (px.has_value() && qty.has_value()) {
      tob.bid = LevelView{*px, *qty, 1};
    }
  }
  if (bbo.has_ask) {
    const auto px = instruments_[*inst].price4_to_price(bbo.ask_price);
    const auto qty = instruments_[*inst].shares_to_qty(bbo.ask_qty);
    if (px.has_value() && qty.has_value()) {
      tob.ask = LevelView{*px, *qty, 1};
    }
  }
  return tob;
}

}  // namespace hft::adapters
