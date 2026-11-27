#include "hft/adapters/matching_adapter.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace hft::adapters {

using namespace contracts::v1;

namespace {

constexpr me::Side to_me_side(Side s) noexcept {
  return s == Side::Buy ? me::Side::Buy : me::Side::Sell;
}

constexpr RejectReason map_me_reject(me::RejectReason r) noexcept {
  switch (r) {
    case me::RejectReason::None: return RejectReason::None;
    case me::RejectReason::UnknownSymbol: return RejectReason::UnknownSymbol;
    case me::RejectReason::InvalidPrice: return RejectReason::InvalidPrice;
    case me::RejectReason::InvalidTick: return RejectReason::InvalidTick;
    case me::RejectReason::InvalidQuantity: return RejectReason::InvalidQty;
    case me::RejectReason::DuplicateOrderId: return RejectReason::DuplicateToken;
    case me::RejectReason::UnknownOrderId: return RejectReason::UnknownOrder;
    default: return RejectReason::ExchangeRejected;
  }
}

void hash_u64(std::uint64_t& h, std::uint64_t v) noexcept {
  for (unsigned i = 0; i < 8; ++i) {
    h ^= static_cast<std::uint8_t>((v >> (i * 8U)) & 0xFFU);
    h *= 1099511628211ULL;
  }
}

}  // namespace

MatchingEngineAdapter::MatchingEngineAdapter(VenueMeta venue,
                                             std::vector<InstrumentMeta> instruments,
                                             BookBackend backend,
                                             std::uint32_t max_orders_per_book)
    : venue_(std::move(venue)), instruments_(std::move(instruments)), backend_(backend) {
  me::EngineConfig cfg;
  cfg.max_orders_per_book = max_orders_per_book;
  for (const auto& inst : instruments_) {
    me::SymbolConfig sc;
    sc.tick_size = 1;
    sc.lot_size = 1;
    sc.max_price = inst.max_price_ticks;
    sc.max_qty = static_cast<me::Qty>(
        std::min<std::uint64_t>(inst.max_qty_lots, 1'000'000'000ULL));
    cfg.symbols.push_back(sc);
  }
  if (cfg.symbols.empty()) {
    cfg.symbols.push_back(me::SymbolConfig{});
  }
  if (backend_ == BookBackend::Pool) {
    pool_engine_ = std::make_unique<me::Engine<me::PoolBook>>(cfg);
  } else {
    map_engine_ = std::make_unique<me::Engine<me::MapBook>>(cfg);
  }
}

template <class Sink>
void MatchingEngineAdapter::process_me_command(const me::Command& cmd, Sink&& sink) {
  if (backend_ == BookBackend::Pool) {
    pool_engine_->process(cmd, std::forward<Sink>(sink));
  } else {
    map_engine_->process(cmd, std::forward<Sink>(sink));
  }
}

Money MatchingEngineAdapter::compute_fee_for_fill(InstrumentId instrument, Price price, Quantity qty,
                                                  bool is_maker) const noexcept {
  if (instrument >= instruments_.size()) return Money{0};
  const auto notional = instruments_[instrument].notional_scaled(price, qty);
  if (!notional.has_value()) return Money{0};
  const std::int32_t bps_x100 =
      is_maker ? venue_.maker_fee_bps_x100 : venue_.taker_fee_bps_x100;
  const auto fee = InstrumentMeta::compute_fee(*notional, bps_x100);
  return fee.value_or(Money{0});
}

void MatchingEngineAdapter::apply_market_event(InstrumentId instrument, const MarketDataEvent& md,
                                               SimTimeNs now_ns) {
  std::vector<ExecutionReport> ignored;
  apply_market_event_with_fills(instrument, md, now_ns, ignored);
}

void MatchingEngineAdapter::apply_market_event_with_fills(
    InstrumentId instrument, const MarketDataEvent& md, SimTimeNs now_ns,
    std::vector<ExecutionReport>& out_passive_fills) {
  if (instrument >= instruments_.size()) return;
  const std::uint64_t feed_id = kFeedOrderMask | (md.feed_order_ref.value & ~kFeedOrderMask);

  auto handle_report = [&](const me::Report& r) {
    if (r.type != me::ReportType::Trade) return;
    // Check whether maker (other_id) is one of our resting strategy orders.
    auto it = by_exchange_id_.find(r.other_id);
    if (it == by_exchange_id_.end()) return;
    TrackedStrategyOrder& ord = it->second;
    const std::uint64_t fill_lots = static_cast<std::uint64_t>(r.qty);
    ord.cum_filled.lots += fill_lots;
    ord.leaves.lots = static_cast<std::uint64_t>(r.other_leaves);
    ord.live = (ord.leaves.lots > 0);

    ExecutionReport er{};
    er.type = ord.live ? ExecReportType::PartiallyFilled : ExecReportType::Filled;
    er.client_order_id = ord.client_order_id;
    er.token = ord.token;
    er.exchange_order_id = ord.exchange_id;
    er.exec_id = next_exec_id_++;
    er.side = ord.side;
    er.exec_price = Price{r.price};
    er.requested_qty = ord.requested_qty;
    er.fill_qty = Quantity{fill_lots};
    er.cum_filled_qty = ord.cum_filled;
    er.leaves_qty = ord.leaves;
    er.is_maker = true;
    er.fee_scaled = compute_fee_for_fill(instrument, er.exec_price, er.fill_qty, true);
    er.exchange_time_ns = now_ns;
    out_passive_fills.push_back(er);
  };

  me::Command cmd{};
  cmd.symbol = instrument;
  cmd.order_id = feed_id;
  cmd.side = to_me_side(md.side);

  switch (md.kind) {
    case MdUpdateKind::Add:
    case MdUpdateKind::SnapshotLevel: {
      if (md.price.ticks <= 0 || md.qty.lots == 0) return;
      cmd.type = me::CommandType::New;
      cmd.price = md.price.ticks;
      cmd.qty = static_cast<me::Qty>(md.qty.lots);
      process_me_command(cmd, handle_report);
      break;
    }
    case MdUpdateKind::Delete: {
      cmd.type = me::CommandType::Cancel;
      process_me_command(cmd, [](const me::Report&) {});
      break;
    }
    case MdUpdateKind::Cancel: {
      // md.qty represents decrement delta in canonical MD events (matching ITCH OrderCancel).
      std::optional<me::OrderInfo> info = (backend_ == BookBackend::Pool)
                                              ? pool_engine_->book(instrument).find(feed_id)
                                              : map_engine_->book(instrument).find(feed_id);
      if (!info.has_value()) return;
      const me::Qty dec = static_cast<me::Qty>(md.qty.lots);
      if (dec <= 0) return;
      if (dec >= info->leaves) {
        cmd.type = me::CommandType::Cancel;
      } else {
        cmd.type = me::CommandType::Reduce;
        cmd.qty = info->leaves - dec;
      }
      process_me_command(cmd, [](const me::Report&) {});
      break;
    }
    case MdUpdateKind::Modify: {
      if (md.price.ticks <= 0 || md.qty.lots == 0) {
        cmd.type = me::CommandType::Cancel;
        process_me_command(cmd, [](const me::Report&) {});
      } else {
        cmd.type = me::CommandType::Replace;
        cmd.new_order_id = feed_id;
        cmd.price = md.price.ticks;
        cmd.qty = static_cast<me::Qty>(md.qty.lots);
        process_me_command(cmd, handle_report);
      }
      break;
    }
    case MdUpdateKind::Trade: {
      // If a feed trade references an existing resting feed order, reduce/cancel it;
      // otherwise inject a synthetic aggressive IOC sweep to cross resting strategy orders!
      std::optional<me::OrderInfo> info = (backend_ == BookBackend::Pool)
                                              ? pool_engine_->book(instrument).find(feed_id)
                                              : map_engine_->book(instrument).find(feed_id);
      if (info.has_value()) {
        const me::Qty dec = static_cast<me::Qty>(md.qty.lots);
        if (dec >= info->leaves) {
          cmd.type = me::CommandType::Cancel;
        } else {
          cmd.type = me::CommandType::Reduce;
          cmd.qty = info->leaves - dec;
        }
        process_me_command(cmd, [](const me::Report&) {});
      } else if (md.price.ticks > 0 && md.qty.lots > 0) {
        const std::uint64_t sweep_id = kFeedOrderMask | (0x7000000000000000ULL + next_exec_id_);
        me::Command sweep{};
        sweep.type = me::CommandType::New;
        sweep.symbol = instrument;
        sweep.order_id = sweep_id;
        sweep.side = to_me_side(md.side);
        sweep.price = md.price.ticks;
        sweep.qty = static_cast<me::Qty>(md.qty.lots);
        me::Qty rem = 0;
        process_me_command(sweep, [&](const me::Report& r) {
          if (r.type == me::ReportType::Accepted) rem = r.leaves;
          if (r.type == me::ReportType::Trade) {
            rem = r.leaves;
            handle_report(r);
          }
        });
        if (rem > 0) {
          me::Command cxl{};
          cxl.type = me::CommandType::Cancel;
          cxl.symbol = instrument;
          cxl.order_id = sweep_id;
          process_me_command(cxl, [](const me::Report&) {});
        }
      }
      break;
    }
    case MdUpdateKind::SnapshotClear:
    case MdUpdateKind::BookClear:
      break;
  }
}

ErrorCode MatchingEngineAdapter::submit_order(InstrumentId instrument, const OrderIntent& intent,
                                              SimTimeNs now_ns,
                                              std::vector<ExecutionReport>& out_reports) {
  auto emit_reject = [&](RejectReason reason, ExchangeOrderId ex_id = 0) {
    ExecutionReport er{};
    er.type = (intent.action == OrderAction::Cancel) ? ExecReportType::CancelRejected
                                                     : ExecReportType::Rejected;
    er.client_order_id = intent.client_order_id;
    er.token = intent.token;
    er.exchange_order_id = ex_id;
    er.side = intent.side;
    er.exec_price = intent.limit_price;
    er.requested_qty = intent.qty;
    er.reject_reason = reason;
    er.exchange_time_ns = now_ns;
    out_reports.push_back(er);
  };

  if (instrument >= instruments_.size()) {
    emit_reject(RejectReason::UnknownSymbol);
    return ErrorCode::InvalidArgument;
  }

  if (intent.action == OrderAction::New) {
    if (token_to_exchange_id_.find(intent.token) != token_to_exchange_id_.end()) {
      emit_reject(RejectReason::DuplicateToken);
      return ErrorCode::DuplicateToken;
    }
    if (intent.qty.lots == 0 || intent.qty.lots > instruments_[instrument].max_qty_lots) {
      emit_reject(RejectReason::InvalidQty);
      return ErrorCode::InvalidArgument;
    }

    const TopOfBook tob = top_of_book(instrument);
    Price submit_price = intent.limit_price;
    if (intent.order_type == OrderType::Market) {
      submit_price = (intent.side == Side::Buy)
                         ? Price{instruments_[instrument].max_price_ticks}
                         : Price{1};
    } else if (submit_price.ticks <= 0 ||
               submit_price.ticks > instruments_[instrument].max_price_ticks) {
      emit_reject(RejectReason::InvalidPrice);
      return ErrorCode::InvalidArgument;
    }

    // Post-only check: reject immediately if order would cross resting opposite book.
    if (intent.post_only && intent.order_type == OrderType::Limit) {
      const bool would_cross =
          (intent.side == Side::Buy && tob.ask.has_value() &&
           submit_price.ticks >= tob.ask->price.ticks) ||
          (intent.side == Side::Sell && tob.bid.has_value() &&
           submit_price.ticks <= tob.bid->price.ticks);
      if (would_cross) {
        emit_reject(RejectReason::PostOnlyWouldCross);
        return ErrorCode::Ok;
      }
    }

    // FOK pre-check: ensure available executable liquidity covers full quantity.
    if (intent.tif == TimeInForce::FOK) {
      const auto opp_levels = depth(instrument, opposite(intent.side), 64);
      std::uint64_t avail = 0;
      for (const auto& lv : opp_levels) {
        const bool marketable =
            (intent.order_type == OrderType::Market) ||
            (intent.side == Side::Buy && lv.price.ticks <= submit_price.ticks) ||
            (intent.side == Side::Sell && lv.price.ticks >= submit_price.ticks);
        if (!marketable) break;
        avail += lv.qty.lots;
        if (avail >= intent.qty.lots) break;
      }
      if (avail < intent.qty.lots) {
        emit_reject(RejectReason::ExchangeRejected);
        return ErrorCode::Ok;
      }
    }

    const ExchangeOrderId ex_id = next_exchange_order_id_++;
    me::Command cmd{};
    cmd.type = me::CommandType::New;
    cmd.symbol = instrument;
    cmd.order_id = ex_id;
    cmd.side = to_me_side(intent.side);
    cmd.price = submit_price.ticks;
    cmd.qty = static_cast<me::Qty>(intent.qty.lots);

    TrackedStrategyOrder tracked{};
    tracked.instrument = instrument;
    tracked.client_order_id = intent.client_order_id;
    tracked.token = intent.token;
    tracked.exchange_id = ex_id;
    tracked.side = intent.side;
    tracked.price = submit_price;
    tracked.requested_qty = intent.qty;
    tracked.leaves = intent.qty;

    bool accepted = false;
    process_me_command(cmd, [&](const me::Report& r) {
      if (r.type == me::ReportType::Rejected) {
        emit_reject(map_me_reject(r.reason), ex_id);
        return;
      }
      if (r.type == me::ReportType::Accepted) {
        accepted = true;
        tracked.live = (r.leaves > 0);
        ExecutionReport ack{};
        ack.type = ExecReportType::Accepted;
        ack.client_order_id = intent.client_order_id;
        ack.token = intent.token;
        ack.exchange_order_id = ex_id;
        ack.side = intent.side;
        ack.exec_price = submit_price;
        ack.requested_qty = intent.qty;
        ack.cum_filled_qty = Quantity{0};
        ack.leaves_qty = intent.qty;
        ack.exchange_time_ns = now_ns;
        out_reports.push_back(ack);
      } else if (r.type == me::ReportType::Trade) {
        const std::uint64_t fill_lots = static_cast<std::uint64_t>(r.qty);
        tracked.cum_filled.lots += fill_lots;
        tracked.leaves.lots = static_cast<std::uint64_t>(r.leaves);
        tracked.live = (tracked.leaves.lots > 0);

        ExecutionReport tr{};
        tr.type = tracked.live ? ExecReportType::PartiallyFilled : ExecReportType::Filled;
        tr.client_order_id = intent.client_order_id;
        tr.token = intent.token;
        tr.exchange_order_id = ex_id;
        tr.exec_id = next_exec_id_++;
        tr.side = intent.side;
        tr.exec_price = Price{r.price};
        tr.requested_qty = intent.qty;
        tr.fill_qty = Quantity{fill_lots};
        tr.cum_filled_qty = tracked.cum_filled;
        tr.leaves_qty = tracked.leaves;
        tr.is_maker = false;
        tr.fee_scaled = compute_fee_for_fill(instrument, tr.exec_price, tr.fill_qty, false);
        tr.exchange_time_ns = now_ns;
        out_reports.push_back(tr);
      }
    });

    if (accepted) {
      // Synthesize IOC / Market remainder cancellation (Project 01 is Limit-GTC natively).
      const bool cancel_rem =
          (intent.tif == TimeInForce::IOC || intent.order_type == OrderType::Market) &&
          tracked.leaves.lots > 0;
      if (cancel_rem) {
        me::Command cxl{};
        cxl.type = me::CommandType::Cancel;
        cxl.symbol = instrument;
        cxl.order_id = ex_id;
        process_me_command(cxl, [&](const me::Report& r) {
          if (r.type == me::ReportType::Canceled) {
            tracked.leaves.lots = 0;
            tracked.live = false;
            ExecutionReport c{};
            c.type = ExecReportType::Canceled;
            c.client_order_id = intent.client_order_id;
            c.token = intent.token;
            c.exchange_order_id = ex_id;
            c.side = intent.side;
            c.exec_price = submit_price;
            c.requested_qty = intent.qty;
            c.cum_filled_qty = tracked.cum_filled;
            c.leaves_qty = Quantity{0};
            c.exchange_time_ns = now_ns;
            out_reports.push_back(c);
          }
        });
      }
      token_to_exchange_id_[intent.token] = ex_id;
      by_exchange_id_[ex_id] = tracked;
    }
    return ErrorCode::Ok;
  }

  if (intent.action == OrderAction::Cancel) {
    const ClientToken lookup_tok = intent.orig_token.empty() ? intent.token : intent.orig_token;
    auto tit = token_to_exchange_id_.find(lookup_tok);
    if (tit == token_to_exchange_id_.end()) {
      emit_reject(RejectReason::UnknownOrder);
      return ErrorCode::UnknownOrder;
    }
    auto oit = by_exchange_id_.find(tit->second);
    if (oit == by_exchange_id_.end() || !oit->second.live) {
      emit_reject(RejectReason::UnknownOrder, tit->second);
      return ErrorCode::UnknownOrder;
    }
    TrackedStrategyOrder& ord = oit->second;
    me::Command cmd{};
    cmd.symbol = ord.instrument;
    cmd.order_id = ord.exchange_id;
    if (intent.remaining_qty_target.lots == 0 ||
        intent.remaining_qty_target.lots >= ord.leaves.lots) {
      cmd.type = me::CommandType::Cancel;
    } else {
      cmd.type = me::CommandType::Reduce;
      cmd.qty = static_cast<me::Qty>(intent.remaining_qty_target.lots);
    }
    process_me_command(cmd, [&](const me::Report& r) {
      if (r.type == me::ReportType::Canceled) {
        ord.leaves.lots = 0;
        ord.live = false;
        ExecutionReport c{};
        c.type = ExecReportType::Canceled;
        c.client_order_id = ord.client_order_id;
        c.token = ord.token;
        c.exchange_order_id = ord.exchange_id;
        c.side = ord.side;
        c.exec_price = ord.price;
        c.requested_qty = ord.requested_qty;
        c.cum_filled_qty = ord.cum_filled;
        c.leaves_qty = Quantity{0};
        c.exchange_time_ns = now_ns;
        out_reports.push_back(c);
      } else if (r.type == me::ReportType::Reduced) {
        ord.leaves.lots = static_cast<std::uint64_t>(r.leaves);
        ord.live = (ord.leaves.lots > 0);
        ExecutionReport c{};
        c.type = ExecReportType::Canceled;
        c.client_order_id = ord.client_order_id;
        c.token = ord.token;
        c.exchange_order_id = ord.exchange_id;
        c.side = ord.side;
        c.exec_price = ord.price;
        c.requested_qty = ord.requested_qty;
        c.cum_filled_qty = ord.cum_filled;
        c.leaves_qty = ord.leaves;
        c.exchange_time_ns = now_ns;
        out_reports.push_back(c);
      } else if (r.type == me::ReportType::Rejected) {
        emit_reject(map_me_reject(r.reason), ord.exchange_id);
      }
    });
    return ErrorCode::Ok;
  }

  if (intent.action == OrderAction::Replace) {
    auto tit = token_to_exchange_id_.find(intent.orig_token);
    if (tit == token_to_exchange_id_.end()) {
      emit_reject(RejectReason::UnknownOrder);
      return ErrorCode::UnknownOrder;
    }
    auto oit = by_exchange_id_.find(tit->second);
    if (oit == by_exchange_id_.end() || !oit->second.live) {
      emit_reject(RejectReason::UnknownOrder, tit->second);
      return ErrorCode::UnknownOrder;
    }
    TrackedStrategyOrder& old_ord = oit->second;
    const ExchangeOrderId new_ex_id = next_exchange_order_id_++;
    const std::uint64_t new_open =
        (intent.qty.lots > old_ord.cum_filled.lots)
            ? (intent.qty.lots - old_ord.cum_filled.lots)
            : 0;
    if (new_open == 0) {
      emit_reject(RejectReason::InvalidQty, old_ord.exchange_id);
      return ErrorCode::InvalidArgument;
    }

    me::Command cmd{};
    cmd.type = me::CommandType::Replace;
    cmd.symbol = old_ord.instrument;
    cmd.order_id = old_ord.exchange_id;
    cmd.new_order_id = new_ex_id;
    cmd.side = to_me_side(old_ord.side);
    cmd.price = intent.limit_price.ticks;
    cmd.qty = static_cast<me::Qty>(new_open);

    TrackedStrategyOrder new_ord = old_ord;
    new_ord.client_order_id = intent.client_order_id;
    new_ord.token = intent.token;
    new_ord.exchange_id = new_ex_id;
    new_ord.price = intent.limit_price;
    new_ord.requested_qty = intent.qty;
    new_ord.leaves = Quantity{new_open};

    bool replaced = false;
    process_me_command(cmd, [&](const me::Report& r) {
      if (r.type == me::ReportType::Rejected) {
        emit_reject(map_me_reject(r.reason), old_ord.exchange_id);
      } else if (r.type == me::ReportType::Replaced) {
        replaced = true;
        old_ord.live = false;
        old_ord.leaves.lots = 0;
        new_ord.live = (r.leaves > 0);
        ExecutionReport rep{};
        rep.type = ExecReportType::Replaced;
        rep.client_order_id = new_ord.client_order_id;
        rep.token = new_ord.token;
        rep.exchange_order_id = new_ex_id;
        rep.side = new_ord.side;
        rep.exec_price = new_ord.price;
        rep.requested_qty = new_ord.requested_qty;
        rep.cum_filled_qty = new_ord.cum_filled;
        rep.leaves_qty = new_ord.leaves;
        rep.exchange_time_ns = now_ns;
        out_reports.push_back(rep);
      } else if (r.type == me::ReportType::Trade) {
        const std::uint64_t fill_lots = static_cast<std::uint64_t>(r.qty);
        new_ord.cum_filled.lots += fill_lots;
        new_ord.leaves.lots = static_cast<std::uint64_t>(r.leaves);
        new_ord.live = (new_ord.leaves.lots > 0);
        ExecutionReport tr{};
        tr.type = new_ord.live ? ExecReportType::PartiallyFilled : ExecReportType::Filled;
        tr.client_order_id = new_ord.client_order_id;
        tr.token = new_ord.token;
        tr.exchange_order_id = new_ex_id;
        tr.exec_id = next_exec_id_++;
        tr.side = new_ord.side;
        tr.exec_price = Price{r.price};
        tr.requested_qty = new_ord.requested_qty;
        tr.fill_qty = Quantity{fill_lots};
        tr.cum_filled_qty = new_ord.cum_filled;
        tr.leaves_qty = new_ord.leaves;
        tr.is_maker = false;
        tr.fee_scaled = compute_fee_for_fill(instrument, tr.exec_price, tr.fill_qty, false);
        tr.exchange_time_ns = now_ns;
        out_reports.push_back(tr);
      }
    });
    if (replaced) {
      token_to_exchange_id_[intent.token] = new_ex_id;
      by_exchange_id_[new_ex_id] = new_ord;
    }
    return ErrorCode::Ok;
  }

  emit_reject(RejectReason::ExchangeRejected);
  return ErrorCode::InvalidArgument;
}

TopOfBook MatchingEngineAdapter::top_of_book(InstrumentId instrument) const {
  TopOfBook out{};
  if (instrument >= instruments_.size()) return out;
  const me::TopOfBook tob = (backend_ == BookBackend::Pool)
                                ? pool_engine_->top_of_book(instrument)
                                : map_engine_->top_of_book(instrument);
  if (tob.bid.has_value()) {
    out.bid = LevelView{Price{tob.bid->price}, Quantity{static_cast<std::uint64_t>(tob.bid->qty)},
                        tob.bid->orders};
  }
  if (tob.ask.has_value()) {
    out.ask = LevelView{Price{tob.ask->price}, Quantity{static_cast<std::uint64_t>(tob.ask->qty)},
                        tob.ask->orders};
  }
  return out;
}

std::vector<LevelView> MatchingEngineAdapter::depth(InstrumentId instrument, Side side,
                                                    std::size_t max_levels) const {
  std::vector<LevelView> res;
  if (instrument >= instruments_.size()) return res;
  const auto levels = (backend_ == BookBackend::Pool)
                          ? pool_engine_->depth(instrument, to_me_side(side), max_levels)
                          : map_engine_->depth(instrument, to_me_side(side), max_levels);
  res.reserve(levels.size());
  for (const auto& lv : levels) {
    res.push_back(LevelView{Price{lv.price}, Quantity{static_cast<std::uint64_t>(lv.qty)},
                            lv.orders});
  }
  return res;
}

std::uint64_t MatchingEngineAdapter::book_hash(InstrumentId instrument) const {
  std::uint64_t h = 14695981039346656037ULL;
  hash_u64(h, instrument);
  for (Side s : {Side::Buy, Side::Sell}) {
    hash_u64(h, static_cast<std::uint64_t>(s));
    const auto lvls = depth(instrument, s, 64);
    hash_u64(h, lvls.size());
    for (const auto& lv : lvls) {
      hash_u64(h, static_cast<std::uint64_t>(lv.price.ticks));
      hash_u64(h, lv.qty.lots);
      hash_u64(h, lv.order_count);
    }
  }
  return h;
}

bool MatchingEngineAdapter::check_invariants(std::string* why) const {
  for (std::size_t i = 0; i < instruments_.size(); ++i) {
    const auto sym = static_cast<me::SymbolId>(i);
    const bool ok = (backend_ == BookBackend::Pool)
                        ? pool_engine_->book(sym).check_invariants(why)
                        : map_engine_->book(sym).check_invariants(why);
    if (!ok) return false;
  }
  return true;
}

std::uint64_t MatchingEngineAdapter::total_commands() const noexcept {
  return (backend_ == BookBackend::Pool) ? pool_engine_->stats().commands
                                         : map_engine_->stats().commands;
}

std::uint64_t MatchingEngineAdapter::total_trades() const noexcept {
  return (backend_ == BookBackend::Pool) ? pool_engine_->stats().trades
                                         : map_engine_->stats().trades;
}

// ---- OuchMatchingBridge (Project 03 -> Project 01 seam) ----

OuchMatchingBridge::OuchMatchingBridge(std::vector<InstrumentMeta> instruments,
                                       std::uint32_t max_orders_per_book)
    : instruments_(std::move(instruments)) {
  me::EngineConfig cfg;
  cfg.max_orders_per_book = max_orders_per_book;
  for (std::size_t i = 0; i < instruments_.size(); ++i) {
    symbol_to_id_[instruments_[i].symbol_str()] = static_cast<std::uint32_t>(i);
    me::SymbolConfig sc;
    sc.tick_size = 1;
    sc.lot_size = 1;
    sc.max_price = static_cast<me::Price>(ouch::kMaxLimitPrice);
    sc.max_qty = static_cast<me::Qty>(ouch::kMaxShares);
    cfg.symbols.push_back(sc);
  }
  if (cfg.symbols.empty()) {
    me::SymbolConfig sc;
    sc.max_price = static_cast<me::Price>(ouch::kMaxLimitPrice);
    sc.max_qty = static_cast<me::Qty>(ouch::kMaxShares);
    cfg.symbols.push_back(sc);
  }
  engine_ = std::make_unique<me::Engine<me::PoolBook>>(cfg);
}

std::optional<std::uint32_t> OuchMatchingBridge::resolve_symbol(const ouch::Symbol& sym) const {
  const auto it = symbol_to_id_.find(sym.str());
  if (it == symbol_to_id_.end()) return std::nullopt;
  return it->second;
}

void OuchMatchingBridge::add(std::uint64_t ref, const ouch::Symbol& symbol, bool buy,
                             std::uint32_t price, std::uint32_t shares, bool ioc,
                             ouch::sim::AddResult& result) {
  result = ouch::sim::AddResult{};
  const auto sym_id = resolve_symbol(symbol);
  if (!sym_id.has_value() || shares == 0) {
    result.canceled = shares;
    return;
  }

  const bool is_market = (price == ouch::kMarketPrice);
  const me::Price submit_px = is_market ? (buy ? static_cast<me::Price>(ouch::kMaxLimitPrice) : 1)
                                        : static_cast<me::Price>(price);
  me::Command cmd{};
  cmd.type = me::CommandType::New;
  cmd.symbol = *sym_id;
  cmd.order_id = ref;
  cmd.side = buy ? me::Side::Buy : me::Side::Sell;
  cmd.price = submit_px;
  cmd.qty = static_cast<me::Qty>(shares);

  std::uint32_t rem = shares;
  bool accepted = false;
  engine_->process(cmd, [&](const me::Report& r) {
    if (r.type == me::ReportType::Accepted) {
      accepted = true;
      rem = static_cast<std::uint32_t>(r.leaves);
    } else if (r.type == me::ReportType::Trade) {
      ouch::sim::Fill f{};
      f.resting_ref = r.other_id;
      f.shares = static_cast<std::uint32_t>(r.qty);
      f.price = static_cast<std::uint32_t>(r.price);
      f.match_number = next_match_num_++;
      result.fills.push_back(f);
      rem = static_cast<std::uint32_t>(r.leaves);
    }
  });

  if (!accepted) {
    result.canceled = shares;
    return;
  }

  if ((ioc || is_market) && rem > 0) {
    me::Command cxl{};
    cxl.type = me::CommandType::Cancel;
    cxl.symbol = *sym_id;
    cxl.order_id = ref;
    engine_->process(cxl, [](const me::Report&) {});
    result.canceled = rem;
    result.resting = 0;
  } else {
    result.resting = rem;
    result.canceled = 0;
    if (rem > 0) {
      ref_to_symbol_[ref] = *sym_id;
    }
  }
}

std::uint32_t OuchMatchingBridge::reduce(std::uint64_t ref, std::uint32_t new_open) {
  const auto it = ref_to_symbol_.find(ref);
  if (it == ref_to_symbol_.end()) return 0;
  const std::uint32_t sym_id = it->second;
  const auto info = engine_->book(sym_id).find(ref);
  if (!info.has_value()) {
    ref_to_symbol_.erase(it);
    return 0;
  }
  const std::uint32_t current = static_cast<std::uint32_t>(info->leaves);
  if (new_open >= current) return 0;

  std::uint32_t removed = 0;
  me::Command cmd{};
  cmd.symbol = sym_id;
  cmd.order_id = ref;
  if (new_open == 0) {
    cmd.type = me::CommandType::Cancel;
    engine_->process(cmd, [&](const me::Report& r) {
      if (r.type == me::ReportType::Canceled) {
        removed = static_cast<std::uint32_t>(r.qty);
      }
    });
    ref_to_symbol_.erase(it);
  } else {
    cmd.type = me::CommandType::Reduce;
    cmd.qty = static_cast<me::Qty>(new_open);
    engine_->process(cmd, [&](const me::Report& r) {
      if (r.type == me::ReportType::Reduced) {
        removed = static_cast<std::uint32_t>(r.qty);
      }
    });
  }
  return removed;
}

std::uint32_t OuchMatchingBridge::open_shares(std::uint64_t ref) const {
  const auto it = ref_to_symbol_.find(ref);
  if (it == ref_to_symbol_.end()) return 0;
  const auto info = engine_->book(it->second).find(ref);
  return info.has_value() ? static_cast<std::uint32_t>(info->leaves) : 0;
}

}  // namespace hft::adapters
