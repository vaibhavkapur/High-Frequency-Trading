#include "hft/adapters/strategy_adapter.hpp"

#include <cmath>

namespace hft::adapters {

using namespace contracts::v1;

// ---- SimpleDeterministicStrategy ----

SimpleDeterministicStrategy::SimpleDeterministicStrategy(Config cfg) : cfg_(cfg) {}

void SimpleDeterministicStrategy::on_observed_market_data(const IObservedMarketView& view,
                                                          VenueId venue, InstrumentId instrument,
                                                          const MarketDataEvent& /*md*/,
                                                          std::vector<OrderIntent>& out_intents) {
  ++seen_events_;
  if (!view.is_tradable(venue, instrument)) return;

  const TopOfBook tob = view.observed_tob(venue, instrument);
  if (!tob.valid_two_sided()) return;
  const SimTimeNs now = view.now();

  // 1. Trigger an intentional oversize order once to verify risk-gate rejection on the trace.
  if (cfg_.inject_oversize_test_order && !sent_oversize_probe_ && seen_events_ == 6) {
    sent_oversize_probe_ = true;
    OrderIntent bad{};
    bad.action = OrderAction::New;
    bad.client_order_id = next_client_id_++;
    bad.token = ClientToken::from_uint64("SMP", bad.client_order_id);
    bad.side = Side::Buy;
    bad.order_type = OrderType::Limit;
    bad.tif = TimeInForce::GTC;
    bad.limit_price = tob.bid->price;
    bad.qty = Quantity{100'000};  // intentionally exceeds RiskLimits::max_order_qty
    bad.strategy_decision_time_ns = now;
    out_intents.push_back(bad);
  }

  // 2. Submit a crossing IOC buy once to verify partial/full aggressive fill + fee accounting.
  if (!sent_crossing_order_ && seen_events_ >= 8 && tob.ask->qty.lots > 0) {
    sent_crossing_order_ = true;
    OrderIntent take{};
    take.action = OrderAction::New;
    take.client_order_id = next_client_id_++;
    take.token = ClientToken::from_uint64("SMP", take.client_order_id);
    take.side = Side::Buy;
    take.order_type = OrderType::Limit;
    take.tif = TimeInForce::IOC;
    take.limit_price = tob.ask->price;
    take.qty = Quantity{std::min<std::uint64_t>(cfg_.quote_qty.lots + 15, tob.ask->qty.lots + 10)};
    take.strategy_decision_time_ns = now;
    out_intents.push_back(take);
    return;
  }

  // 3. Periodically post or cancel/replace a passive bid quote on observed BBO.
  if (seen_events_ % cfg_.act_every_n_events != 0) return;

  const std::int64_t pos = view.net_position(instrument);
  if (has_active_bid_) {
    OrderIntent cxl{};
    cxl.action = OrderAction::Cancel;
    cxl.client_order_id = next_client_id_++;
    cxl.token = ClientToken::from_uint64("CXL", cxl.client_order_id);
    cxl.orig_token = active_bid_token_;
    cxl.side = Side::Buy;
    cxl.strategy_decision_time_ns = now;
    out_intents.push_back(cxl);
    has_active_bid_ = false;
    return;
  }

  if (pos < cfg_.max_position) {
    OrderIntent passive{};
    passive.action = OrderAction::New;
    passive.client_order_id = next_client_id_++;
    passive.token = ClientToken::from_uint64("SMP", passive.client_order_id);
    passive.side = Side::Buy;
    passive.order_type = OrderType::Limit;
    passive.tif = TimeInForce::GTC;
    passive.post_only = true;
    passive.limit_price = tob.bid->price;
    passive.qty = cfg_.quote_qty;
    passive.strategy_decision_time_ns = now;
    out_intents.push_back(passive);
    active_bid_token_ = passive.token;
    has_active_bid_ = true;
  } else if (pos > -cfg_.max_position) {
    OrderIntent sell{};
    sell.action = OrderAction::New;
    sell.client_order_id = next_client_id_++;
    sell.token = ClientToken::from_uint64("SMP", sell.client_order_id);
    sell.side = Side::Sell;
    sell.order_type = OrderType::Limit;
    sell.tif = TimeInForce::IOC;
    sell.limit_price = tob.bid->price;
    sell.qty = cfg_.quote_qty;
    sell.strategy_decision_time_ns = now;
    out_intents.push_back(sell);
  }
}

void SimpleDeterministicStrategy::on_execution_report(const IObservedMarketView& /*view*/,
                                                      VenueId /*venue*/, InstrumentId /*instrument*/,
                                                      const ExecutionReport& report,
                                                      std::vector<OrderIntent>& /*out_intents*/) {
  if (has_active_bid_ && report.token == active_bid_token_) {
    if (report.type == ExecReportType::Filled || report.type == ExecReportType::Canceled ||
        report.type == ExecReportType::Rejected) {
      has_active_bid_ = false;
    }
  }
}

// ---- MarketMakerPlugin (Project 08 mmsim adapter) ----

MarketMakerPlugin::MarketMakerPlugin(Config cfg)
    : cfg_(std::move(cfg)), impl_(mmsim::make_strategy(cfg_.strategy)) {}

std::string MarketMakerPlugin::name() const { return "mmsim::" + impl_->name(); }

MarketMakerPlugin::InstrumentContext& MarketMakerPlugin::get_context(VenueId venue,
                                                                     InstrumentId instrument) {
  const std::uint64_t key = (static_cast<std::uint64_t>(venue) << 32U) | instrument;
  auto it = contexts_.find(key);
  if (it != contexts_.end()) return it->second;

  InstrumentContext ctx{};
  ctx.observed_book = mmsim::make_book(mmsim::BookImplType::FlatArray);
  ctx.features = std::make_unique<mmsim::FeatureEngine>(cfg_.features);
  auto [ins, _] = contexts_.emplace(key, std::move(ctx));
  return ins->second;
}

void MarketMakerPlugin::on_observed_market_data(const IObservedMarketView& view, VenueId venue,
                                                InstrumentId instrument,
                                                const MarketDataEvent& md,
                                                std::vector<OrderIntent>& out_intents) {
  InstrumentContext& ctx = get_context(venue, instrument);
  const SimTimeNs now = view.now();

  // Synchronize Project 08's observed-only L2 book from the delayed observed market view.
  ctx.observed_book->clear();
  for (Side s : {Side::Buy, Side::Sell}) {
    const auto levels = view.observed_depth(venue, instrument, s, 8);
    const mmsim::Side ms = (s == Side::Buy) ? mmsim::Side::Bid : mmsim::Side::Ask;
    for (const auto& lv : levels) {
      (void)ctx.observed_book->apply_delta(ms, lv.price.ticks, static_cast<mmsim::Qty>(lv.qty.lots));
    }
  }

  mmsim::MarketEvent mev{};
  mev.timestamp_ns = now.value;
  mev.seq_no = ++ctx.md_seq;
  mev.type = (md.kind == MdUpdateKind::Trade) ? mmsim::MarketEventType::Trade
                                              : mmsim::MarketEventType::BookDelta;
  mev.side = (md.side == Side::Buy) ? mmsim::Side::Bid : mmsim::Side::Ask;
  mev.price_ticks = md.price.ticks;
  mev.qty = static_cast<mmsim::Qty>(md.qty.lots);

  last_signals_ = ctx.features->on_event(mev, *ctx.observed_book);
  if (!view.is_tradable(venue, instrument) || !last_signals_.valid) return;

  mmsim::StrategyState st{};
  st.position = view.net_position(instrument);
  st.max_abs_position = cfg_.max_abs_position;
  st.maker_fee_bps = cfg_.maker_fee_bps;

  last_decision_ = impl_->compute_quotes(last_signals_, st);
  ++quote_decisions_;

  auto reconcile_side = [&](ActiveQuote& slot, Side side, bool want, Price px, Quantity qty) {
    if (!want || qty.lots == 0 || !px.valid()) {
      if (slot.live) {
        OrderIntent cxl{};
        cxl.action = OrderAction::Cancel;
        cxl.client_order_id = next_client_id_++;
        cxl.token = ClientToken::from_uint64("MC", cxl.client_order_id);
        cxl.orig_token = slot.token;
        cxl.side = side;
        cxl.strategy_decision_time_ns = now;
        out_intents.push_back(cxl);
        slot.live = false;
      }
      return;
    }

    if (slot.live) {
      const std::int64_t diff = std::abs(slot.price.ticks - px.ticks);
      const bool too_young = (now.value - slot.placed_ns.value) < cfg_.strategy.min_quote_lifetime_ns;
      if (diff < cfg_.strategy.requote_threshold_ticks && slot.qty == qty) return;
      if (too_young && diff < cfg_.strategy.requote_threshold_ticks * 2) return;

      OrderIntent cxl{};
      cxl.action = OrderAction::Cancel;
      cxl.client_order_id = next_client_id_++;
      cxl.token = ClientToken::from_uint64("MC", cxl.client_order_id);
      cxl.orig_token = slot.token;
      cxl.side = side;
      cxl.strategy_decision_time_ns = now;
      out_intents.push_back(cxl);
      slot.live = false;
    }

    OrderIntent ord{};
    ord.action = OrderAction::New;
    ord.client_order_id = next_client_id_++;
    ord.token = ClientToken::from_uint64(cfg_.token_prefix, ord.client_order_id);
    ord.side = side;
    ord.order_type = OrderType::Limit;
    ord.tif = TimeInForce::GTC;
    ord.post_only = cfg_.strategy.post_only;
    ord.limit_price = px;
    ord.qty = qty;
    ord.strategy_decision_time_ns = now;
    out_intents.push_back(ord);

    slot.live = true;
    slot.client_order_id = ord.client_order_id;
    slot.token = ord.token;
    slot.price = px;
    slot.qty = qty;
    slot.placed_ns = now;
  };

  reconcile_side(ctx.bid, Side::Buy, last_decision_.has_bid, Price{last_decision_.bid_price_ticks},
                 Quantity{last_decision_.bid_qty > 0
                              ? static_cast<std::uint64_t>(last_decision_.bid_qty)
                              : 0ULL});
  reconcile_side(ctx.ask, Side::Sell, last_decision_.has_ask, Price{last_decision_.ask_price_ticks},
                 Quantity{last_decision_.ask_qty > 0
                              ? static_cast<std::uint64_t>(last_decision_.ask_qty)
                              : 0ULL});
}

void MarketMakerPlugin::on_execution_report(const IObservedMarketView& /*view*/, VenueId venue,
                                            InstrumentId instrument,
                                            const ExecutionReport& report,
                                            std::vector<OrderIntent>& /*out_intents*/) {
  InstrumentContext& ctx = get_context(venue, instrument);
  auto clear_if_terminal = [&](ActiveQuote& slot) {
    if (slot.live && slot.token == report.token) {
      if (report.type == ExecReportType::Filled || report.type == ExecReportType::Canceled ||
          report.type == ExecReportType::Rejected) {
        slot.live = false;
      }
    }
  };
  clear_if_terminal(ctx.bid);
  clear_if_terminal(ctx.ask);
}

}  // namespace hft::adapters
